// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// E1 host tests: the encoder geometry, the VPU35 size table, the arena layout, the property script in Linux iris_venc's order,
// the Annex B scan, the test pattern, and the HFI transaction against a scripted encoder firmware peer (success, session error,
// missing LAST, missing CLOSE ack, system error, export truncation, a malformed bitstream, a wrong buffer address, SMMU fault,
// watchdog).
//   enc_host                       run the tests
//   enc_host --pattern FILE W H N  write N frames of the test pattern as packed NV12 (W x H) for tests/enc_stream_check.py
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include "VideoBringup.h"
#include "VideoEncodeSession.h"
using namespace pianovideo;
using namespace pianovideo::enc;
static unsigned failures = 0;
#define CHECK(x)                                                                                                                           \
  do {                                                                                                                                     \
    if (!(x)) {                                                                                                                            \
      std::printf("FAIL line %u: %s\n", __LINE__, #x);                                                                                     \
      ++failures;                                                                                                                          \
    }                                                                                                                                      \
  } while (0)

struct TestHw {
  uint64_t clock = 0;
  std::string log;
  void barrier() {}
  void stage(int, const char *) {}
  void sleepMs(unsigned n) { clock += uint64_t(n) * 1000; }
  uint64_t nowUs() { return clock += 10; }
  void note(const char *fmt, ...)
  {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log += "PianoVideo: ";
    log += b;
    log += '\n';
    if (std::getenv("VIDEO_ENC_ECHO")) std::printf("PianoVideo: %s\n", b);
  }
};
struct TestOps {
  bool fault = false, watchdog = false;
  uint32_t rd(uint64_t pa) { return pa == kIris + kWrapperIntrStatus ? (watchdog ? 8 : 0) : (fault && (pa & 4095) == vsmmu::kFsr ? 2 : 0); }
};
struct Sent {
  uint32_t type, flags, port, payload, words, v0, v1;
};
// The encoder firmware as iris_hfi_gen2_response.c expects it to answer: no OPEN/START acks, RAW back after a frame, BITSTREAM
// with the picture type before it, DRAIN ack + an empty LAST bitstream, STOP and CLOSE acks.
struct Peer {
  TestHw hw;
  TestOps ops;
  vsmmu::Plan plan;
  hfi::Mem mem;
  hfi::Ids ids;
  uint32_t tx[hfi::kBufferWords] = {}, rx[hfi::kBufferWords] = {};
  struct { volatile uint8_t *arena = nullptr; } dma;
  struct { uint32_t fsr = 0; } rep;
  unsigned watchdogSamples = 0, watchdogSeen = 0, rxEvents = 0;
  uint32_t activeSid = 0;
  std::vector<uint32_t> uc;
  std::vector<uint8_t> arena;
  std::vector<Sent> sent;
  std::vector<std::vector<uint32_t>> bitstreamQueue;   // queued BITSTREAM buffer packets (v[0..15])
  std::vector<std::vector<uint32_t>> rawPending;       // raw frames waiting for an output buffer
  Geometry g;
  unsigned frame = 0, patternOk = 0, patternBad = 0;
  bool opened = false, sessionErrorOnProp = false, dropLast = false, dropCloseAck = false, systemError = false, wrongIova = false,
       badBitstream = false, bigFrames = false;
  bool faultOnRaw = false, watchdogOnStop = false;
  uint32_t sessionErrorProp = 0;
  Peer() : uc(kUcBytes / 4), arena(session::kAvcArenaBytes, 0)
  {
    mem.uc = uc.data();
    dma.arena = arena.data();
    hfi::initQueues(mem);
    plan.bank = 3;
    plan.pages = 128;
  }
  static std::vector<uint32_t> packet(uint32_t type, uint32_t flags, uint32_t port, uint32_t payload, const std::vector<uint32_t> &v = {})
  {
    std::vector<uint32_t> p = {uint32_t(32 + 4 * v.size()), type, flags, payload, port, 0xeeee, 0, 0};
    p.insert(p.end(), v.begin(), v.end());
    return p;
  }
  void reply(std::vector<std::vector<uint32_t>> packets, uint32_t sid = 0)
  {
    std::vector<uint32_t> b = {32, sid ? sid : activeSid, 0xdddd, 0, 0, 0, 0, uint32_t(packets.size())};
    for (const auto &p : packets) b.insert(b.end(), p.begin(), p.end());
    b[0] = uint32_t(4 * b.size());
    CHECK(hfi::write(mem, hfi::kMsgq, b.data(), b[0], [] {}) == 0);
  }
  void packetTrace(const char *, unsigned, const uint32_t *, unsigned) {}
  void drainDebug() {}
  // One coded frame into the first queued bitstream buffer: SPS+PPS+IDR for frame 0 (header joined), a P slice after.
  bool emit(const std::vector<uint32_t> &raw)
  {
    if (bitstreamQueue.empty()) return false;
    std::vector<uint32_t> out = bitstreamQueue.front();
    bitstreamQueue.erase(bitstreamQueue.begin());
    std::vector<uint8_t> nal;
    auto put = [&](uint8_t type, unsigned n) {
      const uint8_t sc[4] = {0, 0, 0, 1};
      nal.insert(nal.end(), sc, sc + 4);
      nal.push_back(uint8_t(0x60 | type));
      for (unsigned i = 0; i < n; ++i) nal.push_back(uint8_t(0x80 | (i & 0x7f)));
    };
    const bool idr = raw[8] == 0;
    if (badBitstream) nal = {0x12, 0x34, 0x56};
    else if (idr) { put(7, 12); put(8, 4); put(5, bigFrames ? 300000 : 3000); }
    else put(1, bigFrames ? 200000 : 800);
    CHECK(nal.size() <= out[5]);
    const uint32_t off = out[2] - session::kArenaBase;
    std::memcpy(arena.data() + off, nal.data(), nal.size());
    out[2] = wrongIova ? out[2] + 4096 : out[2];
    out[6] = 0;
    out[7] = uint32_t(nal.size());
    out[8] = raw[8];
    out[9] = raw[9];
    reply({packet(PropPictureType, 2, PortBitstream, 1, {idr ? PictureIdr : PictureP}),
           packet(Buffers, 1, PortBitstream, 5, out), packet(Buffers, 1, PortRaw, 5, raw)});
    return true;
  }
  int send(unsigned bytes, const char *)
  {
    CHECK(hfi::write(mem, hfi::kCmdq, tx, bytes, [] {}) == 0);
    uint32_t data[hfi::kBufferWords], size = 0;
    CHECK(hfi::read(mem, hfi::kCmdq, data, sizeof data, &size, [] {}) == 0);
    CHECK(size == bytes && data[7] == 1 && data[0] == bytes);
    const uint32_t *p = data + 8;
    const uint32_t type = p[1], flags = p[2], port = p[4], payload = p[3], words = (p[0] - 32) / 4;
    sent.push_back({type, flags, port, payload, words, words ? p[8] : 0, words > 1 ? p[9] : 0});
    if (systemError && sent.size() == 6) { reply({packet(0x05000001, 8, 0, 0)}, 0); return 0; }
    if (type == Open) {
      CHECK(!data[1] && port == 0 && flags == 3 && payload == PayloadU32 && p[8]);
      activeSid = p[8];
      opened = true;
      return 0;
    }
    CHECK(opened && data[1] == activeSid);
    if (type > 0x03000000 && type < 0x03ffffff) {
      if (sessionErrorOnProp && type == sessionErrorProp) reply({packet(type, 4 | 1, port, 0)});
      return 0;
    }
    std::vector<uint32_t> v(p + 8, p + 8 + words);
    if (type == Buffers) {
      CHECK(words == 16 && flags == 1 && payload == 5 && v[10] == kHostNonSecure);
      if (v[0] == BufBitstream) {
        CHECK(port == PortBitstream && !v[7]);
        bitstreamQueue.push_back(v);
        if (!rawPending.empty() && emit(rawPending.front())) rawPending.erase(rawPending.begin());
      } else if (v[0] == BufRaw) {
        CHECK(port == PortRaw && v[7] == g.rawBytes);
        // The frame really is the pattern of its timestamp's frame number (active pixels and the padding).
        const uint32_t f = v[8] / 33333;
        const uint8_t *base = arena.data() + (v[2] - session::kArenaBase);
        bool same = true;
        pattern(g, f, [&](uint32_t at, uint8_t want) { same &= base[at] == want; });
        (same ? patternOk : patternBad)++;
        ++frame;
        if (!emit(v)) rawPending.push_back(v);
        if (faultOnRaw) ops.fault = true;
      } else {
        CHECK(v[7] == 0);   // internal buffers carry no data
      }
      return 0;
    }
    if (type == Drain) {
      CHECK(port == PortRaw && flags == 7);
      std::vector<std::vector<uint32_t>> r = {packet(Drain, 1, PortRaw, 0)};
      if (!dropLast && !bitstreamQueue.empty()) {
        std::vector<uint32_t> out = bitstreamQueue.front();
        bitstreamQueue.erase(bitstreamQueue.begin());
        out[7] = 0;
        out[10] = kFlagLast;
        r.push_back(packet(Buffers, 1, PortBitstream, 5, out));
      }
      reply(r);
      return 0;
    }
    if (type == Stop) {
      if (watchdogOnStop) ops.watchdog = true;
      // STOP hands back what the port still holds (the output buffers), then acknowledges.
      std::vector<std::vector<uint32_t>> r;
      if (port == PortBitstream)
        for (auto &out : bitstreamQueue) {
          out[7] = 0;
          r.push_back(packet(Buffers, 1, PortBitstream, 5, out));
        }
      bitstreamQueue.clear();
      r.push_back(packet(Stop, 1, port, 0));
      reply(r);
      return 0;
    }
    if (type == Close) {
      if (!dropCloseAck) reply({packet(Close, 1, 0, 0)});
      return 0;
    }
    return 0;   // START: no mandatory ack
  }
};

static std::vector<uint32_t> types(const Peer &p)
{
  std::vector<uint32_t> t;
  for (const auto &s : p.sent) t.push_back(s.type);
  return t;
}
static unsigned indexOf(const Peer &p, uint32_t type, uint32_t port, unsigned from = 0)
{
  for (unsigned i = from; i < p.sent.size(); ++i)
    if (p.sent[i].type == type && p.sent[i].port == port) return i;
  return ~0u;
}
static unsigned bufferIndex(const Peer &p, uint32_t bufType)
{
  for (unsigned i = 0; i < p.sent.size(); ++i)
    if (p.sent[i].type == Buffers && p.sent[i].v0 == bufType) return i;
  return ~0u;
}

struct Run {
  Peer peer;
  session::Layout layout;
  std::vector<uint8_t> stream;
  EncodeSelfTest<Peer> t;
  int rc = 0;
  Run(uint32_t w, uint32_t h, uint32_t cap = 256 * 1024) : stream(cap), t(peer, layout)
  {
    CHECK(t.g.configure(w, h));
    peer.g = t.g;
    CHECK(buildLayout(layout, t.g));
    t.stream = stream.data();
    t.streamCap = cap;
  }
  int go() { return rc = t.run(); }
};

static void bootGateTests()
{
  CHECK(!bootEnabled("") && !bootEnabled("piano-video=6 piano-video-enc=0"));
  CHECK(bootEnabled("piano-video=6\tpiano-video-enc=1 piano-video-enc-delay=120"));
  CHECK(!bootEnabled("piano-video-enc") && !bootEnabled("piano-video-enc="));
  CHECK(!bootEnabled("piano-video-enc=2") && !bootEnabled("piano-video-enc=10"));
  CHECK(!bootEnabled("xpiano-video-enc=1 piano-video-enc-delay=1"));
  CHECK(!bootEnabled("piano-video-enc=0 piano-video-enc=1"));
  CHECK(!bootEnabled("piano-video-enc=1 piano-video-enc=0"));
  CHECK(!bootEnabled("piano-video-enc=1 piano-video-enc=1"));
}

static void geometryTests()
{
  Geometry g;
  CHECK(g.configure(640, 480));
  CHECK(g.stride == 640 && g.scanlines == 480 && g.uvScanlines == 240 && g.uvOffset == 307200);
  CHECK(g.rawBytes == 462848);         // ALIGN(640*480 + 640*240, 4096)
  CHECK(g.bitstreamBytes == 921600);   // 640*480*3, at most 720p: not reduced
  CHECK(g.codedWidth == 640 && g.codedHeight == 480 && g.codedCrop() == 0);
  CHECK(g.configure(1920, 1080));
  CHECK(g.stride == 1920 && g.scanlines == 1088 && g.uvScanlines == 544 && g.codedHeight == 1088 && g.codedCrop() == 8);
  CHECK(g.bitstreamBytes == 1568768);   // ALIGN((1920*1088*3) >> 2 = 1566720, 4096)
  CHECK(g.configure(320, 240) && g.stride == 384 && g.scanlines == 256 && g.uvScanlines == 128);
  CHECK(g.configure(1280, 720) && g.scanlines == 736 && g.codedHeight == 720);
  CHECK(!g.configure(800, 600) && !g.configure(0, 0) && !g.configure(641, 480));
  // The size table holds the four supported geometries.
  CHECK(kInternalCount == 4 && kInternal[1].bin == 23787008 && kInternal[1].vpss == 0 && kInternal[3].dpb == 12636160);
  // Layouts: no overlap, guards between buffers, inside the AVC arena, every byte of every buffer mapped.
  for (const auto &row : kInternal) {
    session::Layout l;
    CHECK(g.configure(row.width, row.height) && buildLayout(l, g));
    CHECK(l.used == 9u + (row.vpss ? 1u : 0u) && l.bytes <= session::kAvcArenaBytes && l.arenaBytes() == session::kAvcArenaBytes);
    for (unsigned i = 0; i < l.used; ++i) {
      const auto &b = l.buf[i];
      CHECK(!(b.off & 16383) && b.capacity == b.size);
      for (uint32_t at = 0; at < b.capacity; at += 4096) CHECK(l.mapped((b.off + at) / 4096));
      CHECK(!l.mapped((b.off + ((b.capacity + 16383) & ~16383u)) / 4096));   // the guard after it
      for (unsigned j = i + 1; j < l.used; ++j) CHECK(l.buf[j].off >= b.off + b.capacity);
    }
    CHECK(l.find(BufRaw, 0) && l.find(BufRaw, 1) && l.find(BufBitstream, 1) && l.find(BufDpb, 0) && !l.find(BufRaw, 2));
  }
  // The scripts: Linux's order and ports.
  CHECK(g.configure(640, 480));
  const Script in = inputConfig(g), out = outputConfig(g, Settings{}), caps = capabilities(Settings{});
  CHECK(in.n == 5 && in.p[0].id == PropColorFormat && in.p[1].id == PropRawResolution && in.p[1].v[0] == (640u << 16 | 480u));
  CHECK(in.p[3].id == PropStride && in.p[3].words == 2 && in.p[3].v[0] == (640u << 16 | 480u) && in.p[3].v[1] == (640u << 16 | 240u));
  CHECK(in.p[4].v[0] == 0x14000003);
  for (unsigned i = 0; i < in.n; ++i) CHECK(in.p[i].port == PortRaw);
  CHECK(out.n == 3 && out.p[2].id == PropFrameRate && out.p[2].payload == PayloadQ16 && out.p[2].v[0] == (30u << 16));
  for (unsigned i = 0; i < out.n; ++i) CHECK(out.p[i].port == PortBitstream);
  CHECK(caps.n == 14 && caps.p[0].id == PropProfile && caps.p[0].v[0] == 4 && caps.p[1].v[0] == 14 && caps.p[4].port == PortNone);
  CHECK(caps.p[6].id == PropTotalBitrate && caps.p[6].port == PortRaw && caps.p[13].id == PropRequestSyncFrame);
  Settings bad;
  bad.frames = 500;
  CHECK(admitted(Settings{}) && !admitted(bad));
}

static void residentArenaTests()
{
  TestHw hw;
  Bringup<TestHw> core(hw);
  core.avcWidth = 1920;
  core.avcHeight = 1088;
  CHECK(core.encGeometry.configure(640, 480));

  std::vector<uint64_t> words(vsmmu::kSessionTablePages * 512), uc(vsmmu::kUcPages);
  std::vector<uint64_t> pages(session::kAvcArenaBytes / 4096);
  for (unsigned i = 0; i < uc.size(); ++i) uc[i] = kXnuDramBase + i * 4096ull;
  vsmmu::Tables tables{words.data(), kXnuDramBase + 0x2000000, vsmmu::kSessionTablePages};
  for (uint64_t base : {kXnuDramBase + 0x10000000, kExtraArenaBase}) {
    for (unsigned i = 0; i < pages.size(); ++i) pages[i] = base + i * 4096ull;
    CHECK(vsmmu::build(tables, uc.data(), unsigned(uc.size()), kXnuDramBase + 0x3000000));
    core.hevc = false;
    core.bitdepth = 8;
    CHECK(core.prepareLayout() && !core.layout.hevc && core.layout.used > 10);
    CHECK(session::buildArena(tables, core.layout, pages.data(), session::kAvcArenaBytes));
    const auto avcLeaves = words;

    core.encoder = true;
    CHECK(core.prepareLayout() && core.layout.used == 9);
    CHECK(session::buildArena(tables, core.layout, pages.data(), session::kAvcArenaBytes));
    CHECK(vsmmu::walk(tables, kIovaBase) == uc[0]);
    CHECK(words != avcLeaves);

    core.encoder = false;
    core.hevc = true;
    core.bitdepth = 10;
    CHECK(core.prepareLayout() && core.layout.hevc && core.layout.arenaBytes() == 188 * 1024 * 1024);
    CHECK(session::buildArena(tables, core.layout, pages.data(), session::kAvcArenaBytes));
    core.hevc = false;
    core.bitdepth = 8;
    CHECK(core.prepareLayout());
    CHECK(session::buildArena(tables, core.layout, pages.data(), session::kAvcArenaBytes));
    CHECK(words == avcLeaves);
  }
}

static void scanTests()
{
  const uint8_t s[] = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 9, 9, 9};
  bool bad = true;
  Nal n[8];
  CHECK(scanAnnexB(s, sizeof s, n, 8, &bad) == 3 && !bad);
  CHECK(n[0].type == 7 && n[0].size == 3 && n[1].type == 8 && n[1].offset == 10 && n[2].type == 5 && n[2].size == 4);
  const FrameNals f = classify(s, sizeof s);
  CHECK(f.sps == 1 && f.pps == 1 && f.idr == 1 && !f.slice && !f.bad);
  const uint8_t junk[] = {1, 2, 3};
  CHECK(scanAnnexB(junk, 3, n, 8, &bad) == 0 && bad);
  const uint8_t empty[] = {0, 0, 1, 0, 0, 1, 0x41};
  CHECK(scanAnnexB(empty, sizeof empty, n, 8, &bad) == 0 && bad);
  CHECK(scanAnnexB(s, 0, n, 8, &bad) == 0 && !bad);
}

