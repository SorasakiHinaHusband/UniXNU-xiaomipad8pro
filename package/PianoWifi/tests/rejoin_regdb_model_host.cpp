// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. rejoin_regdb_model_host.cpp -o /tmp/rejoin_regdb_model_host && /tmp/rejoin_regdb_model_host
#include "WifiApple80211.h"
#include "WifiDp.h"
#include "WifiSta.h"
#include "WifiUserControl.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace pianowifi;
namespace apple = pianowifi::a80211;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); std::exit(1); \
} } while (0)

namespace {
constexpr uint8_t kStaMac[] = {2, 0, 0, 0, 0, 1};
constexpr uint8_t kApMac[] = {2, 0, 0, 0, 0, 2};
constexpr uint8_t kSsid[] = {'m', 'o', 'd', 'e', 'l'};
// WPA2-PSK with CCMP and MFP capable. Check the AP's bytes, including the RSN capabilities.
constexpr uint8_t kRsn[] = {48, 20, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 2, 0x80, 0};
constexpr uint16_t kCapability = kDot11CapEss | kDot11CapPrivacy | kDot11CapShortSlot;

struct FrontEnd {
  apple::State state;
  alignas(uint32_t) uint8_t scratch[apple::kScratchBytes];

  apple::Answer ask(bool set, int type, void *data = nullptr, uint32_t size = 0)
  {
    const apple::UserIo io{
        nullptr,
        [](void *, uint64_t from, void *to, uint32_t n) { std::memcpy(to, reinterpret_cast<const void *>(from), n); return 0; },
        [](void *, const void *from, uint64_t to, uint32_t n) { std::memcpy(reinterpret_cast<void *>(to), from, n); return 0; }};
    apple::Req req{};
    req.type = type;
    req.length = size;
    req.data = reinterpret_cast<uintptr_t>(data);
    return apple::handle(state, {"host", "model", "en0"}, set, req, io, scratch);
  }

