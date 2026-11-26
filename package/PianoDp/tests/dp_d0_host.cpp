// SPDX-License-Identifier: BSD-2-Clause-Patent
// Run run-dp-host.sh to generate DpQmpTables.h and run both sanitized host tests.
// After generating that header in /tmp, the standalone build from this directory is:
// c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I/tmp dp_d0_host.cpp -o /tmp/dp_d0_host && /tmp/dp_d0_host
#include "DpLinkTrain.h"
#include "DpQmpTables.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace pianodp;
using Pattern = DpTrainingPattern;
using Aux = DpAuxStatus;
using Train = DpTrainStatus;
using Edid = EdidStatus;

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static constexpr DpTiming k1080p60{148500, 1920, 280, 88, 44, 1080, 45, 4, 5, true, true};
static constexpr DpTiming k1440p60{241500, 2560, 160, 48, 32, 1440, 41, 3, 5, true, false};
static constexpr DpTiming k4k30{297000, 3840, 560, 176, 88, 2160, 90, 8, 10, true, true};
static constexpr DpTiming k4k60Rb{533250, 3840, 160, 48, 32, 2160, 62, 3, 5, true, false};
static constexpr DpTiming k4k60Cta{594000, 3840, 560, 176, 88, 2160, 90, 8, 10, true, true};

using Block = std::array<uint8_t, kEdidBlockBytes>;
using Descriptor = std::array<uint8_t, 18>;

static bool sameTiming(const DpTiming &a, const DpTiming &b)
{
  return a.pixelKhz == b.pixelKhz && a.hActive == b.hActive && a.hBlank == b.hBlank &&
         a.hSyncOffset == b.hSyncOffset && a.hSyncWidth == b.hSyncWidth &&
         a.vActive == b.vActive && a.vBlank == b.vBlank &&
         a.vSyncOffset == b.vSyncOffset && a.vSyncWidth == b.vSyncWidth &&
         a.hSyncPositive == b.hSyncPositive && a.vSyncPositive == b.vSyncPositive &&
         a.separateSync == b.separateSync && a.interlaced == b.interlaced;
}

static void seal(uint8_t *block)
{
  uint8_t sum = 0;
  for (size_t i = 0; i < kEdidBlockBytes - 1; ++i) sum = uint8_t(sum + block[i]);
  block[kEdidBlockBytes - 1] = uint8_t(0 - sum);
}

// Encode synthetic DTDs in the EDID 1.4 wire format, with zero borders.
static Descriptor wireDtd(const DpTiming &t)
{
  const uint32_t clock = t.pixelKhz / 10;
  return {uint8_t(clock), uint8_t(clock >> 8), uint8_t(t.hActive), uint8_t(t.hBlank),
          uint8_t((t.hActive >> 8) << 4 | (t.hBlank >> 8)), uint8_t(t.vActive), uint8_t(t.vBlank),
          uint8_t((t.vActive >> 8) << 4 | (t.vBlank >> 8)), uint8_t(t.hSyncOffset), uint8_t(t.hSyncWidth),
          uint8_t((t.vSyncOffset & 15) << 4 | (t.vSyncWidth & 15)),
          uint8_t((t.hSyncOffset >> 8) << 6 | (t.hSyncWidth >> 8) << 4 |
                  (t.vSyncOffset >> 4) << 2 | (t.vSyncWidth >> 4)),
          0, 0, 0, 0, 0,
          uint8_t((t.separateSync ? 0x18 : 0) | (t.hSyncPositive ? 2 : 0) |
                  (t.vSyncPositive ? 4 : 0) | (t.interlaced ? 0x80 : 0))};
}

static void putDtd(Block &block, size_t offset, const DpTiming &timing)
{
  const auto dtd = wireDtd(timing);
  std::copy(dtd.begin(), dtd.end(), block.begin() + offset);
}