static void patternTests()
{
  Geometry g;
  CHECK(g.configure(320, 240));
  std::vector<uint8_t> a(g.rawBytes, 0xee), b(g.rawBytes, 0xee);
  pattern(g, 0, [&](uint32_t at, uint8_t v) { a[at] = v; });
  pattern(g, 1, [&](uint32_t at, uint8_t v) { b[at] = v; });
  CHECK(a != b);
  CHECK(a[0] == 16 && a[320] == 0 && a[g.uvOffset] == 96 && a[g.uvOffset + 1] == 96);   // padding 0; Cb/Cr interleaved
  unsigned outside = 0;
  for (uint32_t at = g.uvOffset + g.stride * g.uvScanlines; at < g.rawBytes; ++at) outside += a[at] != 0xee;
  CHECK(outside == 0);   // nothing written past the UV plane
}

static void sessionTests()
{
  {  // success, 640x480 (no VPSS)
    Run r(640, 480);
    CHECK(r.go() == 0 && r.t.r.pass());
    const auto &p = r.peer;
    CHECK(p.patternOk == 10 && !p.patternBad);
    CHECK(r.t.r.etb == 10 && r.t.r.ebd == 10 && r.t.r.fbd == 10 && r.t.r.idr == 1 && r.t.r.sps == 1 && r.t.r.slices == 9);
    CHECK(r.t.r.drainAck && r.t.r.last && r.t.r.stopBitstream && r.t.r.stopRaw && r.t.r.closed && !r.t.r.truncated);
    CHECK(r.t.frames[0].picture == PictureIdr && r.t.frames[1].picture == PictureP);
    // The order: OPEN, CODEC, raw config (5), caps (14), START raw, bitstream config (3), caps (14), COMV count before COMV,
    // BIN..DPB, START bitstream, two empty bitstream buffers, then frames; DRAIN; STOP 1, STOP 2, CLOSE.
    const auto t = types(p);
    CHECK(t.size() > 50 && t[0] == Open && t[1] == PropCodec && p.sent[1].v0 == CodecEncodeAvc && p.sent[1].payload == PayloadEnum);
    CHECK(t[2] == PropColorFormat && t[6] == PropColorInfo && t[7] == PropProfile && t[20] == PropRequestSyncFrame);
    const unsigned startRaw = indexOf(p, Start, PortRaw), startBits = indexOf(p, Start, PortBitstream);
    CHECK(startRaw == 21 && t[22] == PropBitstreamResolution && t[25] == PropProfile && startBits != ~0u && startBits > startRaw);
    CHECK(bufferIndex(p, BufVpss) == ~0u);
    const unsigned bin = bufferIndex(p, BufBin), comv = bufferIndex(p, BufComv), dpb = bufferIndex(p, BufDpb);
    CHECK(bin > startRaw && bin < comv && comv < dpb && dpb < startBits && p.sent[comv - 1].type == PropComvCount);
    CHECK(bufferIndex(p, BufBitstream) == startBits + 1 && bufferIndex(p, BufRaw) > startBits);
    const unsigned drain = indexOf(p, Drain, PortRaw), stop1 = indexOf(p, Stop, PortBitstream), stop2 = indexOf(p, Stop, PortRaw);
    CHECK(drain != ~0u && drain < stop1 && stop1 < stop2 && t.back() == Close);
    // The export is the whole stream in output order and starts with the SPS.
    CHECK(r.t.r.streamBytes > 3000 && r.stream[4] == 0x67 && r.t.frames[1].offset == r.t.frames[0].bytes);
    CHECK(r.peer.hw.log.find("E1 result PASS") != std::string::npos);
  }
  {  // 320x240 queues VPSS on the raw port before START raw
    Run r(320, 240);
    CHECK(r.go() == 0 && r.t.r.pass());
    const unsigned vpss = bufferIndex(r.peer, BufVpss);
    CHECK(vpss != ~0u && r.peer.sent[vpss].port == PortRaw && vpss < indexOf(r.peer, Start, PortRaw));
  }
  {  // a session error on a property stops at that stage; cleanup still STOPs/CLOSEs
    Run r(640, 480);
    r.peer.sessionErrorOnProp = true;
    r.peer.sessionErrorProp = PropTotalBitrate;
    CHECK(r.go() != 0 && !r.t.r.pass() && r.t.r.failedStage <= InputStart && r.t.r.closed);
    CHECK(!r.t.r.etb && r.peer.hw.log.find("E1 result FAIL") != std::string::npos);
  }
  {  // no LAST after DRAIN: a bounded timeout, not a hang
    Run r(640, 480);
    r.peer.dropLast = true;
    CHECK(r.go() == kErrHfiTimeout && r.t.r.failedStage == Draining && r.t.r.fbd == 10 && !r.t.r.last);
  }
  {  // no CLOSE ack: the run reports it
    Run r(640, 480);
    r.peer.dropCloseAck = true;
    CHECK(r.go() != 0 && !r.t.r.closed && r.t.r.close == kErrHfiTimeout && !r.t.r.pass());
  }
  {  // a system error is fatal at once
    Run r(640, 480);
    r.peer.systemError = true;
    CHECK(r.go() == kErrSysError && !r.t.r.pass());
    CHECK(indexOf(r.peer, Stop, PortRaw) == ~0u && indexOf(r.peer, Close, PortNone) == ~0u);
  }
  {  // the export is bounded; a full buffer is flagged and frames past it are not parsed
    Run r(640, 480, 64 * 1024);
    r.peer.bigFrames = true;
    r.go();
    CHECK(r.t.r.truncated && r.t.r.streamBytes == 64 * 1024 && !r.t.r.pass());
  }
  {  // a bitstream that is not Annex B fails the NAL gate
    Run r(640, 480);
    r.peer.badBitstream = true;
    r.go();
    CHECK(r.t.r.badNal > 0 && !r.t.r.pass());
  }
  {  // a completion for an address we never queued is a protocol error
    Run r(640, 480);
    r.peer.wrongIova = true;
    CHECK(r.go() == kErrHfi && !r.t.r.pass());
  }
  {  // an SMMU fault during frame submission leaves the DMA state held for quarantine
    Run r(640, 480);
    r.peer.faultOnRaw = true;
    CHECK(r.go() == kErrFault && !r.t.r.pass());
    CHECK(r.t.r.etb == 1 && r.t.r.fbd == 0 && r.peer.sent.back().v0 == BufRaw);
    CHECK(indexOf(r.peer, Stop, PortBitstream) == ~0u && indexOf(r.peer, Close, PortNone) == ~0u);
  }
  {  // a watchdog during shutdown prevents the second STOP and CLOSE
    Run r(640, 480);
    r.peer.watchdogOnStop = true;
    CHECK(r.go() == kErrWatchdog && !r.t.r.pass());
    CHECK(r.peer.sent.back().type == Stop && r.peer.sent.back().port == PortBitstream);
    CHECK(!r.t.r.stopRaw && !r.t.r.closed);
  }
}