  apple::ScanResult readBss(int type)
  {
    uint8_t bytes[apple::kScanResultBytes27];
    const auto answer = ask(false, type, bytes, sizeof bytes);
    CHECK(!answer.error && answer.wrote == sizeof bytes);
    apple::ScanResult result;
    std::memcpy(&result, bytes, sizeof result);
    return result;
  }
};

apple::AssociateRequest27 savedNetwork()
{
  apple::AssociateRequest27 request{};
  request.version = 1;
  request.mode = 2;
  request.auth_lower = 1;
  request.auth_upper = 0x08;
  request.network.ssid_length = sizeof kSsid;
  std::memcpy(request.network.ssid, kSsid, sizeof kSsid);
  request.network.key_length = 8;
  request.network.key_cipher = 10;
  std::memset(request.network.key, '0', request.network.key_length);
  return request;
}

// These are subsets of channels in synthetic firmware events. The country and
// rules travel together, with no firmware binary or system regdb dependency.
RegDomain firmwareDomain(const char *alpha2, uint32_t countryId, uint32_t domainCode, bool extended,
                         std::initializer_list<RegRule> rules)
{
  uint8_t message[512];
  WmiWriter writer;
  wmiBegin(writer, message, sizeof message, extended ? uint32_t(kWmiEvtRegChanListCcExt) : uint32_t(kWmiEvtRegChanListCc));
  auto *header = wmiStruct(writer, extended ? kWmiTagRegChanListCcExtEvent : kWmiTagRegChanListCcEvent,
                           extended ? kRegExtEventWords : kRegEventWords);
  wmiPut(header, 2, uint32_t(uint8_t(alpha2[0])) | uint32_t(uint8_t(alpha2[1])) << 8);
  wmiPut(header, 3, 1);
  wmiPut(header, 4, countryId);
  wmiPut(header, 5, domainCode);
  wmiPut(header, 12, 1); // the first rule is for 2.4 GHz
  wmiPut(header, 13, uint32_t(rules.size()) - 1);
  const uint32_t words = extended ? kRegRuleExtWords : kRegRuleWords;
  const uint32_t stride = (words + 1) * 4;
  uint8_t *entry = wmiTlv(writer, kWmiTagArrayStruct, uint32_t(rules.size()) * stride);
  CHECK(entry && rules.size());
  for (const auto &rule : rules) {
    wmiPut(entry, 0, words * 4 | uint32_t(extended ? kWmiTagRegRuleExt : kWmiTagRegRule) << kWmiTlvTagShift);
    wmiPut(entry, 1, rule.startKhz | rule.endKhz << 16);
    wmiPut(entry, 2, rule.maxBwMhz | uint32_t(uint8_t(rule.powerDbm)) << 16);
    wmiPut(entry, 3, rule.flags);
    entry += stride;
  }
  RegDomain reg;
  CHECK(regParse(reg, message + kWmiCmdHeaderBytes, wmiEnd(writer) - kWmiCmdHeaderBytes, extended));
  CHECK(reg.seen && reg.extended == extended && !reg.status && reg.countryId == countryId && reg.domainCode == domainCode);
  CHECK(reg.ruleCount == rules.size() && !reg.rulesDropped);
  return reg;
}

void testRegulatory()
{
  FrontEnd front;
  const RegDomain china = firmwareDomain("CN", 156, 1, true, {
      {2402, 2482, 40, 20, 0}, {5735, 5775, 40, 23, 0}, {5170, 5189, 20, 23, 0}});
  const RegDomain usa = firmwareDomain("US", 840, 2, false, {
      {2402, 2472, 40, 20, 0}, {5170, 5210, 40, 23, 0}, {5250, 5270, 20, 23, kRegRuleFlagRadar}});
  const RegDomain japan = firmwareDomain("JP", 392, 3, true, {
      {2402, 2482, 40, 20, 0}, {5170, 5190, 20, 18, kRegRuleFlagNoIr},
      {5190, 5210, 20, 18, kRegRuleFlagDisabled}, {5210, 5230, 10, 18, 0}});

  struct Expected {
    const RegDomain &reg;
    const char *country;
    uint32_t lower, passive, radar;
    std::vector<uint32_t> upper;
  };
  for (const auto &test : {Expected{china, "CN", 13, 0, 0, {5745, 5765}},
                           Expected{usa, "US", 11, 1, 1, {5180, 5200, 5260}},
                           Expected{japan, "JP", 13, 1, 0, {5180}}}) {
    CHECK(std::memcmp(test.reg.alpha2, test.country, 3) == 0);
    apple::setRegulatoryCountry(front.state.country, test.reg.alpha2, 2);
    char country[3];
    const auto get = front.ask(false, apple::kCountryCode, country, sizeof country);
    CHECK(!get.error && get.wrote == 3 && std::memcmp(country, test.country, 3) == 0);
    uint8_t set[8] = {1, 0, 0, 0, uint8_t(test.country[0]), uint8_t(test.country[1]), 0, 0};
    CHECK(!front.ask(true, apple::kCountryCode, set, sizeof set).error);
    set[4] = set[5] = 'Z';
    CHECK(front.ask(true, apple::kCountryCode, set, sizeof set).error == apple::kErrUnsupported);
    CHECK(!front.ask(false, apple::kCountryCode, country, sizeof country).error);
    CHECK(std::memcmp(country, test.country, 3) == 0);

    ChanList channels;
    chanListBuild(channels, test.reg, true);
    CHECK(channels.count == test.lower + test.upper.size() && channels.left24 == 13 - test.lower);
    CHECK(channels.left5 == std::size(kAssocChannels5) - test.upper.size());
    CHECK(channels.passive == test.passive && channels.radar == test.radar);
    for (uint32_t i = 0; i < test.lower; ++i) CHECK(channels.entry[i].freq == 2412 + 5 * i);
    for (size_t i = 0; i < test.upper.size(); ++i) {
      const auto &channel = channels.entry[test.lower + i];
      CHECK(channel.freq == test.upper[i]);
      const bool last = i + 1 == test.upper.size();
      CHECK(bool(channel.info & kWmiChanInfoPassive) == (last && test.passive));
      CHECK(bool(channel.info & kWmiChanInfoDfs) == (last && test.radar));
      const uint32_t halfDbm = test.reg.countryId == 392 ? 36 : 46;
      CHECK(channel.reg1 == (halfDbm << 8 | halfDbm << 16));
    }
  }

  for (const char *invalid : {"cN", "U1", "J?", "0\x80"}) {
    const auto reg = firmwareDomain(invalid, 0, 0, false, {{2402, 2472, 20, 18, 0}});
    apple::setRegulatoryCountry(front.state.country, reg.alpha2, 2);
    char country[3];
    CHECK(!front.ask(false, apple::kCountryCode, country, sizeof country).error);
    CHECK(std::memcmp(country, "XX", 3) == 0);
    uint8_t set[8] = {1, 0, 0, 0, uint8_t(invalid[0]), uint8_t(invalid[1]), 0, 0};
    CHECK(front.ask(true, apple::kCountryCode, set, sizeof set).error == apple::kErrInval);
    ChanList channels;
    chanListBuild(channels, reg, true);
    CHECK(channels.count == 11 && channels.left5 == std::size(kAssocChannels5));
  }
}

// Minimal firmware/AP for one station. Run the station's real WMI outbox,
// event parsers, management frames and teardown on a bounded logical clock.
struct Station {
  Sta sta;
  ScanList scan;
  const RegDomain reg = firmwareDomain("CN", 156, 1, true, {{2402, 2482, 40, 20, 0}, {5170, 5250, 80, 23, 0}});
  ChanList channels;
  uint8_t frames[kAssocFrameBuffers][kAssocFrameBytes];
  uint64_t now = 1;
  uint32_t airFreq = 5180, startFreq = 0, pendingScan = 0;
  std::vector<uint32_t> commands, scanFreqs;