static Block baseBlock(const DpTiming &preferred, uint8_t extensions = 0)
{
  Block block{0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  constexpr uint16_t pnp = (20 << 10) | (19 << 5) | 20; // synthetic TST
  block[8] = uint8_t(pnp >> 8);
  block[9] = uint8_t(pnp);
  block[10] = 0x34;
  block[11] = 0x12;
  block[16] = 1;
  block[17] = 36;
  block[18] = 1;
  block[19] = 4;
  block[20] = 0xa5;  // digital DisplayPort, 8 bpc
  block[21] = 60;
  block[22] = 34;
  block[23] = 120;
  block[24] = 0x02;
  std::fill(block.begin() + 38, block.begin() + 54, 1); // unused standard timings
  putDtd(block, 54, preferred);
  for (size_t offset = 72; offset <= 108; offset += 18) block[offset + 3] = 0x10; // dummy descriptors
  block[126] = extensions;
  seal(block.data());
  return block;
}

static std::vector<uint8_t> wireEdid(std::initializer_list<Block> blocks)
{
  std::vector<uint8_t> bytes;
  for (const auto &block : blocks) bytes.insert(bytes.end(), block.begin(), block.end());
  return bytes;
}

static std::vector<uint8_t> multiBlockEdid()
{
  auto base = baseBlock(k4k60Rb, 3);
  putDtd(base, 72, k1440p60);
  seal(base.data());
  Block cta{2, 3, 12, 2, 0x23, 0x09, 0x07, 0x07, 0x43, 97, 95, 0x90};
  putDtd(cta, 12, k1080p60);
  seal(cta.data());
  // A duplicate VIC, a valid extended VIC, and one near the native-flag boundary.
  Block cta2{2, 3, 8, 2, 0x43, 16, 193, 192};
  seal(cta2.data());
  Block otherExtension{0xf0, 2, 2}; // opaque block map
  seal(otherExtension.data());
  return wireEdid({base, cta, cta2, otherExtension});
}

static void testEdidTimings()
{
  EdidInfo info;
  for (const auto &mode : {k1080p60, k1440p60, k4k60Rb}) {
    const auto block = baseBlock(mode);
    CHECK(parseEdid(block.data(), block.size(), info) == Edid::Ok);
    CHECK(std::strcmp(info.manufacturer, "TST") == 0 && info.productCode == 0x1234);
    CHECK(info.widthMm == 600 && info.heightMm == 340);
    CHECK(info.extensionCount == 0 && info.modeCount == 1);
    CHECK(sameTiming(info.modes[0].timing, mode));
  }
  for (const auto &size : {std::array{0, 0}, std::array{79, 0}, std::array{0, 79}}) {
    auto block = baseBlock(k1080p60);
    block[21] = uint8_t(size[0]);
    block[22] = uint8_t(size[1]);
    seal(block.data());
    CHECK(parseEdid(block.data(), block.size(), info) == Edid::Ok && !info.widthMm && !info.heightMm);
  }
  constexpr DpTiming wideSync{400000, 2048, 1300, 390, 511, 1024, 300, 42, 33, false, true, true, true};
  const auto block = baseBlock(wideSync);
  CHECK(parseEdid(block.data(), block.size(), info) == Edid::Ok);
  CHECK(sameTiming(info.modes[0].timing, wideSync));

  const auto bytes = multiBlockEdid();
  CHECK(parseEdid(bytes.data(), bytes.size(), info) == Edid::Ok);
  CHECK(info.extensionCount == 3 && info.modeCount == 6);
  CHECK(info.hasVic(16) && info.nativeVic(16) && info.hasVic(64) && info.nativeVic(64));
  CHECK(info.hasVic(193) && !info.nativeVic(193) && !info.hasVic(0) && !info.hasVic(255));
  CHECK(!info.nativeVic(97) && info.modes[3].vic == 97 && info.modes[4].vic == 95 && info.modes[5].vic == 16);
  CHECK(sameTiming(info.modes[0].timing, k4k60Rb) && sameTiming(info.modes[1].timing, k1440p60));
  CHECK(!info.modes[2].vic && sameTiming(info.modes[2].timing, k1080p60));
  CHECK(sameTiming(info.modes[3].timing, k4k60Cta) && sameTiming(info.modes[4].timing, k4k30));
  CHECK(sameTiming(info.modes[5].timing, k1080p60));

  auto fullBase = baseBlock(k1080p60, 3);
  for (size_t offset = 72; offset <= 108; offset += 18) putDtd(fullBase, offset, k1440p60);
  seal(fullBase.data());
  Block fullCta{2, 3, 10, 2, 0x45, 16, 95, 97, 105, 107};
  for (size_t offset = 10; offset <= 100; offset += 18) putDtd(fullCta, offset, k4k30);
  seal(fullCta.data());
  const auto full = wireEdid({fullBase, fullCta, fullCta, fullCta});
  CHECK(parseEdid(full.data(), full.size(), info) == Edid::Ok && info.modeCount == EdidInfo::kMaxModes);

  for (const uint8_t revision : {1, 2, 3}) {
    Block cta{2, revision, 4};
    putDtd(cta, 4, k1440p60);
    seal(cta.data());
    const auto combined = wireEdid({baseBlock(k1080p60, 1), cta});
    CHECK(parseEdid(combined.data(), combined.size(), info) == Edid::Ok);
    CHECK(info.modeCount == 2 && sameTiming(info.modes[1].timing, k1440p60));
  }
  Block emptyCta{2, 3, 0};
  seal(emptyCta.data());
  const auto empty = wireEdid({baseBlock(k1080p60, 1), emptyCta});
  CHECK(parseEdid(empty.data(), empty.size(), info) == Edid::Ok && info.modeCount == 1);
}

static void testMalformedEdid()
{
  const auto good = multiBlockEdid();
  EdidInfo info;
  CHECK(parseEdid(good.data(), good.size(), info) == Edid::Ok);
  CHECK(parseEdid(nullptr, good.size(), info) == Edid::InvalidSize && !info.modeCount);
  CHECK(parseEdid(good.data(), std::numeric_limits<size_t>::max(), info) == Edid::InvalidSize);
  // Test exact short buffers, so a stray read crosses the ASan allocation boundary.
  for (size_t length = 0; length < good.size(); ++length) {
    const std::vector<uint8_t> shortEdid(good.begin(), good.begin() + length);
    CHECK(parseEdid(shortEdid.data(), shortEdid.size(), info) == Edid::InvalidSize);
  }
  auto oversized = good;
  oversized.resize(5 * kEdidBlockBytes);
  oversized[126] = 4;
  seal(oversized.data());
  CHECK(parseEdid(oversized.data(), oversized.size(), info) == Edid::InvalidSize);

  auto mutate = [&](size_t offset, uint8_t value, Edid expected) {
    auto bytes = good;
    bytes[offset] = value;
    seal(bytes.data() + offset / kEdidBlockBytes * kEdidBlockBytes);
    const Edid status = parseEdid(bytes.data(), bytes.size(), info);
    if (status != expected) {
      std::printf("EDID byte %zu = %#x returned %u, expected %u\n", offset, unsigned(value), unsigned(status), unsigned(expected));
      ++failures;
    }
    CHECK(!info.modeCount && !info.manufacturer[0] && !info.extensionCount);
  };
  mutate(1, 0x00, Edid::BadHeader);
  mutate(19, 3, Edid::UnsupportedVersion);
  mutate(8, 0xff, Edid::BadManufacturer);
  mutate(126, 255, Edid::InvalidSize);
  mutate(126, 0, Edid::InvalidSize);
  mutate(54 + 9, 0, Edid::BadTiming); // zero sync width
  mutate(54 + 8, 250, Edid::BadTiming); // sync beyond blanking
  for (size_t block = 0; block < good.size(); block += kEdidBlockBytes) {
    auto bytes = good;
    bytes[block + 127] ^= 1;
    CHECK(parseEdid(bytes.data(), bytes.size(), info) == Edid::BadChecksum);
  }
  auto zeroClock = baseBlock(k1080p60);
  zeroClock[54] = zeroClock[55] = 0;
  seal(zeroClock.data());
  CHECK(parseEdid(zeroClock.data(), zeroClock.size(), info) == Edid::BadTiming);
  zeroClock = baseBlock(k1080p60);
  putDtd(zeroClock, 72, k1440p60);
  zeroClock[72] = zeroClock[73] = 0;
  seal(zeroClock.data());
  CHECK(parseEdid(zeroClock.data(), zeroClock.size(), info) == Edid::BadTiming);

  const size_t cta = kEdidBlockBytes;
  mutate(cta, 0, Edid::BadExtension);
  mutate(cta + 1, 0, Edid::BadExtension);
  for (const uint8_t offset : {0, 1, 3, 110, 126, 127, 128, 255})
    mutate(cta + 2, offset, Edid::BadExtension);
  mutate(cta + 2, 13, Edid::BadTiming); // offset inside the first DTD
  mutate(cta + 4, 0x3f, Edid::BadExtension); // DBC length crosses DTD boundary
  mutate(cta + 8, 0, Edid::BadExtension); // nonzero bytes after padding
  mutate(cta + 9, 0x80, Edid::BadExtension); // reserved SVD
  mutate(cta + 12 + 9, 0, Edid::BadTiming);
  mutate(cta + 126, 1, Edid::BadExtension); // nonzero tail within a partial DTD

  // Exercise checksum-valid corruptions as well as arbitrary bytes and lengths.
  uint32_t random = 0x6bbf29a1;
  auto next = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (unsigned iteration = 0; iteration < 5000; ++iteration) {
    std::vector<uint8_t> bytes(next() % (kEdidMaxBytes + 17));
    for (uint8_t &byte : bytes) byte = uint8_t(next());
    CHECK(parseEdid(bytes.data(), bytes.size(), info) != Edid::Ok);
    bytes = good;
    for (unsigned change = 0; change < 8; ++change) {
      const size_t offset = next() % bytes.size();
      bytes[offset] = uint8_t(next());
    }
    for (size_t offset = 0; offset < bytes.size(); offset += kEdidBlockBytes) seal(bytes.data() + offset);
    if (parseEdid(bytes.data(), bytes.size(), info) == Edid::Ok) {
      CHECK(info.modeCount > 0 && info.modeCount <= EdidInfo::kMaxModes);
      CHECK(validDpTiming(info.modes[0].timing));
    } else {
      CHECK(!info.modeCount);
    }
  }
}

static void testModeBudget()
{
  const auto bytes = multiBlockEdid();
  EdidInfo info;
  CHECK(parseEdid(bytes.data(), bytes.size(), info) == Edid::Ok);
  for (unsigned pin : {3, 4, 5, 6}) {
    const DpLink link{kDpHbr2Khz, pinInfo(uint8_t(pin)).lanes};
    const bool fourLanes = pin == 3 || pin == 5;
    CHECK(edidModeFits(k4k60Rb, link) == fourLanes && edidModeFits(k4k60Cta, link) == fourLanes);
    for (const auto &mode : {k1440p60, k4k30, k1080p60}) CHECK(edidModeFits(mode, link));
    CHECK(preferredFittingMode(info, link) == &info.modes[fourLanes ? 0 : 1]);
  }
  CHECK(preferredFittingMode(info, {kDpRbrKhz, 4}) == &info.modes[2]);
  CHECK(edidModeFits(k1080p60, {kDpRbrKhz, 4}) && !edidModeFits(k1080p60, {kDpRbrKhz, 2}));
  CHECK(!edidModeFits(k1440p60, {kDpRbrKhz, 4}) && !edidModeFits(k4k30, {kDpRbrKhz, 4}));
  CHECK(!edidModeFits(k4k60Rb, {kDpHbrKhz, 4}));
  CHECK(!preferredFittingMode(info, {kDpRbrKhz, 1}));
  CHECK(!edidModeFits(k1080p60, {810000, 4}) && !edidModeFits(k1080p60, {kDpHbr2Khz, 3}));

  Block cta{2, 3, 8, 1, 0x43, 97, 95, 16};
  seal(cta.data());
  const auto ctaOnlyFallback = wireEdid({baseBlock(k4k60Rb, 1), cta});
  CHECK(parseEdid(ctaOnlyFallback.data(), ctaOnlyFallback.size(), info) == Edid::Ok);
  const DpLink link{kDpHbr2Khz, 2};
  const EdidMode *mode = preferredFittingMode(info, link);
  CHECK(mode && mode->vic == 95);
  DpMsaTu stream;
  CHECK(mode && computeDpMsaTu(mode->timing, link, stream) && stream.validSymbols == 53);
}

static bool qmpDriveExists(uint32_t rate, DpDrive d)
{
  if (d.swing > 3 || d.preemphasis > 3) return false;
  const auto &swing = rate >= kDpHbr2Khz ? piano_dp_qmp_swing_hbr3_hbr2 : piano_dp_qmp_swing_hbr_rbr;
  const auto &pre = rate >= kDpHbr2Khz ? piano_dp_qmp_pre_emphasis_hbr3_hbr2 : piano_dp_qmp_pre_emphasis_hbr_rbr;
  return swing[d.swing][d.preemphasis] <= 0x3f && pre[d.swing][d.preemphasis] <= 0x3f;
}

using StatusBytes = std::array<uint8_t, 6>;
static constexpr StatusBytes kCrOk{0x11, 0x11};
static constexpr StatusBytes kEqOk{0x77, 0x77, 1};
static constexpr StatusBytes kCrPending{};

struct Script {
  DpLink link;
  std::vector<StatusBytes> cr{kCrOk};
  std::vector<StatusBytes> eq{kEqOk};
};

struct Fault {
  bool isWrite;
  uint32_t addr;
  Pattern pattern;
  Aux result;
  size_t size = 0;
  bool used = false;
};

// The sink checks wire transactions and indexes each scripted status by link,
// training phase, and poll. A short status ACK is represented explicitly.
struct Sink {
  explicit Sink(std::initializer_list<Script> attempts) : script(attempts) {}

  std::vector<Script> script;
  std::array<uint8_t, 16> caps{0x14, 0x14, 0xc4, 0x01, 0, 0, 1};
  std::vector<Fault> faults;
  std::vector<Pattern> patterns;
  std::vector<DpDrive> drives;
  std::vector<unsigned> delays;
  Pattern current = Pattern::Disabled;
  size_t attempt = 0, polls = 0;
  unsigned reads = 0, writes = 0, disableWrites = 0;
  bool failPhy = false;

  Aux inject(bool isWrite, uint32_t addr, size_t size)
  {
    for (auto &fault : faults) {
      if (fault.isWrite == isWrite && fault.addr == addr && fault.pattern == current &&
          (!fault.size || fault.size == size) && !fault.used) {
        fault.used = true;
        return fault.result;
      }
    }
    return Aux::Ack;
  }

  Aux auxRead(uint32_t addr, uint8_t *data, size_t size)
  {
    ++reads;
    if (const auto fault = inject(false, addr, size); fault != Aux::Ack) return fault;
    if (addr == 0 && size == caps.size()) {
      std::copy(caps.begin(), caps.end(), data);
      return Aux::Ack;
    }
    if (addr == 0x202 && size == 6 && attempt && attempt <= script.size()) {
      const auto &step = script[attempt - 1];
      const auto &responses = current == Pattern::Tps1 ? step.cr : step.eq;
      if (polls < responses.size()) {
        const auto &response = responses[polls++];
        std::copy(response.begin(), response.end(), data);
        return Aux::Ack;
      }
    }
    CHECK(false && "unexpected AUX read or excessive polls");
    return Aux::IoError;
  }

  Aux auxWrite(uint32_t addr, const uint8_t *data, size_t size)
  {
    ++writes;
    if (addr == 0x102 && size == 1 && data[0] == 0) ++disableWrites;
    if (const auto fault = inject(true, addr, size); fault != Aux::Ack) return fault;
    if (addr == 0x100 && size == 2 && attempt < script.size()) {
      const DpLink link = script[attempt++].link;
      CHECK(uint32_t(data[0]) * 27000 == link.rateKhz);
      CHECK((data[1] & 0x1f) == link.lanes && (data[1] & 0x80) == (caps[2] & 0x80));
      return Aux::Ack;
    }
    if (addr == 0x107 && size == 2) {
      CHECK(data[0] == (caps[3] & 1 ? 0x10 : 0) && data[1] == 1);
      return Aux::Ack;
    }
    if (addr == 0x102 && size == 1 && data[0] == 0) return Aux::Ack;
    if ((addr == 0x102 || addr == 0x103) && attempt && !drives.empty()) {
      const auto link = script[attempt - 1].link;
      size_t firstLane = 0;
      if (addr == 0x102 && size) {
        const uint8_t encoded[] = {0, 0x21, 0x22, 0x23, 7};
        CHECK(data[0] == encoded[unsigned(current)]);
        firstLane = 1;
      }
      CHECK(size == firstLane + link.lanes);
      const auto [swing, pre] = drives.back();
      const unsigned laneSet = swing | (swing == 3 ? 4 : 0) | (pre << 3) | (pre == 3 - swing ? 32 : 0);
      for (size_t i = firstLane; i < size; ++i) CHECK(data[i] == laneSet);
      return Aux::Ack;
    }
    CHECK(false && "unexpected AUX write");
    return Aux::IoError;
  }

  bool setPhy(uint32_t rate, unsigned lanes, unsigned swing, unsigned pre)
  {
    CHECK(validDpLink({rate, lanes}) && qmpDriveExists(rate, {swing, pre}));
    const size_t nextAttempt = current == Pattern::Disabled ? attempt : attempt - 1;
    CHECK(nextAttempt < script.size());
    if (nextAttempt < script.size()) {
      CHECK(rate == script[nextAttempt].link.rateKhz && lanes == script[nextAttempt].link.lanes);
    }
    if (current == Pattern::Disabled) CHECK(!swing && !pre);
    drives.push_back({swing, pre});
    return !failPhy;
  }

  void setPattern(Pattern pattern)
  {
    current = pattern;
    patterns.push_back(pattern);
    polls = 0;
  }

  void delayUs(unsigned delay) { delays.push_back(delay); }
};

static void testTrainSuccess()
{
  {
    Sink sink{{{kDpHbr2Khz, 4}}};
    const auto result = trainDpLink(sink, 4);
    CHECK(result.status == Train::Trained && result.auxStatus == Aux::Ack && result.attempts == 1);
    CHECK(result.link.rateKhz == kDpHbr2Khz && result.link.lanes == 4);
    CHECK(result.enhancedFraming && result.downspread);
    CHECK((sink.patterns == std::vector{Pattern::Tps1, Pattern::Tps3, Pattern::Disabled}));
    CHECK((sink.delays == std::vector{100u, 400u}));
    CHECK(sink.reads == 3 && sink.writes == 5 && sink.disableWrites == 1);
  }
  {
    StatusBytes requestSwing2{0x11, 0x01, 0, 0, 0xfe}; // lane 3 lacks CR, lane 0 requests (2,3)
    StatusBytes requestEq{0x77, 0x77, 0, 0, 0x0d};     // EQ bits set but alignment pending
    Sink sink{{{kDpHbr2Khz, 4},
               {kCrPending, kCrPending, kCrPending, requestSwing2, kCrOk},
               {requestEq, kEqOk}}};
    CHECK(trainDpLink(sink, 4).status == Train::Trained);
    CHECK((sink.drives == std::vector<DpDrive>{{0, 0}, {2, 1}, {1, 2}}));
    CHECK(sink.delays.size() == 7);
  }
  {
    Sink sink{{{kDpHbr2Khz, 2}, {{0x11}}, {{0x77, 0, 1}}}};
    sink.caps[1] = 0x1e;  // HBR3 sink, piano's PHY must stay within HBR2
    sink.caps[3] = 0x80;  // TPS4, no downspread
    sink.caps[14] = 0x82; // extended cap flag does not change EQ interval
    const auto result = trainDpLink(sink, 2);
    CHECK(result.status == Train::Trained && result.link.rateKhz == kDpHbr2Khz && result.link.lanes == 2);
    CHECK(!result.downspread && sink.patterns[1] == Pattern::Tps4);
    CHECK((sink.delays == std::vector{100u, 8000u}));
  }
  {
    Sink sink{{{kDpRbrKhz, 1}, {{1}}, {{7, 0, 1}}}};
    sink.caps[0] = 0x11;
    sink.caps[1] = 0x06;
    sink.caps[2] = 1;
    sink.caps[14] = 1;
    CHECK(trainDpLink(sink, 4).status == Train::Trained && sink.patterns[1] == Pattern::Tps2);
    CHECK((sink.delays == std::vector{4000u, 4000u}));
  }
}

static void testTrainFallback()
{
  const std::vector<StatusBytes> noCr(4, kCrPending);
  const std::vector<StatusBytes> noEq(6, StatusBytes{0x33, 0x33, 1}); // symbol lock absent
  {
    Sink sink{{{kDpHbr2Khz, 4}, noCr}, {{kDpHbrKhz, 4}, noCr}, {{kDpRbrKhz, 4}}};
    const auto result = trainDpLink(sink, 4);
    CHECK(result.status == Train::Trained && result.attempts == 3 && result.link.rateKhz == kDpRbrKhz);
    CHECK(sink.delays.size() == 10 && sink.disableWrites == 3);
  }
  {
    Sink sink{{{kDpHbr2Khz, 4}, {kCrOk}, noEq}, {{kDpHbr2Khz, 2}, {kCrOk}, noEq}, {{kDpHbr2Khz, 1}}};
    const auto result = trainDpLink(sink, 4);
    CHECK(result.status == Train::Trained && result.attempts == 3 && result.link.lanes == 1);
    CHECK(sink.delays.size() == 16 && sink.disableWrites == 3);
  }
  {
    Sink sink{{{kDpHbr2Khz, 4}, noCr}, {{kDpHbrKhz, 4}, noCr}, {{kDpRbrKhz, 4}, noCr}, {{kDpHbr2Khz, 2}, noCr}};
    const auto result = trainDpLink(sink, 4);
    CHECK(result.status == Train::ClockRecoveryFailed && result.attempts == 4);
    CHECK(result.link.rateKhz == kDpHbr2Khz && result.link.lanes == 2 && sink.delays.size() == 16);
    CHECK(sink.disableWrites == 4 && sink.current == Pattern::Disabled);
  }
  {
    Sink sink{{{kDpHbr2Khz, 4}, {kCrOk}, {{0x77, 0x07, 1}}}, {{kDpHbrKhz, 4}}};
    const auto result = trainDpLink(sink, 4);
    CHECK(result.status == Train::Trained && result.attempts == 2 && result.link.rateKhz == kDpHbrKhz);
  }
  {
    Sink sink{{{kDpRbrKhz, 1}, {{0, 0, 0, 0, 0x0f}, kCrPending}}};
    sink.caps[1] = 0x06;
    CHECK(trainDpLink(sink, 1).status == Train::ClockRecoveryFailed);
    CHECK((sink.drives == std::vector<DpDrive>{{0, 0}, {3, 0}}));
    CHECK(sink.delays.size() == 2);
  }
  {
    std::vector<StatusBytes> oscillation;
    for (unsigned i = 0; i < 16; ++i) oscillation.push_back({0, 0, 0, 0, uint8_t((i + 1) % 2)});
    Sink sink{{{kDpRbrKhz, 1}, oscillation}};
    sink.caps[1] = 0x06;
    CHECK(trainDpLink(sink, 1).status == Train::ClockRecoveryFailed);
    CHECK(sink.delays.size() == 16 && sink.disableWrites == 1);
  }
}

static void testTrainErrors()
{
  DpReceiverCaps caps;
  std::array<uint8_t, 16> bytes{0x14, 0x14, 4};
  CHECK(parseDpReceiverCaps(bytes.data(), bytes.size(), caps));
  CHECK(caps.revision == 0x14 && caps.maxRateKhz == kDpHbr2Khz && caps.maxLanes == 4);
  CHECK(!parseDpReceiverCaps(nullptr, bytes.size(), caps));
  CHECK(!parseDpReceiverCaps(bytes.data(), 15, caps));
  for (const auto field : {0, 1, 2, 14}) {
    auto bad = bytes;
    bad[field] = 0xff;
    if (field == 0) bad[field] = 0;
    CHECK(!parseDpReceiverCaps(bad.data(), bad.size(), caps) && !caps.maxRateKhz);
  }
  {
    Sink sink{};
    CHECK(trainDpLink(sink, 3).status == Train::InvalidPinLanes && !sink.reads);
    sink.caps[2] = 3;
    CHECK(trainDpLink(sink, 4).status == Train::InvalidCaps && !sink.writes);
  }
  for (const auto &fault : {Fault{false, 0, Pattern::Disabled, Aux::Nack},
                            Fault{true, 0x100, Pattern::Disabled, Aux::Timeout},
                            Fault{true, 0x107, Pattern::Disabled, Aux::Nack},
                            Fault{true, 0x102, Pattern::Tps1, Aux::Nack},
                            Fault{false, 0x202, Pattern::Tps1, Aux::ShortTransfer},
                            Fault{false, 0x202, Pattern::Tps3, Aux::Timeout}}) {
    Sink sink{{{kDpHbr2Khz, 4}}};
    sink.faults.push_back(fault);
    const auto result = trainDpLink(sink, 4);
    CHECK(result.status == Train::AuxError && result.auxStatus == fault.result && result.attempts <= 1);
    CHECK(sink.current == Pattern::Disabled && sink.faults[0].used);
    CHECK(sink.disableWrites == unsigned(fault.pattern != Pattern::Disabled));
  }
  {
    Sink sink{{{kDpHbr2Khz, 4}}};
    sink.faults = {{false, 0x202, Pattern::Tps1, Aux::Timeout}, {true, 0x102, Pattern::Tps1, Aux::Nack, 1}};
    const auto result = trainDpLink(sink, 4);
    CHECK(result.status == Train::AuxError && result.auxStatus == Aux::Timeout);
    CHECK(sink.current == Pattern::Disabled && sink.disableWrites == 1 && sink.faults[1].used);
  }
  {
    Sink sink{{{kDpHbr2Khz, 4}}};
    sink.faults = {{true, 0x102, Pattern::Tps3, Aux::Nack, 1}};
    const auto result = trainDpLink(sink, 4);
    CHECK(result.status == Train::AuxError && result.auxStatus == Aux::Nack);
    CHECK(sink.current == Pattern::Disabled && sink.disableWrites == 1 && sink.faults[0].used);
  }
  {
    Sink sink{{{kDpHbr2Khz, 4}}};
    sink.failPhy = true;
    CHECK(trainDpLink(sink, 4).status == Train::PhyError && !sink.writes);
  }
}

static void testMsaTu()
{
  struct Case {
    DpTiming mode;
    DpLink link;
    unsigned hTotal, vTotal, hStart, vStart;
    unsigned mvid, validSymbols, wholeSymbols;
  };
  constexpr Case cases[] = {
    {k1080p60, {kDpRbrKhz, 4}, 2200, 1125, 192, 41, 30037, 44, 44},
    {k1440p60, {kDpHbr2Khz, 2}, 2720, 1481, 112, 38, 14655, 43, 42},
    {k4k30, {kDpHbr2Khz, 2}, 4400, 2250, 384, 82, 18022, 53, 52},
    {k4k60Rb, {kDpHbr2Khz, 4}, 4000, 2222, 112, 59, 32358, 48, 47},
  };
  DpMsaTu out;
  for (const auto &c : cases) {
    CHECK(computeDpMsaTu(c.mode, c.link, out));
    CHECK(out.hTotal == c.hTotal && out.vTotal == c.vTotal && out.hStart == c.hStart && out.vStart == c.vStart);
    CHECK(out.hBlank && out.vBlank && out.hActive == c.mode.hActive && out.vActive == c.mode.vActive);
    CHECK(out.hSyncWidth == c.mode.hSyncWidth && out.vSyncWidth == c.mode.vSyncWidth);
    CHECK(out.hSyncPositive == c.mode.hSyncPositive && out.vSyncPositive == c.mode.vSyncPositive);
    CHECK(out.misc0 == 0x20 && out.misc1 == 0 && out.nvid == 32768 && out.mvid == c.mvid);
    CHECK(out.tuSize == 64 && out.validSymbols == c.validSymbols && out.validSymbols <= 64);
    CHECK(out.validSymbolsFloor == c.wholeSymbols && out.validSymbolsNumerator < out.validSymbolsDenominator);
    CHECK(uint64_t(out.validSymbolsFloor) * out.validSymbolsDenominator + out.validSymbolsNumerator ==
          uint64_t(c.mode.pixelKhz) * 24 * 64);
  }
  CHECK(!computeDpMsaTu(k4k60Rb, {kDpHbr2Khz, 2}, out) && !out.tuSize && !out.mvid);
  CHECK(!computeDpMsaTu(k4k60Rb, {0, 4}, out));
  CHECK(!computeDpMsaTu(k4k60Rb, {kDpHbr2Khz, 0}, out));
  auto bad = k1080p60;
  bad.pixelKhz = UINT32_MAX;
  CHECK(!computeDpMsaTu(bad, {kDpHbr2Khz, 4}, out));
  bad.pixelKhz = 0;
  CHECK(!computeDpMsaTu(bad, {kDpHbr2Khz, 4}, out));
  bad = k1080p60;
  bad.hActive = UINT16_MAX;
  CHECK(!computeDpMsaTu(bad, {kDpHbr2Khz, 4}, out));
  bad = k1080p60;
  bad.vBlank = 0;
  CHECK(!computeDpMsaTu(bad, {kDpHbr2Khz, 4}, out));
  bad = k1080p60;
  bad.interlaced = true;
  CHECK(!computeDpMsaTu(bad, {kDpHbr2Khz, 4}, out));
  bad = k1080p60;
  bad.separateSync = false;
  CHECK(!computeDpMsaTu(bad, {kDpHbr2Khz, 4}, out));
  auto edge = k1080p60;
  edge.pixelKhz = 348750; // 62 symbols exactly on two HBR2 lanes
  CHECK(computeDpMsaTu(edge, {kDpHbr2Khz, 2}, out) && out.validSymbols == 62 && !out.validSymbolsNumerator);
  ++edge.pixelKhz;
  CHECK(edidModeFits(edge, {kDpHbr2Khz, 2}) && !computeDpMsaTu(edge, {kDpHbr2Khz, 2}, out));
}

static void testQmpTables()
{
  static_assert(piano_dp_qmp_piano_max_mbps * 100 == kDpHbr2Khz);
  static_assert(piano_dp_qmp_rates[0].link_clk_hz == kDpRbrKhz * 1000);
  static_assert(piano_dp_qmp_rates[1].link_clk_hz == kDpHbrKhz * 1000);
  static_assert(piano_dp_qmp_rates[2].link_clk_hz == kDpHbr2Khz * 1000);
  for (uint32_t rate : {kDpRbrKhz, kDpHbrKhz, kDpHbr2Khz}) {
    for (unsigned swing = 0; swing <= 3; ++swing) {
      for (unsigned pre = 0; pre <= 3; ++pre) {
        CHECK(qmpDriveExists(rate, {swing, pre}) == (swing + pre <= 3));
        const DpDrive drive = clampDpDrive(swing, pre);
        CHECK(drive.swing == swing && drive.preemphasis == std::min(pre, 3 - swing));
        CHECK(qmpDriveExists(rate, drive));
      }
    }
  }
  CHECK((clampDpDrive(UINT32_MAX, UINT32_MAX) == DpDrive{3, 0}));
}

int main()
{
  testEdidTimings();
  testMalformedEdid();
  testModeBudget();
  testTrainSuccess();
  testTrainFallback();
  testTrainErrors();
  testMsaTu();
  testQmpTables();
  std::printf("PianoDp D0 host tests: %s\n", failures ? "failed" : "passed");
  return failures != 0;
}
