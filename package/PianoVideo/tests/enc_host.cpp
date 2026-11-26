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
#include <algorithm>
#include <charconv>
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
  int phase = 0;
  std::string log;
  void barrier() {}
  void stage(int s, const char *) { phase = s; }
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
  int stage;
};
enum class OutputReply { Normal, AtCapacity, Empty, WrongIova, BadCapacity, BadOffset, BadLength, Wraparound, BadTimestamp };
// The encoder firmware as iris_hfi_gen2_response.c expects it to answer: RAW back after a frame, BITSTREAM
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
  std::vector<uint32_t> openedSessions, closedSessions;
  std::vector<uint32_t> arp;
  std::vector<std::vector<uint32_t>> arpPackets;
  std::vector<std::vector<uint32_t>> internals;
  std::vector<std::vector<uint32_t>> bitstreamQueue;   // queued BITSTREAM buffer packets (v[0..15])
  std::vector<std::vector<uint32_t>> rawPending;       // raw frames waiting for an output buffer
  Geometry g;
  session::Layout *layout = nullptr;
  uint32_t fps = 30;
  unsigned frame = 0, patternOk = 0, patternBad = 0, rawBatch = 1, peakRaw = 0, peakBitstream = 0;
  unsigned rawSlots = 0, bitstreamSlots = 0;
  bool sessionErrorOnProp = false, dropLast = false, dropCloseAck = false, systemError = false, badBitstream = false;
  bool faultOnRaw = false, watchdogOnStop = false;
  bool rejectArp = false, reportArpOnStop = false, errorCloseAck = false, errorBitstreamStart = false;
  bool reorderFirstBatch = false, zeroRawTimestamps = false, wrongRawIndex = false;
  OutputReply firstOutput = OutputReply::Normal;
  uint32_t sessionErrorProp = 0;
  Peer() : uc(kUcBytes / 4)
  {
    mem.uc = uc.data();
    hfi::initQueues(mem);
    plan.bank = 3;
    plan.pages = 128;
  }
  void bind(const Geometry &geometry, session::Layout &l)
  {
    g = geometry;
    layout = &l;
    arena.resize(l.bytes);
    dma.arena = arena.data();
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
    bitstreamSlots |= 1u << out[1];
    std::vector<uint8_t> nal;
    auto put = [&](uint8_t type, unsigned n) {
      const uint8_t sc[4] = {0, 0, 0, 1};
      nal.insert(nal.end(), sc, sc + 4);
      nal.push_back(uint8_t(0x60 | type));
      for (unsigned i = 0; i < n; ++i) nal.push_back(uint8_t(0x80 | (i & 0x7f)));
    };
    const bool idr = raw[8] == 0;
    const auto mode = idr ? firstOutput : OutputReply::Normal;
    const uint32_t dataOffset = mode == OutputReply::AtCapacity ? 4 : 0;
    if (badBitstream) nal = {0x12, 0x34, 0x56};
    else if (idr) {
      put(7, 12); put(8, 4); put(5, 3000);
      if (mode == OutputReply::AtCapacity) nal.resize(out[5] - dataOffset, 0x80);
    } else put(1, 800);
    CHECK(nal.size() <= out[5] - dataOffset);
    const uint32_t off = out[2] - session::kArenaBase;
    std::memcpy(arena.data() + off + dataOffset, nal.data(), nal.size());
    out[6] = dataOffset;
    out[7] = uint32_t(nal.size());
    switch (mode) {
    case OutputReply::Empty: out[7] = 0; break;
    case OutputReply::WrongIova: out[2] += 4096; break;
    case OutputReply::BadCapacity: out[5] = out[7] - 1; break;
    case OutputReply::BadOffset: out[6] = out[5] + 1; out[7] = 0; break;
    case OutputReply::BadLength: out[6] = 4; out[7] = out[5] - 3; break;
    case OutputReply::Wraparound: out[6] = out[5]; out[7] = UINT32_MAX; break;
    case OutputReply::BadTimestamp:
    case OutputReply::Normal:
    case OutputReply::AtCapacity: break;
    }
    out[8] = mode == OutputReply::BadTimestamp ? UINT32_MAX : raw[8];
    out[9] = mode == OutputReply::BadTimestamp ? UINT32_MAX : raw[9];
    auto returnedRaw = raw;
    if (zeroRawTimestamps) returnedRaw[8] = returnedRaw[9] = 0;
    if (wrongRawIndex && idr) returnedRaw[1] = (raw[1] + 1) % kRaws;
    reply({packet(PropPictureType, 2, PortBitstream, 1, {idr ? PictureIdr : PictureP}),
           packet(Buffers, 1, PortBitstream, 5, out), packet(Buffers, 1, PortRaw, 5, returnedRaw)});
    return true;
  }
  void flushRaw()
  {
    while (!rawPending.empty() && emit(rawPending.front())) rawPending.erase(rawPending.begin());
  }
  int send(unsigned bytes, const char *)
  {
    if (rejectArp && tx[9] == 0x01000009 && tx[16] == 9) return kErrHfi;
    CHECK(hfi::write(mem, hfi::kCmdq, tx, bytes, [] {}) == 0);
    uint32_t data[hfi::kBufferWords], size = 0;
    CHECK(hfi::read(mem, hfi::kCmdq, data, sizeof data, &size, [] {}) == 0);
    CHECK(size == bytes && data[7] == 1 && data[0] == bytes);
    const uint32_t *p = data + 8;
    const uint32_t type = p[1], flags = p[2], port = p[4], payload = p[3], words = (p[0] - 32) / 4;
    sent.push_back({type, flags, port, payload, words, words ? p[8] : 0, words > 1 ? p[9] : 0, hw.phase - 80});
    if (systemError && sent.size() == 6) { reply({packet(0x05000001, 8, 0, 0)}, 0); return 0; }
    if (type == Open) {
      CHECK(!data[1] && port == 0 && flags == 3 && payload == PayloadU32 && p[8]);
      CHECK(!activeSid && arp.empty() && internals.empty() && bitstreamQueue.empty() && rawPending.empty());
      activeSid = p[8];
      openedSessions.push_back(activeSid);
      frame = 0;
      return 0;
    }
    CHECK(activeSid && data[1] == activeSid);
    if (type > 0x03000000 && type < 0x03ffffff) {
      if (sessionErrorOnProp && type == sessionErrorProp) reply({packet(type, 4 | 1, port, 0)});
      return 0;
    }
    std::vector<uint32_t> v(p + 8, p + 8 + words);
    if (type == Buffers) {
      CHECK(words == 16 && flags == 1 && payload == 5 && v[10] == kHostNonSecure);
      const auto *b = layout->find(v[0], v[1]);
      CHECK(b && v[2] == b->iova() && v[5] == b->capacity && port == b->port);
      CHECK(!v[3] && !v[4] && !v[6]);
      if (v[0] == BufBitstream) {
        CHECK(port == PortBitstream && v[1] < 4 && !v[7] && !v[8] && !v[9]);
        CHECK(std::none_of(bitstreamQueue.begin(), bitstreamQueue.end(), [&](const auto &out) { return out[1] == v[1]; }));
        bitstreamQueue.push_back(v);
        peakBitstream = std::max(peakBitstream, unsigned(bitstreamQueue.size()));
      } else if (v[0] == BufRaw) {
        CHECK(port == PortRaw && v[1] < 4 && v[7] == g.rawBytes);
        // The frame really is the pattern of its timestamp's frame number (active pixels and the padding).
        const uint64_t timestamp = uint64_t(v[9]) << 32 | v[8];
        CHECK(timestamp == uint64_t(frame) * 1000000000ULL / fps);
        const uint8_t *base = arena.data() + (v[2] - session::kArenaBase);
        bool same = true;
        pattern(g, frame, [&](uint32_t at, uint8_t want) { same &= base[at] == want; });
        (same ? patternOk : patternBad)++;
        ++frame;
        CHECK(std::none_of(rawPending.begin(), rawPending.end(), [&](const auto &in) { return in[1] == v[1]; }));
        rawSlots |= 1u << v[1];
        rawPending.push_back(v);
        peakRaw = std::max(peakRaw, unsigned(rawPending.size()));
        if (rawPending.size() == 4 && frame == 4 && reorderFirstBatch) std::swap(rawPending[1], rawPending[2]);
        if (rawPending.size() >= rawBatch) flushRaw();
        if (faultOnRaw) ops.fault = true;
      } else if (v[0] == 9) {
        CHECK(arp.empty() && p[4] == 0 && b->queued);
        arp.assign(v.begin(), v.end());
        arpPackets.emplace_back(data, data + size / 4);
      } else {
        CHECK(!v[7] && !v[8] && !v[9]);
        internals.push_back(v);
      }
      return 0;
    }
    if (type == Drain) {
      CHECK(port == PortRaw && flags == 7);
      flushRaw();
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
      if (port == PortBitstream) {
        for (auto &out : bitstreamQueue) {
          out[7] = 0;
          r.push_back(packet(Buffers, 1, PortBitstream, 5, out));
        }
        bitstreamQueue.clear();
      } else {
        for (const auto &in : rawPending) r.push_back(packet(Buffers, 1, PortRaw, 5, in));
        rawPending.clear();
      }
      for (auto it = internals.begin(); it != internals.end();) {
        const auto *b = layout->find((*it)[0], (*it)[1]);
        if (b->port == port) {
          r.push_back(packet(Buffers, 1, port, 5, *it));
          it = internals.erase(it);
        } else {
          ++it;
        }
      }
      if (!arp.empty()) {
        CHECK(layout->find(9, 0)->queued);
        if (reportArpOnStop) r.push_back(packet(Buffers, 1, 0, 5, arp));
      }
      r.push_back(packet(Stop, 1, port, 0));
      reply(r);
      return 0;
    }
    if (type == Close) {
      CHECK(bitstreamQueue.empty() && rawPending.empty());
      if (!arp.empty()) CHECK(layout->find(9, 0)->queued);
      if (!dropCloseAck) {
        reply({packet(Close, errorCloseAck ? 5 : 1, 0, 0)});
        if (!errorCloseAck) {
          arp.clear();
          internals.clear();
          closedSessions.push_back(activeSid);
          activeSid = 0;
        }
      }
      return 0;
    }
    if (type == Start) {
      CHECK(!arp.empty() && layout->find(9, 0)->queued);
      reply({packet(Start, port == PortBitstream && errorBitstreamStart ? 4 : 1, port, 0)});
    }
    return 0;
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
  Run(uint32_t w, uint32_t h, uint32_t cap = 256 * 1024) : stream(cap + 1, 0xa5), t(peer, layout)
  {
    CHECK(t.g.configure(w, h));
    CHECK(buildLayout(layout, t.g));
    peer.bind(t.g, layout);
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
  const struct {
    uint32_t width, height, stride, yRows, uvRows, yBytes, uvBytes, rawBytes, bitstreamBytes, codedHeight, vpssBytes, guardedBytes;
    uint32_t bin, comv, nonComv, line, dpb;
  } cases[] = {
      {320, 240, 384, 256, 128, 98304, 49152, 147456, 245760, 240, 131072, 9732096,
       5969408, 41472, 274176, 619776, 626688},
      {640, 480, 640, 480, 240, 307200, 153600, 462848, 921600, 480, 0, 32980992,
       23787008, 147456, 313856, 700928, 1921024},
      {1280, 720, 1280, 736, 368, 942080, 471040, 1413120, 708608, 720, 1449984, 27557888,
       9563648, 436224, 392448, 834048, 5804032},
      {1920, 1080, 1920, 1088, 544, 2088960, 1044480, 3133440, 1568768, 1088, 0, 56180736,
       21661184, 983040, 542720, 984576, 12636160},
  };
  Geometry g;
  CHECK(kInternalCount == 4);
  // Layouts: no overlap, guards between buffers, inside the AVC arena, every byte of every buffer mapped.
  for (const auto &c : cases) {
    CHECK(g.configure(c.width, c.height));
    CHECK(g.stride == c.stride && g.scanlines == c.yRows && g.uvScanlines == c.uvRows);
    CHECK(g.uvOffset == c.yBytes && g.stride * g.uvScanlines == c.uvBytes);
    CHECK(g.rawBytes == c.rawBytes && g.bitstreamBytes == c.bitstreamBytes);
    CHECK(g.codedWidth == c.width && g.codedHeight == c.codedHeight && g.internal.vpss == c.vpssBytes);
    const Script in = inputConfig(g), out = outputConfig(g, Settings{});
    CHECK(in.p[1].v[0] == (c.width << 16 | c.height));
    CHECK(in.p[3].v[0] == (c.stride << 16 | c.yRows) && in.p[3].v[1] == (c.stride << 16 | c.uvRows));
    CHECK(out.p[0].id == PropBitstreamResolution && out.p[0].v[0] == (c.width << 16 | c.codedHeight));
    CHECK(out.p[1].id == PropCrop && out.p[1].payload == Payload64Packed && out.p[1].words == 2);
    CHECK(out.p[1].v[0] == 0 && out.p[1].v[1] == c.codedHeight - c.height);

    session::Layout l;
    CHECK(buildLayout(l, g));
    CHECK(l.used == 14u + (c.vpssBytes ? 1u : 0u) && l.bytes == c.guardedBytes && l.arenaBytes() == 256u * 1024 * 1024);
    for (unsigned i = 0; i < 4; ++i) {
      const auto *raw = l.find(BufRaw, i), *bits = l.find(BufBitstream, i);
      CHECK(raw && raw->capacity == c.rawBytes && raw->port == PortRaw);
      CHECK(bits && bits->capacity == c.bitstreamBytes && bits->port == PortBitstream);
    }
    for (unsigned i = 0; i < l.used; ++i) {
      const auto &b = l.buf[i];
      CHECK(!(b.off & 16383) && b.capacity == b.size);
      for (uint32_t at = 0; at < b.capacity; at += 4096) CHECK(l.mapped((b.off + at) / 4096));
      for (uint32_t at = (b.capacity + 4095) & ~4095u; at < ((b.capacity + 16383) & ~16383u) + 16384; at += 4096)
        CHECK(!l.mapped((b.off + at) / 4096));
      for (unsigned j = i + 1; j < l.used; ++j) CHECK(l.buf[j].off >= b.off + b.capacity);
    }
    const auto *vpss = l.find(BufVpss, 0);
    CHECK(bool(vpss) == (g.stride != g.codedWidth || g.scanlines != g.codedHeight));
    CHECK(c.vpssBytes ? vpss && vpss->capacity == c.vpssBytes && vpss->port == PortRaw : !vpss);
    const auto *arp = l.find(9, 0);
    CHECK(arp && arp->capacity == 204800 && arp->port == 0);
    const struct { uint32_t type, bytes; } internals[] = {
        {BufBin, c.bin}, {BufComv, c.comv}, {BufNonComv, c.nonComv}, {BufLine, c.line}, {BufDpb, c.dpb},
    };
    for (const auto &internal : internals) {
      const auto *b = l.find(internal.type, 0);
      CHECK(b && b->capacity == internal.bytes && b->port == PortBitstream);
    }
    CHECK(!l.find(BufRaw, 4) && !l.find(BufBitstream, 4));
  }

  const uint32_t invalid[][2] = {
      {0, 0}, {0, 480}, {640, 0}, {641, 480}, {640, 481}, {800, 600}, {320, 480}, {1920, 1088},
      {1922, 1080}, {1920, 1082}, {3840, 2160}, {65536 + 640, 480}, {640, 65536 + 480},
      {UINT32_MAX, 480}, {640, UINT32_MAX}, {0xffffff80, 0xffffffe0},
  };
  session::Layout rejected;
  for (const auto &size : invalid) {
    CHECK(g.configure(1920, 1080) && buildLayout(rejected, g));
    CHECK(!g.configure(size[0], size[1]));
    CHECK(!g.width && !g.height && !g.stride && !g.scanlines && !g.uvScanlines && !g.uvOffset);
    CHECK(!g.rawBytes && !g.bitstreamBytes && !g.codedResolution() && !g.codedCrop() && !g.internal.bin);
    CHECK(!buildLayout(rejected, g) && !rejected.used && !rejected.bytes);
  }
  for (uint32_t capacity : {0u, 256u * 1024 * 1024, UINT32_MAX}) {
    CHECK(g.configure(640, 480));
    g.rawBytes = capacity;
    CHECK(!buildLayout(rejected, g) && !rejected.used && !rejected.bytes);
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
  CHECK(caps.n == 16 && caps.p[0].id == PropProfile && caps.p[0].v[0] == 4 && caps.p[1].v[0] == 14 && caps.p[4].port == PortNone);
  CHECK(caps.p[6].id == PropTotalBitrate && caps.p[6].port == PortRaw && caps.p[14].id == PropRequestSyncFrame);
}

static void residentArenaTests()
{
  TestHw hw;
  Bringup<TestHw> core(hw);
  core.avcWidth = 1920;
  core.avcHeight = 1088;
  std::vector<uint64_t> words(vsmmu::kSessionTablePages * 512), uc(vsmmu::kUcPages);
  std::vector<uint64_t> pages(session::kAvcArenaBytes / 4096);
  for (unsigned i = 0; i < uc.size(); ++i) uc[i] = kXnuDramBase + i * 4096ull;
  vsmmu::Tables tables{words.data(), kXnuDramBase + 0x2000000, vsmmu::kSessionTablePages};
  for (uint64_t base : {kXnuDramBase + 0x10000000, kExtraArenaBase}) {
    for (unsigned i = 0; i < pages.size(); ++i)
      pages[i] = base + (base == kExtraArenaBase ? i : (i * 7907u) % pages.size()) * 4096ull;
    CHECK(vsmmu::build(tables, uc.data(), unsigned(uc.size()), kXnuDramBase + 0x3000000));
    core.hevc = false;
    core.bitdepth = 8;
    core.encoder = bootEnabled("piano-video=6 piano-video-enc=0");
    CHECK(core.prepareLayout() && !core.layout.hevc && core.layout.used > 10);
    CHECK(!core.layout.find(9, 0) && hw.log.find("E1") == std::string::npos);
    CHECK(session::buildArena(tables, core.layout, pages.data(), session::kAvcArenaBytes));
    const auto avcLeaves = words;

    for (const auto &size : kInternal) {
      core.encoder = bootEnabled("piano-video=6 piano-video-enc=1");
      CHECK(core.encGeometry.configure(size.width, size.height));
      CHECK(core.prepareLayout() && core.layout.used == (size.vpss ? 15u : 14u));
      CHECK(session::buildArena(tables, core.layout, pages.data(), session::kAvcArenaBytes));
      const auto *arp = core.layout.find(9, 0);
      if (arp) {
        for (unsigned i = 0; i < 50; ++i)
          CHECK(vsmmu::walk(tables, arp->iova() + i * 4096) == pages[arp->off / 4096 + i]);
        for (unsigned i = 50; i < 56; ++i)
          CHECK(!vsmmu::walk(tables, arp->iova() + i * 4096));
      } else {
        CHECK(arp);
      }
      CHECK(vsmmu::walk(tables, kIovaBase) == uc[0]);
      CHECK(words != avcLeaves);

      core.encoder = false;
      core.hevc = true;
      core.bitdepth = 10;
      CHECK(core.prepareLayout() && core.layout.hevc && core.layout.arenaBytes() == 188 * 1024 * 1024);
      CHECK(session::buildArena(tables, core.layout, pages.data(), session::kAvcArenaBytes));
      core.hevc = false;
      core.bitdepth = 8;
      CHECK(core.prepareLayout() && !core.layout.find(9, 0));
      CHECK(session::buildArena(tables, core.layout, pages.data(), session::kAvcArenaBytes));
      CHECK(words == avcLeaves);
    }
  }
}

static void scanTests()
{
  const uint8_t s[] = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 9, 9, 9};
  bool bad = true;
  Nal n[8];
  CHECK(scanAnnexB(s, sizeof s, n, 8, &bad) == 3 && !bad);
  CHECK(n[0].type == 7 && n[0].offset == 4 && n[0].size == 3);
  CHECK(n[1].type == 8 && n[1].offset == 10 && n[1].size == 2);
  CHECK(n[2].type == 5 && n[2].offset == 16 && n[2].size == 4 && n[2].offset + n[2].size == sizeof s);
  const FrameNals f = classify(s, sizeof s);
  CHECK(f.sps == 1 && f.pps == 1 && f.idr == 1 && !f.slice && !f.bad);
  const uint8_t junk[] = {1, 2, 3};
  CHECK(scanAnnexB(junk, 3, n, 8, &bad) == 0 && bad);
  const uint8_t empty[] = {0, 0, 1, 0, 0, 1, 0x41};
  CHECK(scanAnnexB(empty, sizeof empty, n, 8, &bad) == 0 && bad);
  CHECK(scanAnnexB(nullptr, 0, nullptr, 0, &bad) == 0 && !bad);
  CHECK(scanAnnexB(s, sizeof s, nullptr, 0, &bad) == 3 && !bad);
  for (unsigned cap : {1u, 2u, 3u}) {
    std::vector<Nal> limited(cap);
    CHECK(scanAnnexB(s, sizeof s, limited.data(), cap, &bad) == 3 && !bad);
    CHECK(limited.back().offset == n[cap - 1].offset);
  }
  for (const std::vector<uint8_t> &prefix : {std::vector<uint8_t>{0}, {0, 0}, {0, 0, 1}, {0, 0, 0, 1}}) {
    CHECK(scanAnnexB(prefix.data(), uint32_t(prefix.size()), n, 8, &bad) == 0 && bad);
  }
  const uint8_t emptyTail[] = {0, 0, 1, 0x67, 0, 0, 0, 1};
  CHECK(scanAnnexB(emptyTail, sizeof emptyTail, n, 1, &bad) == 1 && bad);
  CHECK(classify(emptyTail, sizeof emptyTail).bad);
  const uint8_t escaped[] = {0, 0, 0, 1, 0x65, 0, 0, 3, 1};
  CHECK(scanAnnexB(escaped, sizeof escaped, n, 8, &bad) == 1 && !bad && n[0].size == 5);
}

static void settingsAndReopenTests()
{
  Peer peer;
  Geometry g;
  session::Layout layout;
  CHECK(g.configure(320, 240) && buildLayout(layout, g));
  peer.bind(g, layout);
  const Settings invalid[] = {
      {.fps = 0}, {.fps = 61}, {.fps = 65536}, {.fps = UINT32_MAX},
      {.bitrate = 0}, {.bitrate = 99999}, {.bitrate = 40000001}, {.bitrate = UINT32_MAX},
      {.gop = 0}, {.gop = 301}, {.gop = UINT32_MAX},
      {.frames = 0}, {.frames = 1}, {.frames = 121}, {.frames = UINT32_MAX},
  };
  CHECK(admitted(Settings{}));
  for (const auto &settings : invalid) {
    EncodeSelfTest<Peer> t(peer, layout);
    t.g = g;
    t.set = settings;
    CHECK(!admitted(settings));
    CHECK(t.run() == kErrArgument && t.r.failedStage == Prepared && !t.r.etb && peer.sent.empty());
  }

  std::vector<uint8_t> stream(128 * 1024);
  const Settings supported[] = {{.fps = 1, .bitrate = 100000, .gop = 1, .frames = 6},
                                {.fps = 60, .bitrate = 40000000, .gop = 300, .frames = 120}};
  for (unsigned run = 0; run < 2; ++run) {
    const auto &settings = supported[run];
    CHECK(buildLayout(layout, g));  // Native reopen rebuilds layout ownership and uses a fresh transaction.
    for (unsigned i = 0; i < layout.used; ++i) CHECK(!layout.buf[i].queued && !layout.buf[i].pendingRelease);
    EncodeSelfTest<Peer> t(peer, layout);
    t.g = g;
    t.set = settings;
    t.sessionId += run;
    t.stream = stream.data();
    t.streamCap = uint32_t(stream.size());
    peer.fps = settings.fps;
    const unsigned from = unsigned(peer.sent.size()), patternsBefore = peer.patternOk;
    CHECK(admitted(settings) && t.run() == 0 && t.r.pass());
    CHECK(t.r.etb == settings.frames && t.r.ebd == settings.frames && t.r.fbd == settings.frames);
    CHECK(t.r.ftb == settings.frames + 4 && t.r.empty == 4 && t.r.drainAck && t.r.last);
    CHECK(t.r.stopBitstream && t.r.stopRaw && t.r.closed && t.frames[0].offset == 0);
    CHECK(t.r.sps == 1 && t.r.pps == 1 && t.r.idr == 1 && t.r.slices == settings.frames - 1);
    CHECK(t.r.streamBytes == 3031 + (settings.frames - 1) * 805);
    for (unsigned f = 0; f < settings.frames; ++f) {
      CHECK(t.frames[f].ebdUs > t.frames[f].etbUs && t.frames[f].fbdUs > t.frames[f].etbUs);
      CHECK(t.frames[f].offset == (f ? 3031 + (f - 1) * 805 : 0));
    }
    CHECK(peer.patternOk - patternsBefore == settings.frames && !peer.patternBad);
    CHECK(!peer.activeSid && peer.bitstreamQueue.empty() && peer.rawPending.empty());
    CHECK(peer.mem.ld(hfi::qhdr(hfi::kMsgq) + hfi::kQRead) == peer.mem.ld(hfi::qhdr(hfi::kMsgq) + hfi::kQWrite));
    for (unsigned i = 0; i < layout.used; ++i) CHECK(!layout.buf[i].queued);

    const struct { uint32_t id, port, payload, value; unsigned count; } wire[] = {
        {0x0300010c, 1, 8, settings.fps * 65536, 1},      // Q16 fps
        {0x0300013b, 2, 1, settings.bitrate, 2},          // RAW target bitrate in bits/s
        {0x0300012a, 1, 9, 0, 2},                       // BITSTREAM VBR_CFR
        {0x03000146, 2, 1, settings.gop, 2},              // RAW GOP length
    };
    for (const auto &p : wire) {
      unsigned count = 0;
      for (unsigned i = from; i < peer.sent.size(); ++i) {
        const auto &s = peer.sent[i];
        if (s.type != p.id) continue;
        CHECK(s.port == p.port && s.payload == p.payload && s.words == 1 && s.v0 == p.value);
        ++count;
      }
      CHECK(count == p.count);
    }
  }
  const std::vector<uint32_t> epochs = {0x56340e01, 0x56340e02};
  CHECK(peer.openedSessions == epochs && peer.closedSessions == epochs);
  CHECK(peer.arpPackets.size() == epochs.size() && peer.arpPackets[0][1] == epochs[0] && peer.arpPackets[1][1] == epochs[1]);
}

static void timestampTests()
{
  CHECK(frameNs(0, 30) == 0 && frameNs(1, 30) == 33333333 && frameNs(2, 30) == 66666666 && frameNs(3, 30) == 100000000);
  CHECK(frameNs(1, 60) == 16666666 && frameNs(2, 60) == 33333333 && frameNs(3, 60) == 50000000);
  CHECK(frameNs(119, 30) == 3966666666 && frameNs(119, 60) == 1983333333 && frameNs(119, 1) == 119000000000);
  for (uint32_t fps = 1; fps <= 60; ++fps) {
    for (unsigned f = 0; f < 120; ++f) {
      const uint64_t timestamp = frameNs(f, fps);
      CHECK(frameIndex(timestamp, fps) == f);
      CHECK(frameIndex(timestamp + 1, fps) == kMaxFrames);
      if (timestamp) CHECK(frameIndex(timestamp - 1, fps) == kMaxFrames);
    }
    CHECK(frameIndex(frameNs(120, fps), fps) == kMaxFrames && frameIndex(UINT64_MAX, fps) == kMaxFrames);
  }
  CHECK(frameIndex(0, 0) == kMaxFrames && frameIndex(0, UINT32_MAX) == kMaxFrames);
}

static void outputBoundsTests()
{
  constexpr uint32_t firstBytes = 3031, totalBytes = firstBytes + 805;
  std::vector<uint8_t> complete;
  for (uint32_t cap : {totalBytes, 0u, 2u, firstBytes - 1, firstBytes, totalBytes - 1}) {
    Run r(320, 240, cap);
    r.t.set.frames = 2;
    CHECK(r.go() == 0 && r.t.r.closed && r.t.r.last && r.t.r.empty == 4);
    CHECK(r.t.r.fbd == 2 && r.t.r.streamBytes == cap && !r.t.r.badNal);
    CHECK(r.t.r.truncated == (cap < totalBytes) && r.t.r.pass() == (cap == totalBytes));
    CHECK(r.t.frames[0].bytes == firstBytes && r.t.frames[1].bytes == 805);
    CHECK(r.t.frames[0].nals.idr == (cap >= firstBytes ? 1u : 0u));
    CHECK(r.t.frames[1].nals.slice == (cap == totalBytes ? 1u : 0u));
    CHECK(r.stream[cap] == 0xa5);
    if (cap == totalBytes) complete = r.stream;
    else CHECK(!std::memcmp(r.stream.data(), complete.data(), cap));
  }
  {
    Run r(320, 240, 245760 + 805 - 4);
    r.t.set.frames = 2;
    r.peer.firstOutput = OutputReply::AtCapacity;
    CHECK(r.go() == 0 && r.t.r.pass());
    CHECK(r.t.frames[0].bytes == 245760 - 4 && r.t.frames[1].offset == r.t.frames[0].bytes);
    CHECK(r.t.frames[0].nals.idr == 1 && r.stream[4] == 0x67 && r.stream[245760 - 5] == 0x80);
    CHECK(r.t.r.streamBytes == r.t.streamCap && r.stream.back() == 0xa5);
  }
  for (auto mode : {OutputReply::Empty, OutputReply::WrongIova, OutputReply::BadCapacity,
                    OutputReply::BadOffset, OutputReply::BadLength, OutputReply::Wraparound, OutputReply::BadTimestamp}) {
    Run r(320, 240);
    r.peer.firstOutput = mode;
    CHECK(r.go() == kErrHfi && r.t.r.failedStage == Frames && !r.t.r.pass());
    CHECK(r.t.r.closed && !r.t.r.last && r.t.r.etb == 1 && r.t.r.ebd == 1);
    CHECK(r.t.r.fbd == 0 && r.t.r.ftb == 4 && r.t.r.streamBytes == 0 && r.stream[0] == 0xa5);
  }
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

static void capabilityWireTests(const Peer &p, unsigned first, Stage stage)
{
  const struct { uint32_t id, port, payload, value; } expected[] = {
      {0x03000107, 1, 9, 4}, {0x03000108, 1, 9, 14}, {0x03000123, 2, 1, 64}, {0x03000123, 1, 1, 64},
      {0x0300010a, 0, 1, 2}, {0x03000149, 1, 9, 2}, {0x0300013b, 2, 1, 2000000}, {0x0300012a, 1, 9, 0},
      {0x03000146, 2, 1, 9}, {0x03000121, 1, 1, 1}, {0x03000129, 1, 10, 0x00060600}, {0x0300014c, 2, 9, 0},
      {0x03000131, 1, 1, 0}, {0x03000147, 1, 1, 0}, {0x03000145, 2, 9, 1}, {0x0300012b, 1, 1, 0},
  };
  for (const auto &want : expected) {
    CHECK(first < p.sent.size());
    if (first >= p.sent.size()) return;
    const auto &got = p.sent[first++];
    CHECK(got.type == want.id && got.port == want.port && got.payload == want.payload && got.v0 == want.value);
    CHECK(got.stage == int(stage) && got.flags == 0 && got.words == 1);
  }
}

static void sessionTests()
{
  {  // success, 640x480 (no VPSS)
    Run r(640, 480);
    r.peer.rawBatch = 4;
    r.peer.reorderFirstBatch = true;
    r.peer.zeroRawTimestamps = true;
    CHECK(r.go() == 0 && r.t.r.pass());
    const auto &p = r.peer;
    CHECK(p.patternOk == 10 && !p.patternBad);
    CHECK(r.t.r.etb == 10 && r.t.r.ebd == 10 && r.t.r.fbd == 10 && r.t.r.idr == 1 && r.t.r.sps == 1 && r.t.r.slices == 9);
    CHECK(r.t.r.drainAck && r.t.r.last && r.t.r.stopBitstream && r.t.r.stopRaw && r.t.r.closed && !r.t.r.truncated);
    CHECK(r.t.frames[0].picture == PictureIdr && r.t.frames[1].picture == PictureP);
    CHECK(p.rawSlots == 0xf && p.bitstreamSlots == 0xf && p.peakRaw == 4 && p.peakBitstream == 4);
    CHECK(r.t.r.ftb == 14 && r.t.r.empty == 4);
    for (unsigned f = 0; f < 10; ++f)
      CHECK(r.t.frames[f].ebdUs > r.t.frames[f].etbUs && r.t.frames[f].fbdUs > r.t.frames[f].etbUs);
    CHECK(r.t.frames[2].ebdUs < r.t.frames[1].ebdUs && r.t.frames[2].fbdUs < r.t.frames[1].fbdUs);
    CHECK(r.t.frames[2].offset == r.t.frames[0].bytes && r.t.frames[1].offset == r.t.frames[2].offset + r.t.frames[2].bytes);
    const auto t = types(p);
    CHECK(t.size() > 50 && t[0] == Open && t[1] == PropCodec && p.sent[1].v0 == CodecEncodeAvc && p.sent[1].payload == PayloadEnum);
    CHECK(t[2] == PropColorFormat && t[6] == PropColorInfo);
    capabilityWireTests(p, 7, InputConfig);
    capabilityWireTests(p, 28, OutputConfig);
    const unsigned startRaw = indexOf(p, 0x01000005, 2), startBits = indexOf(p, 0x01000005, 1);
    CHECK(startRaw == 24 && t[25] == PropBitstreamResolution && startBits == 50);
    const auto *arp = r.layout.find(9, 0);
    const unsigned arpAt = bufferIndex(p, 9);
    CHECK(arp && !arp->queued && p.arp.empty() && p.internals.empty());
    CHECK(arpAt == 23 && p.arpPackets.size() == 1 && p.sent[arpAt].stage == InputConfig);
    if (arp && !p.arpPackets.empty()) {
      const std::vector<uint32_t> expected = {
          128, r.t.sessionId, 23, 0, 0, 0, 0, 1,
          96, 0x01000009, 1, 5, 0, 23, 0, 0,
          9, 0, arp->iova(), 0, 0, 204800, 0, 0, 0, 0, 0x00000200, 0, 0, 0, 0, 0,
      };
      CHECK(p.arpPackets[0] == expected && !(arp->iova() & 16383));
    }
    CHECK(bufferIndex(p, BufVpss) == ~0u);
    const unsigned bin = bufferIndex(p, BufBin), comv = bufferIndex(p, BufComv), dpb = bufferIndex(p, BufDpb);
    CHECK(bin > startRaw && bin < comv && comv < dpb && dpb < startBits && p.sent[comv - 1].type == PropComvCount);
    const auto &count = p.sent[comv - 1];
    CHECK(count.type == 0x03000193 && count.port == 1 && count.flags == 0 && count.payload == 1 && count.words == 1 && count.v0 == 4);
    CHECK(bufferIndex(p, BufBitstream) == startBits + 1 && bufferIndex(p, BufRaw) > startBits);
    const unsigned drain = indexOf(p, Drain, PortRaw), stop1 = indexOf(p, Stop, PortBitstream), stop2 = indexOf(p, Stop, PortRaw);
    CHECK(drain != ~0u && drain < stop1 && stop1 < stop2 && t.back() == Close);
    // The export is the whole stream in output order and starts with the SPS.
    CHECK(r.t.r.streamBytes > 3000 && r.stream[4] == 0x67);
    CHECK(r.peer.hw.log.find("E1 result PASS") != std::string::npos);
  }
  for (const auto &size : {kInternal[0], kInternal[2], kInternal[3]}) {
    Run r(size.width, size.height);
    r.t.set.frames = 2;
    CHECK(r.go() == 0 && r.t.r.pass());
    const unsigned vpss = bufferIndex(r.peer, BufVpss);
    if (size.vpss)
      CHECK(vpss != ~0u && r.peer.sent[vpss].port == PortRaw && bufferIndex(r.peer, 9) < vpss &&
            vpss < indexOf(r.peer, Start, PortRaw));
    else
      CHECK(vpss == ~0u);
  }
  {  // a failed ring write leaves ARP host-owned and no port started
    Run r(640, 480);
    r.peer.rejectArp = true;
    CHECK(r.go() == kErrHfi && r.t.r.failedStage == InputConfig && r.t.r.closed);
    CHECK(r.peer.arpPackets.empty() && !r.layout.find(9, 0)->queued);
    CHECK(indexOf(r.peer, Start, PortRaw) == ~0u && indexOf(r.peer, Stop, PortRaw) == ~0u);
  }
  {  // match the stage-5 failure trace: both STOPs return scratch and CLOSE fences ARP
    Run r(640, 480);
    r.peer.errorBitstreamStart = true;
    r.peer.reportArpOnStop = true;
    CHECK(r.go() == kErrHfi && r.t.r.failedStage == OutputStart && r.t.r.errorPort == 1);
    CHECK(r.t.r.stopBitstream && r.t.r.stopRaw && r.t.r.closed && !r.t.r.close && !r.t.r.etb);
    CHECK(r.peer.arpPackets.size() == 1 && !r.layout.find(9, 0)->queued && r.peer.arp.empty());
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
  for (bool errorAck : {false, true}) {
    Run r(640, 480);
    r.peer.dropCloseAck = !errorAck;
    r.peer.errorCloseAck = errorAck;
    CHECK(r.go() != 0 && !r.t.r.closed && r.t.r.close == kErrHfiTimeout && !r.t.r.pass());
    CHECK(r.t.r.stopBitstream && r.t.r.stopRaw && r.layout.find(9, 0)->queued && !r.peer.arp.empty());
  }
  {  // a system error is fatal at once
    Run r(640, 480);
    r.peer.systemError = true;
    CHECK(r.go() == kErrSysError && !r.t.r.pass());
    CHECK(indexOf(r.peer, Stop, PortRaw) == ~0u && indexOf(r.peer, Close, PortNone) == ~0u);
  }
  {  // a bitstream that is not Annex B fails the NAL gate
    Run r(640, 480);
    r.peer.badBitstream = true;
    r.go();
    CHECK(r.t.r.badNal > 0 && !r.t.r.pass());
  }
  {  // a different slot cannot return this input credit with the original IOVA
    Run r(640, 480);
    r.peer.rawBatch = 4;
    r.peer.wrongRawIndex = true;
    CHECK(r.go() == kErrHfi && r.t.r.failedStage == Frames && r.t.r.closed);
    CHECK(r.t.r.etb == 4 && r.t.r.ebd == 3 && !r.t.frames[0].ebdUs && r.t.frames[1].ebdUs);
  }
  {  // an SMMU fault during frame submission leaves the DMA state held for quarantine
    Run r(640, 480);
    r.peer.faultOnRaw = true;
    CHECK(r.go() == kErrFault && !r.t.r.pass());
    CHECK(r.t.r.etb == 1 && r.t.r.fbd == 0 && r.peer.sent.back().v0 == BufRaw);
    CHECK(indexOf(r.peer, Stop, PortBitstream) == ~0u && indexOf(r.peer, Close, PortNone) == ~0u);
    CHECK(r.layout.find(9, 0)->queued && !r.peer.arp.empty());
  }
  {  // a watchdog during shutdown prevents the second STOP and CLOSE
    Run r(640, 480);
    r.peer.watchdogOnStop = true;
    CHECK(r.go() == kErrWatchdog && !r.t.r.pass());
    CHECK(r.peer.sent.back().type == Stop && r.peer.sent.back().port == PortBitstream);
    CHECK(!r.t.r.stopRaw && !r.t.r.closed);
    CHECK(r.layout.find(9, 0)->queued && !r.peer.arp.empty());
  }
}

static int writePattern(const char *file, uint32_t w, uint32_t h, unsigned n)
{
  Geometry g;
  if (!g.configure(w, h) || !n || n > kMaxFrames) return 2;
  FILE *f = std::fopen(file, "wb");
  if (!f) return 2;
  std::vector<uint8_t> nv12(g.rawBytes), packed(w * h * 3 / 2);
  for (unsigned k = 0; k < n; ++k) {
    pattern(g, k, [&](uint32_t at, uint8_t v) { nv12[at] = v; });
    for (uint32_t y = 0; y < h; ++y) std::memcpy(&packed[y * w], &nv12[y * g.stride], w);
    for (uint32_t y = 0; y < h / 2; ++y) std::memcpy(&packed[w * h + y * w], &nv12[g.uvOffset + y * g.stride], w);
    if (std::fwrite(packed.data(), 1, packed.size(), f) != packed.size()) {
      std::fclose(f);
      return 2;
    }
  }
  return std::fclose(f) ? 2 : 0;
}

int main(int argc, char **argv)
{
  if (argc == 6 && !std::strcmp(argv[1], "--pattern")) {
    uint32_t args[3];
    for (unsigned i = 0; i < 3; ++i) {
      const char *begin = argv[3 + i], *end = begin + std::strlen(begin);
      const auto parsed = std::from_chars(begin, end, args[i]);
      if (parsed.ec != std::errc{} || parsed.ptr != end) return 2;
    }
    return writePattern(argv[2], args[0], args[1], args[2]);
  }
  bootGateTests();
  geometryTests();
  residentArenaTests();
  scanTests();
  patternTests();
  timestampTests();
  settingsAndReopenTests();
  outputBoundsTests();
  sessionTests();
  if (failures) {
    std::printf("enc_host: %u FAILED\n", failures);
    return 1;
  }
  std::printf("enc_host: all E1 encoder checks passed\n");
  return 0;
}