  Station() { chanListBuild(channels, reg, true); }

  void hearBeacon()
  {
    uint8_t frame[kDot11HeaderBytes + 12 + 2 + sizeof kSsid + 3 + sizeof kRsn]{};
    frame[0] = kDot11SubtypeBeacon;
    std::memcpy(frame + 16, kApMac, sizeof kApMac);
    auto *fixed = frame + kDot11HeaderBytes;
    fixed[8] = 100;
    fixed[10] = uint8_t(kCapability);
    fixed[11] = uint8_t(kCapability >> 8);
    auto *ies = fixed + 12;
    ies[1] = sizeof kSsid;
    std::memcpy(ies + 2, kSsid, sizeof kSsid);
    const uint8_t rates[] = {1, 1, 0x8c};
    std::memcpy(ies + 2 + sizeof kSsid, rates, sizeof rates);
    std::memcpy(ies + 2 + sizeof kSsid + sizeof rates, kRsn, sizeof kRsn);
    WmiMgmtRx rx{};
    rx.frame = frame;
    rx.bytes = sizeof frame;
    rx.freq = airFreq;
    rx.channel = apple::channelOfFreq(airFreq);
    rx.signalDbm = -50;
    CHECK(scanAdd(scan, rx));
  }

  void begin(const piano_wifi_profile &profile, bool scanFirst = false, bool kickoutEnds = false)
  {
    sta = {};
    pendingScan = startFreq = 0;
    commands.clear();
    StaOptions options;
    options.flags = kStaFlagDirected | kStaFlagDeauth;
    options.holdMs = options.paceMs = options.rescans = 0;
    options.stepMs = options.frameWaitMs = 50;
    options.scanWaitMs = 100;
    options.scanFirst = scanFirst;
    options.kickoutEnds = kickoutEnds;
    StaEnv env{};
    env.ownMac = kStaMac;
    env.scan = &scan;
    env.reg = &reg;
    env.chanList = &channels;
    for (unsigned i = 0; i < kAssocFrameBuffers; ++i)
      env.frame[i] = {frames[i], 0x20000000 + i * kAssocFrameBytes, kAssocFrameBytes};
    staBegin(sta, options, env, now);
    CHECK(staSetProfile(sta, profile.ssid, profile.ssid_len, profile.security, nullptr));
  }

  void deliver(const WmiWriter &writer)
  {
    const uint32_t size = wmiEnd(writer);
    CHECK(size > kWmiCmdHeaderBytes);
    CHECK(staOnEvent(sta, wmiEventId(writer.buffer, size), writer.buffer + kWmiCmdHeaderBytes, size - kWmiCmdHeaderBytes, now));
  }

  void event(uint32_t id, uint32_t tag, std::initializer_list<uint32_t> words, int macWord = -1)
  {
    uint8_t bytes[64];
    WmiWriter writer;
    wmiBegin(writer, bytes, sizeof bytes, id);
    auto *fixed = wmiStruct(writer, tag, uint32_t(words.size()));
    CHECK(fixed);
    unsigned index = 0;
    for (uint32_t word : words) wmiPut(fixed, index++, word);
    if (macWord >= 0) std::memcpy(fixed + macWord * 4, kApMac, sizeof kApMac);
    deliver(writer);
  }

  void mgmtReply(uint8_t subtype, std::initializer_list<uint8_t> body)
  {
    uint8_t bytes[128];
    WmiWriter writer;
    wmiBegin(writer, bytes, sizeof bytes, kWmiEvtMgmtRx);
    auto *header = wmiStruct(writer, kWmiTagMgmtRxHeader, 17);
    const uint32_t length = kDot11HeaderBytes + uint32_t(body.size());
    wmiPut(header, 4, length);
    wmiPut(header, 16, airFreq);
    auto *frame = wmiTlv(writer, kWmiTagArrayByte, length);
    CHECK(frame);
    frame[0] = subtype;
    std::memcpy(frame + 4, kStaMac, sizeof kStaMac);
    std::memcpy(frame + 10, kApMac, sizeof kApMac);
    std::memcpy(frame + 16, kApMac, sizeof kApMac);
    std::copy(body.begin(), body.end(), frame + kDot11HeaderBytes);
    deliver(writer);
  }