static int writePattern(const char *file, uint32_t w, uint32_t h, unsigned n)
{
  Geometry g;
  if (!g.configure(w, h)) return 2;
  FILE *f = std::fopen(file, "wb");
  if (!f) return 2;
  std::vector<uint8_t> nv12(g.rawBytes), packed(w * h * 3 / 2);
  for (unsigned k = 0; k < n; ++k) {
    pattern(g, k, [&](uint32_t at, uint8_t v) { nv12[at] = v; });
    for (uint32_t y = 0; y < h; ++y) std::memcpy(&packed[y * w], &nv12[y * g.stride], w);
    for (uint32_t y = 0; y < h / 2; ++y) std::memcpy(&packed[w * h + y * w], &nv12[g.uvOffset + y * g.stride], w);
    std::fwrite(packed.data(), 1, packed.size(), f);
  }
  std::fclose(f);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc == 6 && !std::strcmp(argv[1], "--pattern"))
    return writePattern(argv[2], uint32_t(std::atoi(argv[3])), uint32_t(std::atoi(argv[4])), unsigned(std::atoi(argv[5])));
  bootGateTests();
  geometryTests();
  residentArenaTests();
  scanTests();
  patternTests();
  sessionTests();
  if (failures) {
    std::printf("enc_host: %u FAILED\n", failures);
    return 1;
  }
  std::printf("enc_host: all E1 encoder checks passed\n");
  return 0;
}