  void turn()
  {
    staRun(sta, ++now);
    if (!sta.outLength) return;
    const uint32_t id = wmiEventId(sta.out, sta.outLength);
    const uint8_t *payload = sta.out + kWmiCmdHeaderBytes;
    const uint32_t length = sta.outLength - kWmiCmdHeaderBytes;
    commands.push_back(id);
    staSent(sta, now);
    uint32_t size = 0;
    switch (id) {
    case kWmiCmdStartScan: {
      const auto *scanCmd = wmiFind(payload, length, kWmiTagStartScanCmd, &size);
      CHECK(scanCmd && size >= 17 * 4);
      pendingScan = wmiGet(scanCmd, 0);
      const auto *freqs = wmiFind(payload, length, kWmiTagArrayUint32, &size);
      CHECK(freqs && size == 4 * wmiGet(scanCmd, 16));
      scanFreqs.clear();
      for (uint32_t i = 0; i < size / 4; ++i) scanFreqs.push_back(wmiGet(freqs, i));
      break;
    }
    case kWmiCmdVdevStart: {
      const auto *channel = wmiFind(payload, length, kWmiTagChannel, &size);
      CHECK(channel && size == kWmiChannelWords * 4);
      startFreq = wmiGet(channel, 0);
      event(kWmiEvtVdevStartResp, kWmiTagVdevStartRespEvent, {0, 0, 0, 0, 0, 0});
      break;
    }
    case kWmiCmdMgmtTxSend: {
      const auto *data = wmiFind(payload, length, kWmiTagArrayByte, &size);
      CHECK(data && size >= kDot11HeaderBytes && std::memcmp(data + 4, kApMac, sizeof kApMac) == 0);
      // The AP is silent on the cached channel after a move.
      if (startFreq != airFreq) break;
      if (data[0] == kDot11Auth) mgmtReply(kDot11Auth, {0, 0, 2, 0, 0, 0});
      else if (data[0] == kDot11AssocRequest) mgmtReply(kDot11AssocResponse, {0x11, 0, 0, 0, 1, 0xc0, 1, 1, 0x8c});
      else CHECK(data[0] == kDot11Deauth);
      break;
    }
    case kWmiCmdPeerAssoc:
      event(kWmiEvtPeerAssocConf, kWmiTagPeerAssocConfEvent, {0, 0, 0}, 1);
      break;
    case kWmiCmdPeerDelete:
      event(kWmiEvtPeerDeleteResp, kWmiTagPeerDeleteRespEvent, {0, 0, 0}, 1);
      break;
    case kWmiCmdVdevStop:
      event(kWmiEvtVdevStopped, kWmiTagVdevStoppedEvent, {0});
      break;
    case kWmiCmdPeerCreate:
    case kWmiCmdVdevUp:
    case kWmiCmdVdevDown:
      break;
    default:
      CHECK(false);
    }
  }

  void runTo(int step)
  {
    for (unsigned i = 0; i < 1000 && sta.step != step && !staFinished(sta); ++i) turn();
    if (sta.step != step)
      std::fprintf(stderr, "expected %s, got %s (error %d)\n", staStepName(step), staStepName(sta.step), sta.error);
    CHECK(sta.step == step);
  }

  void finishScan(bool hearsAp)
  {
    CHECK(pendingScan && !sta.choice.found && !startFreq);
    for (int i = 0; i < 4; ++i) turn();
    CHECK(sta.step == kStaRescan && commands == std::vector<uint32_t>{kWmiCmdStartScan});
    CHECK(std::find(scanFreqs.begin(), scanFreqs.end(), airFreq) != scanFreqs.end());
    if (hearsAp) hearBeacon();
    event(kWmiEvtScan, kWmiTagScanEvent, {kWmiScanEvCompleted, 0, 0, kWmiScanRequestorId, pendingScan, 0});
    pendingScan = 0;
  }

  void joined()
  {
    runTo(kStaAssociated);
    CHECK(sta.vdevUp && sta.assocAnswered && sta.rsn && startFreq == airFreq);
    CHECK(sta.choice.net.rsnElementLength == sizeof kRsn);
    CHECK(std::memcmp(sta.choice.net.rsnElement, kRsn, sizeof kRsn) == 0);
  }

  void checkTeardown() const
  {
    CHECK(sta.downSent && sta.peerDeleted && sta.vdevStopped);
  }
};

// Firmware consumes real refill entries and posts real REO descriptors. Keep
// the same DMA arena and ring positions across station generations.
struct ReceiveRings {
  std::vector<uint64_t> memory = std::vector<uint64_t>((8u << 20) / sizeof(uint64_t));
  HttRx rx;
  Dp dp;
  std::array<uint32_t, 2> doorbells{};
  const ChipIo io{
      this, [](void *, uint32_t) { return 0u; },
      [](void *ctx, uint32_t address, uint32_t value) {
        auto &rings = *static_cast<ReceiveRings *>(ctx);
        if (address == kDpReo2Sw0R2 + 4) rings.doorbells[0] = value;
        if (address == kDpReo2Sw1R2 + 4) rings.doorbells[1] = value;
      }};
  uint32_t consumed = 0;
  uint32_t delivered = 0;
  static constexpr uint32_t kFrameBytes = 64;

  ReceiveRings()
  {
    Arena arena;
    arenaInit(arena, reinterpret_cast<uint8_t *>(memory.data()), 0x1'2340'0000ull, uint32_t(memory.size() * sizeof(uint64_t)));
    CHECK(httPlan(rx, arena, true));
    auto &refill = rx.ring[kHttRefill];
    refill.announced = true;
    httFill(rx);
    CHECK(dpPlan(dp, arena, &rx));
    dpProgram(dp, io);
    // Start empty near the end so the first loss crosses the exception ring wrap.
    dp.reoExc.ours = dp.reoExc.entries - 1;
    *dp.reoExc.theirs = dp.reoExc.ours * dp.reoExc.words;
  }

  uint32_t takeBuffer()
  {
    auto &ring = rx.ring[kHttRefill];
    const uint32_t tail = *ring.tail / ring.entryWords;
    CHECK(tail != *ring.head / ring.entryWords);
    const uint8_t *entry = ring.va + 4 * ring.entryWords * tail;
    const uint32_t cookie = dpdetail::cookieOf(entry);
    CHECK(cookie < kRxRefillEntries - 1 && dpdetail::addrOf(entry) == rx.refillBus[cookie]);
    *ring.tail = (tail + 1) % ring.entries * ring.entryWords;
    ++consumed;
    return cookie;
  }

  void post(CeRing &ring, uint64_t address, uint32_t rbm, uint32_t cookie, uint32_t status, uint32_t msdu = 0)
  {
    const uint32_t head = *ring.theirs / ring.words;
    const uint32_t next = (head + 1) % ring.entries;
    CHECK(next != ring.ours);
    uint8_t *entry = ring.va + 4 * ring.words * head;
    std::memset(entry, 0, 4 * ring.words);
    httdetail::putBuffer(entry, address, rbm, cookie);
    wmiPut(entry, 4, msdu);
    wmiPut(entry, 7, status);
    *ring.theirs = next * ring.words;
  }

  void exceptions(uint32_t linkIndex)
  {
    const auto old = dp.st;
    const auto posted = rx.refillPosted;
    CHECK(linkIndex < dp.linkDescs);
    uint8_t *link = dp.linkVa + linkIndex * kDpLinkDescBytes;
    for (unsigned i = 0; i < 2; ++i) {
      const uint32_t cookie = takeBuffer();
      httdetail::putBuffer(link + kDpLinkMsduAt + i * kDpLinkMsduBytes, rx.refillBus[cookie], rx.rbm, cookie);
    }
    post(dp.reoExc, dp.linkBus + linkIndex * kDpLinkDescBytes, kDpRbmIdleDescDev0, linkIndex, kDpReoBufTypeLinkDesc);
    const uint32_t direct = takeBuffer();
    const uint32_t status = 1u << kDpReoPushShift | 5u << kDpReoErrShift;
    post(dp.reoExc, rx.refillBus[direct], rx.rbm, direct, status);
    post(dp.reoExc, rx.refillBus[direct], rx.rbm, direct, status); // duplicate with room on the refill ring
    CHECK(httRefillFree(rx) == 4);
    const auto drop = [](uint32_t, const uint8_t *, uint32_t) { CHECK(false); };
    CHECK(dpPoll(dp, io, drop) == 3 && dpPoll(dp, io, drop) == 0);
    CHECK(dp.st.excEntries == old.excEntries + 3 && dp.st.excLinks == old.excLinks + 1 && !dp.st.excLinkForeign);
    CHECK(dp.st.excBufRecycled == old.excBufRecycled + 3 && dp.st.excBufOther == old.excBufOther + 1);
    CHECK(dp.st.lastPushReason == 1 && dp.st.lastErrorCode == 5);
    CHECK(dp.reoExc.ours * dp.reoExc.words == *dp.reoExc.theirs && doorbells[0] == *dp.reoExc.theirs);
    CHECK(httRefillFree(rx) == 1 && rx.refillPosted == posted + 3);
  }

  void receive(uint32_t cookie, uint8_t marker)
  {
    uint8_t *buffer = rx.refillVa[cookie];
    std::memset(buffer, 0, kRxBufferBytes);
    wmiPut(buffer + kRxDescInfo14, 0, kRxDescMsduDone);
    wmiPut(buffer + kRxDescInfo10, 0, kFrameBytes);
    wmiPut(buffer + kRxDescInfo11, 0, kRxDecapEthernet << kRxDescDecapShift);
    uint8_t *frame = buffer + kRxDescBytes;
    frame[12] = 0x08;
    frame[14] = marker;
    post(dp.reo, rx.refillBus[cookie], rx.rbm, cookie, 0, kDpMsduFirst | kDpMsduLast | kFrameBytes << kDpMsduLenShift);
    const uint32_t before = delivered;
    const auto deliver = [&](uint32_t kind, const uint8_t *bytes, uint32_t size) {
      CHECK(kind == kDpRxEthernet && size == kFrameBytes && bytes[14] == marker);
      ++delivered;
    };
    CHECK(dpPoll(dp, io, deliver) == 1 && delivered == before + 1);
    CHECK(dp.st.rxFrames == delivered && dp.st.rxRecycled == delivered && !dp.st.rxBadCookie && !dp.st.rxRecycleFull);
    CHECK(dp.reo.ours * dp.reo.words == *dp.reo.theirs && doorbells[1] == *dp.reo.theirs);
  }

  void checkAllBuffersPosted() const
  {
    const auto &ring = rx.ring[kHttRefill];
    CHECK(httRefillFree(rx) == 0 && rx.refillPosted == consumed + kRxRefillEntries - 1);
    std::array<bool, kRxRefillEntries - 1> seen{};
    const uint32_t head = *ring.head / ring.entryWords, tail = *ring.tail / ring.entryWords;
    CHECK(head == ring.ours && head < ring.entries && tail < ring.entries);
    for (uint32_t slot = tail; slot != head;
         slot = (slot + 1) % ring.entries) {
      const uint8_t *entry = ring.va + 4 * ring.entryWords * slot;
      const uint32_t cookie = dpdetail::cookieOf(entry);
      CHECK(cookie < seen.size() && !seen[cookie] && rx.refillSlot[cookie] == slot);
      CHECK(dpdetail::addrOf(entry) == rx.refillBus[cookie]);
      seen[cookie] = true;
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool posted) { return posted; }));
  }
};

void testInvoluntaryRejoin(bool kickoutEnds)
{
  FrontEnd front;
  auto saved = savedNetwork();
  const auto request = front.ask(true, apple::kAssociate, &saved, sizeof saved);
  CHECK(!request.error && request.associationProfile);
  const auto &profile = request.associationProfileData;

  // The retained-profile retry path can start a new station without SET20.
  // The native request path is exercised separately below.
  WifiUserControl intent;
  WifiJoinRequest profileMode;
  Station station;
  ReceiveRings rings;
  auto beginFromProfile = [&](uint32_t lossCause) {
    CHECK(intent.allowed(intent.read()) && profileMode.canStart(station.now));
    station.begin(profile, lossCause != kStaDiscNone, kickoutEnds);
  };
  rings.checkAllBuffersPosted();
  station.hearBeacon();
  beginFromProfile(kStaDiscNone);
  station.joined();

  // Exercise generations 6 through 15 over the same DMA rings for each
  // policy: kickout ends the station, or the default low-ack report followed
  // by beacon miss ends it.
  const uint32_t originalIntent = intent.read();
  for (uint32_t generation = 6; generation <= 15; ++generation) {
    station.event(kWmiEvtPeerStaKickout, kWmiTagPeerStaKickoutEvent, {0, 0, 1, 0}, 0);
    if (!kickoutEnds) {
      CHECK(station.sta.step == kStaAssociated && station.sta.kickouts == 1 && !station.sta.disconnect);
      station.event(kWmiEvtRoam, kWmiTagRoamEvent, {0, kWmiRoamReasonBeaconMiss, 0});
    }
    const uint32_t cause = station.sta.disconnect;
    CHECK(cause == (kickoutEnds ? kStaDiscKickout : kStaDiscBeaconMiss));
    CHECK(station.sta.step == kStaTeardown);

    const uint32_t pendingRx = rings.takeBuffer();
    rings.exceptions(generation);
    station.runTo(kStaDone);
    station.checkTeardown();
    CHECK(!station.sta.deauthSent);
    CHECK(intent.read() == originalIntent);

    const uint32_t oldFreq = station.startFreq;
    station.airFreq = oldFreq == 5180 ? 5200 : 5180;
    CHECK(station.scan.count == 1 && station.scan.entries[0].freq == oldFreq);
    beginFromProfile(cause);
    station.runTo(kStaRescan);
    if (generation == 6) {
      // A scan without a beacon leaves the cached channel deaf. A failure
      // before association retains the involuntary cause for the next try.
      station.finishScan(false);
      station.runTo(kStaStopped);
      CHECK(station.sta.error == kStaErrNoAnswer && !station.sta.associatedAt && !station.sta.disconnect);
      CHECK(station.startFreq == oldFreq && intent.allowed(intent.read()));
      beginFromProfile(cause);
      station.runTo(kStaRescan);
    }
    station.finishScan(true);
    station.joined();
    CHECK(station.sta.scanFirstDone && station.sta.rescansDone == 0);
    rings.receive(pendingRx, uint8_t(generation));
    rings.checkAllBuffersPosted();
  }
  CHECK(rings.delivered == 10 && rings.dp.st.excBufRecycled == 30);
}

void publishBss(FrontEnd &front, const ScanEntry &entry)
{
  // Mirror the scan property's fields at the IOKit boundary.
  auto &bss = front.state.scan.bss[0];
  std::memcpy(bss.bssid, entry.bssid, sizeof bss.bssid);
  std::memcpy(bss.ssid, entry.ssidRaw, entry.ssidLength);
  bss.ssidLength = entry.ssidLength;
  bss.channel = entry.channel;
  bss.freq = entry.freq;
  bss.rssi = entry.rssi;
  bss.capability = entry.capability;
  bss.beaconInterval = entry.beaconInterval;
  bss.rateCount = entry.rateCount;
  std::memcpy(bss.rates, entry.rates, entry.rateCount);
  bss.rsnLength = entry.rsnElementLength;
  std::memcpy(bss.rsn, entry.rsnElement, bss.rsnLength);
  front.state.scan.count = 1;
}

bool hasRsn(const apple::ScanResult &bss)
{
  CHECK(bss.elementsLength <= sizeof bss.elements);
  for (uint32_t i = 0; i < bss.elementsLength;) {
    CHECK(i + 2 <= bss.elementsLength);
    const uint32_t bytes = 2u + bss.elements[i + 1];
    CHECK(i + bytes <= bss.elementsLength);
    if (bss.elements[i] == kDot11ElementRsn) {
      CHECK(bytes == sizeof kRsn && std::memcmp(bss.elements + i, kRsn, bytes) == 0);
      return true;
    }
    i += bytes;
  }
  return false;
}

void checkSecuredBss(const apple::ScanResult &bss)
{
  CHECK(bss.capability == kCapability && hasRsn(bss));
  CHECK(bss.ssidLength == sizeof kSsid && std::memcmp(bss.ssid, kSsid, sizeof kSsid) == 0);
  CHECK(std::memcmp(bss.bssid, kApMac, sizeof kApMac) == 0);
}

void testMenuAndWake()
{
  Station station;
  FrontEnd front;
  WifiUserControl intent;
  WifiBootPower bootPower;
  WifiJoinRequest native;
  auto saved = savedNetwork();
  uint32_t restart = 0, generation = 0;
  bool usable = true, sleeping = false;

  auto receipt = [&] {
    if (!WifiUserControl::canReportJoinReceipt(intent.read(), usable, sleeping)) return 0u;
    return native.currentReceipt(station.now, restart, generation, station.sta.step == kStaAssociated,
                                 station.sta.authorized && intent.allowed(intent.read()));
  };
  auto canStart = [&] {
    return usable && !sleeping && intent.allowed(intent.read()) && native.canStart(station.now);
  };
  auto reportIdle = [&] {
    apple::observeAssociation(front.state, {}, WifiUserControl::reportedPower(intent.read()), true);
    uint8_t bytes[apple::kScanResultBytes27];
    CHECK(front.ask(false, apple::kCurrentNetwork, bytes, sizeof bytes).error == apple::kErrNotAssociated);
  };
  auto setPower = [&](bool on) {
    auto power = apple::powerRecord(on);
    const auto answer = front.ask(true, apple::kPower, &power, sizeof power);
    CHECK(!answer.error && answer.effects == apple::kDoPowerChanged && answer.powerOn == on);
    CHECK(bootPower.setPower(intent, answer.powerOn));
    if (!on) native.cancel();
    return intent.read();
  };
  auto leave = [&] {
    const auto answer = front.ask(true, apple::kDisassociate);
    CHECK(!answer.error && answer.effects == apple::kDoDisassociate);
    native.cancel();
    return intent.disconnect();
  };
  auto stopStation = [&] {
    staStop(station.sta, station.now);
    station.runTo(kStaStopped);
    station.checkTeardown();
    CHECK(station.sta.stopAsked && !station.sta.disconnect);
  };

  auto autoJoin = [&] {
    // A stand-in for the native client's automatic SET20 from a saved
    // network. The old SET20 generation is single-use, including after ON.
    const auto result = front.ask(true, apple::kAssociate, &saved, sizeof saved);
    CHECK(!result.error && result.associationProfile && intent.join());
    const auto &profile = result.associationProfileData;
    CHECK(profile.psk_len == saved.network.key_length &&
          std::memcmp(profile.psk, saved.network.key, profile.psk_len) == 0);
    native.accept(++generation, station.now, restart);
    CHECK(canStart());
    native.consume();
    station.begin(profile);
    station.joined();

    // The host AP ends at 802.11 association. Model the supplicant's
    // successful authorization when publishing the native join result.
    station.sta.authorized = true;
    CHECK(native.finish(generation, 1, station.now, restart) && receipt() == generation);
    apple::Association association{};
    association.associated = association.authorized = true;
    association.authType = 2;
    association.generation = generation;
    association.result = 1;
    association.ssidLength = station.sta.ssidLength;
    std::memcpy(association.ssid, station.sta.ssid, association.ssidLength);
    std::memcpy(association.bssid, station.sta.bssid, sizeof association.bssid);
    association.channel = uint16_t(station.sta.choice.channel);
    association.frequency = station.sta.choice.freq;
    const auto old = apple::observeAssociation(front.state, association, true, true);
    CHECK(apple::associationTransition(old, association).connected);
    const auto bss = front.readBss(apple::kCurrentNetwork);
    checkSecuredBss(bss);
    CHECK(bss.channel.channel == association.channel);
  };
  auto scan = [&] {
    station.hearBeacon();
    publishBss(front, station.scan.entries[0]);
    CHECK(front.ask(true, apple::kScanReq).effects == apple::kDoScanStarted);
    checkSecuredBss(front.readBss(apple::kScanResult));
  };

  scan();
  autoJoin();
  CHECK(generation == 1);
  bootPower.arm();  // explicit menu control is enabled for this host scenario
  const uint32_t left = leave();  // SET22 tears down the station, with power still on
  CHECK(intent.power(left) && !intent.allowed(left) && !receipt());
  const uint32_t off = setPower(false);  // physical OFF is SET19
  CHECK(!intent.radioOffConfirmed(off) && intent.reportedPower(off) && !canStart());
  stopStation();
  intent.stopped(left);
  CHECK(!intent.radioOffConfirmed(off));
  usable = false;
  intent.radioOff(off);
  CHECK(intent.radioOffConfirmed(off) && !intent.reportedPower(intent.read()));
  reportIdle();
  CHECK(front.ask(true, apple::kAssociate, &saved, sizeof saved).error == apple::kErrPowerOff);

  const uint32_t on = setPower(true);
  CHECK(intent.allowed(on) && !canStart() && !receipt());
  usable = true;
  ++restart;
  reportIdle();
  CHECK(!canStart() && !receipt() && native.generation == 1);
  scan();
  autoJoin();

  // Exercise both a system-only park and a park after sleep-preparation
  // SET22, where a wake ON reopens the logical station gate.
  for (bool sleepLeave : {false, true}) {
    uint32_t stopped = sleepLeave ? leave() : intent.read();
    sleeping = true;
    CHECK(!receipt() && !canStart());
    stopStation();
    usable = false;
    intent.stopped(stopped);
    reportIdle();
    sleeping = false;
    ++restart;
    if (sleepLeave) {
      CHECK(!intent.allowed(intent.read()));
      setPower(true);
    }
    CHECK(intent.allowed(intent.read()));
    usable = true;
    CHECK(!canStart() && !receipt() && native.generation == generation);
    scan();
    autoJoin();
  }
  CHECK(generation == 4 && restart == 3);

  CHECK(!front.ask(true, apple::kScanCacheClear).error);
  const auto uncached = front.readBss(apple::kCurrentNetwork);
  CHECK((uncached.capability & kDot11CapPrivacy) && !hasRsn(uncached));
  scan();
  checkSecuredBss(front.readBss(apple::kCurrentNetwork));
}
} // namespace

int main()
{
  testInvoluntaryRejoin(false);
  testInvoluntaryRejoin(true);
  testMenuAndWake();
  testRegulatory();
  std::puts("rejoin_regdb_model_host: all passed (offline; multi-AP roaming and menu-row click HOLD)");
}
