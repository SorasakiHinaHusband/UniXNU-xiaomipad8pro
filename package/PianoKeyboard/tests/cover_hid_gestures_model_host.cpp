// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. -I../../include -I../../PianoAuto -I../../PianoSensors cover_hid_gestures_model_host.cpp -o /tmp/cover_hid_gestures_model_host && /tmp/cover_hid_gestures_model_host
// Offline fixture for the cover's I2C reports, HID output and property changes. The strict frame checks, coordinate clamp and
// UART-style byte-stream replay are fixture policies. The WN8030 layout uses additive checksums. See PianoKeyboard.cpp's readFrame.
#include "CoverPower.h"
#include "HidDescriptors.h"
#include "Wn8030Piano.h"
#include "../../PianoBacklight/PianoBacklightClient.h"
#include "../../PianoPolicy/Policy.h"
#include "../../PianoSensors/SensorsStream.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace pianokeyboard;
using Bytes = std::span<const uint8_t>;
using WireFrame = std::array<uint8_t, kFrameBytes>;
using KeyReport = std::array<uint8_t, kKeyboardBytes>;
using ConsumerReport = std::array<uint8_t, kConsumerBytes>;
using TouchReport = std::array<uint8_t, kTouchpadBytes>;
using PointerReport = std::array<uint8_t, kPointerBytes>;
using CommandWire = std::array<uint8_t, kCommandWire>;
using Property = std::pair<std::string_view, int>;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); std::abort(); \
} } while (false)

constexpr std::string_view kAttachedProperty = "piano-keyboard-attached", kHallProperty = "piano-keyboard-hall";
constexpr KeyReport kKeysUp{kIdKeyboard};
constexpr ConsumerReport kConsumerUp{kIdConsumer};
constexpr PointerReport kPointerUp{};
constexpr TouchReport kLift{kIdTouchpad};

// Only the input, LED and publication parts of the serving loop are represented here. Events stand in for IOHIDDevice and IORegistry.
class CoverFixture {
 public:
  explicit CoverFixture(PointerConfig config = {}, bool lidDrop = false, bool blank = false) : pointer_(config)
  {
    gate_.configure(lidDrop);
    power_.configure(blank);
  }

  bool i2cRead(Bytes bytes, uint64_t ms)
  {
    if (power_.asleep() || !power_.running() || bytes.size() != kFrameBytes || !frameSumOk(bytes.data())) return false;
    Report reports[24];
    unsigned count = 0;
    if (unpackFrame(bytes.data(), reports, std::size(reports), &count) != Frame::reports) return false;
    // Preflight all reports before publishing even the valid prefix of a bad transaction.
    for (unsigned i = 0; i < count; ++i)
      if (!validPayload(reports[i])) return false;
    for (unsigned i = 0; i < count; ++i) dispatch(reports[i], ms);
    return true;
  }

  // Replay the same WN8030 bytes across arbitrary stream chunks. Keep a possible sync byte after a failed candidate.
  unsigned streamRead(Bytes bytes, uint64_t ms)
  {
    if (power_.asleep() || !power_.running()) return 0;
    unsigned accepted = 0;
    for (uint8_t byte : bytes) {
      if (!streamSize_ && !syncByte(byte)) continue;
      stream_[streamSize_++] = byte;
      if (streamSize_ < stream_.size()) continue;
      if (i2cRead(stream_, ms)) {
        streamSize_ = 0;
        ++accepted;
      } else {
        const auto next = std::find_if(stream_.begin() + 1, stream_.end(), syncByte);
        streamSize_ = size_t(stream_.end() - next);
        std::copy(next, stream_.end(), stream_.begin());
      }
    }
    return accepted;
  }

  void sleep(bool asleep, uint64_t ms)
  {
    const auto step = power_.sleep(asleep, ms);
    if (asleep) streamSize_ = 0;
    if (CoverPower::releasesInput(step)) releaseInput();
    if (step == CoverPower::Step::unblank && attached_) sendAttachCommands();
  }

  void sampleHall(uint32_t raw, bool edge, uint64_t ms)
  {
    const int sample = hallOpenFromRaw(raw, true);
    if (!hallDebounce_.sample(hall_, sample, edge, ms)) return;
    hall_ = sample;
    changes.emplace_back(kHallProperty, hall_);
    if (gate_.hall(hall_)) releaseInput();
  }

  bool ledOutput(Bytes bytes, uint8_t optionId = 0)
  {
    if (bytes.size() == 2 && bytes[0] == kIdKeyboard) ledWanted_ = (bytes[1] & kLedCapsLock) != 0;
    else if (bytes.size() == 1 && optionId == kIdKeyboard) ledWanted_ = (bytes[0] & kLedCapsLock) != 0;
    else return false;
    return true;
  }

  void serviceLeds()
  {
    if (power_.asleep() || !ledWanted_) return;
    const bool wanted = *ledWanted_;
    ledWanted_.reset();
    if (wanted == caps_) return;
    caps_ = wanted;
    if (attached_ && power_.running()) sendCommand(kCmdIndicator, caps_ ? kLedCapsOn : kLedCapsOff);
  }

  int property(std::string_view name, int fallback) const
  {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
      if (it->first == name) return it->second;
    return fallback;
  }

  uint32_t continuations() const { return pointer_.continuations(); }

  std::vector<KeyReport> keys;
  std::vector<ConsumerReport> consumer;
  std::vector<PointerReport> pointer;
  std::vector<Gesture> gestures;
  std::vector<int> brightnessSteps;
  std::vector<CommandWire> commands;
  std::vector<Property> changes;

 private:
  static bool syncByte(uint8_t byte) { return (byte & 0x7f) == 0x57; }

  static bool validPayload(const Report &r)
  {
    if (r.id == kIdTouchpad) return r.bytes[kTouchpadBytes - 1] <= 5;
    int attached;
    if (vendorKind(r, &attached) != Vendor::attach) return true;
    // Attach needs payload bytes 0..3 (the bit is in byte 9 of the report), followed by a checksum inside this report.
    const unsigned size = r.bytes[5];
    const unsigned checksum = 6 + size;
    return size >= 4 && checksum < r.length && sum8(r.bytes, checksum) == r.bytes[checksum];
  }

  void releaseInput()
  {
    keys.push_back(kKeysUp);
    consumer.push_back(kConsumerUp);
    auto &report = pointer.emplace_back();
    Pointer::release(report.data());
    pointer_.reset();
    filter_.reset();
    lastKeys_ = kKeysUp;
    brightness_ = {};
  }

  void sendCommand(uint8_t cmd, uint8_t value) { command(commands.emplace_back().data(), cmd, value); }

  void sendAttachCommands()
  {
    Command list[5];
    const unsigned n = attachCommands(caps_, 1, 0, list);
    for (unsigned i = 0; i < n; ++i) sendCommand(list[i].cmd, list[i].value);
  }

  void attach(bool on)
  {
    if (gate_.attach(on)) releaseInput();
    if (attached_ == on) return;
    attached_ = on;
    changes.emplace_back(kAttachedProperty, int(on));
    if (on) sendAttachCommands();
  }

  void touch(const uint8_t *raw, uint64_t ms)
  {
    TouchReport report;
    std::copy_n(raw, report.size(), report.begin());
    for (unsigned slot = 0; slot < kFingers; ++slot) {
      uint8_t *contact = report.data() + 2 + 8 * slot;
      putLe16(contact + 4, std::min<unsigned>(le16(contact + 4), kTouchXMax));
      putLe16(contact + 6, std::min<unsigned>(le16(contact + 6), kTouchYMax));
    }
    uint8_t out[2][kPointerBytes];
    const unsigned n = pointer_.touch(report.data(), ms, out);
    CHECK(n <= std::size(out));
    for (unsigned i = 0; i < n; ++i) std::copy_n(out[i], kPointerBytes, pointer.emplace_back().begin());
    const Swipe swipe = pointer_.takeSwipe();
    if (swipe != Swipe::none) {
      uint8_t shortcut[4][kKeyboardBytes];
      const unsigned count = filter_.shortcut(swipeUsage(swipe), shortcut);
      CHECK(count == std::size(shortcut));
      for (const auto &key : shortcut) std::copy_n(key, kKeyboardBytes, keys.emplace_back().begin());
    }
    Gesture gesture;
    if (pointer_.takeGesture(&gesture)) gestures.push_back(gesture);
  }

  void dispatch(const Report &r, uint64_t ms)
  {
    int attached;
    if (vendorKind(r, &attached) == Vendor::attach) {
      attach(attached == 1);
      return;
    }
    switch (r.id) {
    case kIdKeyboard: {
      if (!gate_.pass()) return;
      if (Keys::newKey(lastKeys_.data(), r.bytes)) pointer_.keyTyped(ms);
      std::copy_n(r.bytes, lastKeys_.size(), lastKeys_.begin());
      const int step = brightness_.keyboard(r.bytes);
      if (step) brightnessSteps.push_back(step);
      filter_.filter(r.bytes, keys.emplace_back().data());
      break;
    }
    case kIdConsumer: {
      if (!gate_.pass()) return;
      std::copy_n(r.bytes, kConsumerBytes, consumer.emplace_back().begin());
      const int step = brightness_.consumer(le16(r.bytes + 1));
      if (step) brightnessSteps.push_back(step);
      break;
    }
    case kIdTouchpad:
      if (gate_.pass()) touch(r.bytes, ms);
      break;
    }
  }

  WireFrame stream_{};
  size_t streamSize_ = 0;
  InputGate gate_;
  CoverPower power_;
  HallDebounce hallDebounce_;
  Pointer pointer_;
  Keys filter_;
  pianobacklight::KeyWatch brightness_;
  KeyReport lastKeys_ = kKeysUp;
  bool attached_ = false, caps_ = false;
  int hall_ = -1;
  std::optional<bool> ledWanted_;
};

static void seal(WireFrame &frame)
{
  frame.back() = sum8(frame.data() + 2, kFrameBytes - 3);
}

static WireFrame packet(std::initializer_list<Bytes> reports)
{
  WireFrame frame{0x57, 1, 0x39};
  size_t at = 3;
  for (Bytes report : reports) {
    CHECK(report.size() < frame.size() - at);
    std::copy(report.begin(), report.end(), frame.begin() + at);
    at += report.size();
  }
  seal(frame);
  return frame;
}

static std::array<uint8_t, 32> coverState(uint8_t state)
{
  std::array<uint8_t, 32> report{0x23, 0x31, 0x38, 0x80, 0xa2, 4, 1, 0, 0xff, state};
  report[10] = sum8(report.data(), 10);
  return report;
}

static std::array<uint8_t, 5> media(uint16_t usage)
{
  return {kIdConsumer, uint8_t(usage), uint8_t(usage >> 8), 0xab, 0xcd};
}

struct Finger { uint8_t id; uint16_t x, y; uint8_t flags = 7; };

static TouchReport contacts(uint8_t buttons, uint8_t count, std::initializer_list<Finger> fingers)
{
  CHECK(fingers.size() <= kFingers);
  TouchReport report{kIdTouchpad, buttons};
  report.back() = count;
  auto *slot = report.data() + 2;
  for (const Finger &finger : fingers) {
    slot[0] = finger.flags;
    slot[1] = finger.id;
    putLe16(slot + 4, finger.x);
    putLe16(slot + 6, finger.y);
    slot += 8;
  }
  return report;
}

static void expectRelease(const CoverFixture &cover)
{
  CHECK(!cover.keys.empty() && !cover.consumer.empty() && !cover.pointer.empty());
  CHECK(cover.keys.back() == kKeysUp && cover.consumer.back() == kConsumerUp && cover.pointer.back() == kPointerUp);
}

static void expectIndicator(const CommandWire &wire, uint8_t value, uint8_t checksum)
{
  const CommandWire expected{0x4c, 0x32, 0, 0x4e, 0x31, 0x80, 0x38, 0x2e, 1, value, checksum};
  CHECK(wire == expected);
}

static void testKeysMediaAndCaps()
{
  CoverFixture cover;
  CHECK(cover.i2cRead(packet({coverState(0x23)}), 0));
  CHECK(cover.commands.size() == 5);
  expectIndicator(cover.commands[1], kLedCapsOff, 0x62);
  expectIndicator(cover.commands[3], kLedMicOff, 0x59);

  const KeyReport letter{kIdKeyboard, 0, 0, 0x04};
  CHECK(cover.i2cRead(packet({letter, kKeysUp}), 1));
  CHECK((cover.keys == std::vector<KeyReport>{letter, kKeysUp}));

  const KeyReport chord{kIdKeyboard, 0x97, 0, 4, 8, 12, 16, 20, 24};
  CHECK(cover.i2cRead(packet({chord}), 2) && cover.keys.back() == chord);
  for (uint8_t error : {1, 2, 3}) {
    const KeyReport rollover{kIdKeyboard, 0xe3, 0, 30, 31, 32, 33, 34, error};
    KeyReport expected = chord;
    expected[1] = rollover[1];
    CHECK(cover.i2cRead(packet({rollover}), 3 + error) && cover.keys.back() == expected);
  }
  const KeyReport partialRelease{kIdKeyboard, 0x02, 0, 20};
  CHECK(cover.i2cRead(packet({partialRelease}), 7) && cover.keys.back() == partialRelease);
  CHECK(cover.i2cRead(packet({kKeysUp}), 8) && cover.keys.back() == kKeysUp);

  constexpr uint16_t usages[] = {0x6f, 0x6f, 0, 0x70, 0xe9, 0xea, 0xe2, 0x0234, 0};
  CHECK(cover.i2cRead(packet({media(0x6f), media(0x6f), media(0), media(0x70), media(0xe9), media(0xea),
                             media(0xe2), media(0x0234), media(0)}), 9));
  CHECK(cover.consumer.size() == std::size(usages));
  for (size_t i = 0; i < std::size(usages); ++i) {
    const ConsumerReport expected{kIdConsumer, uint8_t(usages[i]), uint8_t(usages[i] >> 8)};
    CHECK(cover.consumer[i] == expected);
  }
  CHECK((cover.brightnessSteps == std::vector<int>{1, -1}));

  const KeyReport capsKey{kIdKeyboard, 0, 0, 0x39};
  CHECK(cover.i2cRead(packet({capsKey, kKeysUp}), 10));
  cover.serviceLeds();
  CHECK(cover.commands.size() == 5);
  const std::array<uint8_t, 2> allLeds{kIdKeyboard, 0xff}, wrongId{kIdConsumer, 0}, otherLeds{kIdKeyboard, 0xfd};
  const std::array<uint8_t, 1> capsOnly{kLedCapsLock}, truncated{kIdKeyboard};
  CHECK(cover.ledOutput(allLeds));
  CHECK(!cover.ledOutput(wrongId) && !cover.ledOutput(truncated) && !cover.ledOutput({}));
  cover.serviceLeds();
  CHECK(cover.commands.size() == 6);
  expectIndicator(cover.commands.back(), kLedCapsOn, 0x63);
  CHECK(cover.ledOutput(capsOnly, kIdKeyboard));
  cover.serviceLeds();
  CHECK(cover.commands.size() == 6);
  CHECK(cover.i2cRead(packet({coverState(0x22), coverState(0x23)}), 11));
  CHECK(cover.commands.size() == 11);
  expectIndicator(cover.commands[7], kLedCapsOn, 0x63);
  CHECK(cover.i2cRead(packet({coverState(0x22)}), 12));
  CHECK(cover.ledOutput(otherLeds));
  cover.serviceLeds();
  CHECK(cover.commands.size() == 11);
  CHECK(cover.i2cRead(packet({coverState(0x23)}), 13) && cover.commands.size() == 16);
  expectIndicator(cover.commands[12], kLedCapsOff, 0x62);
  expectIndicator(cover.commands[14], kLedMicOff, 0x59);
}

static void testMoveTapDragScrollAndPalm()
{
  CoverFixture cover;
  const auto one = contacts(0, 1, {{1, 1000, 700}});
  CHECK(cover.i2cRead(packet({one}), 0) && cover.pointer.empty());
  CHECK(cover.i2cRead(packet({kLift}), 30));
  CHECK((cover.pointer == std::vector<PointerReport>{{1}, kPointerUp}));
  CHECK(cover.gestures.back().most == 1 && cover.gestures.back().tap == 1);

  CHECK(cover.i2cRead(packet({one}), 100));
  CHECK(cover.i2cRead(packet({contacts(0, 1, {{1, 1040, 730}})}), 110));
  CHECK((cover.pointer.back() == PointerReport{0, 40, 0, 20, 0, 0, 0}));
  CHECK(cover.i2cRead(packet({contacts(1, 1, {{1, 1050, 730}})}), 120));
  CHECK((cover.pointer.back() == PointerReport{1}));
  const size_t beforeDrag = cover.pointer.size();
  CHECK(cover.i2cRead(packet({contacts(1, 1, {{1, 1100, 730}})}), 169));
  CHECK(cover.pointer.size() == beforeDrag);
  CHECK(cover.i2cRead(packet({contacts(1, 1, {{1, 1120, 730}})}), 170));
  CHECK((cover.pointer.back() == PointerReport{1, 20, 0, 0, 0, 0, 0}));
  CHECK(cover.i2cRead(packet({kLift}), 180) && cover.pointer.back() == kPointerUp);
  CHECK(cover.gestures.back().pressed && !cover.gestures.back().tap);

  const auto two = contacts(0, 2, {{1, 1000, 900}, {2, 1300, 900}});
  CHECK(cover.i2cRead(packet({two}), 240));
  CHECK(cover.i2cRead(packet({contacts(0, 2, {{1, 1005, 810}, {2, 1305, 810}})}), 250));
  CHECK((cover.pointer.back() == PointerReport{0, 0, 0, 0, 0, 12, 0}));
  CHECK(cover.i2cRead(packet({contacts(1, 2, {{1, 1005, 810}, {2, 1305, 810}})}), 260));
  CHECK((cover.pointer.back() == PointerReport{2}));
  const size_t beforeLift = cover.pointer.size();
  CHECK(cover.i2cRead(packet({contacts(1, 1, {{1, 1005, 810}})}), 270) && cover.pointer.size() == beforeLift);
  CHECK(cover.i2cRead(packet({kLift}), 280) && cover.pointer.back() == kPointerUp);
  CHECK(cover.gestures.back().most == 2 && cover.gestures.back().pressButton == 2);
  CHECK(cover.i2cRead(packet({two, kLift}), 350));
  CHECK(cover.gestures.back().tap == 2 && cover.pointer[cover.pointer.size() - 2][0] == 2);

  const auto palm = contacts(0, 2, {{1, 1000, 700, 3}, {2, 65535, 65535, 3}});
  const size_t beforePalm = cover.pointer.size();
  CHECK(cover.i2cRead(packet({one, palm}), 400));
  CHECK(cover.pointer.size() == beforePalm && cover.gestures.back().most == 1 && !cover.gestures.back().tap);
  CHECK(cover.i2cRead(packet({kLift}), 410) && cover.pointer.size() == beforePalm);

  const auto nearEdge = contacts(0, 1, {{1, kTouchXMax - 60, kTouchYMax - 60}});
  const auto beyondEdge = contacts(0, 1, {{1, 65535, 65535}});
  CHECK(cover.i2cRead(packet({nearEdge, beyondEdge}), 500));
  CHECK((cover.pointer.back() == PointerReport{0, 60, 0, 40, 0, 0, 0}));
  CHECK(cover.i2cRead(packet({kLift}), 800));
  CHECK(cover.gestures.back().x1 == kTouchXMax && cover.gestures.back().y1 == kTouchYMax);

  CHECK(cover.i2cRead(packet({two, contacts(0, 2, {{1, 1000, 0}, {2, 1300, 0}})}), 900));
  CHECK((cover.pointer.back() == PointerReport{0, 0, 0, 0, 0, 120, 0}));
  CHECK(cover.i2cRead(packet({contacts(0, 2, {{1, 1000, 65535}, {2, 1300, 65535}})}), 910));
  CHECK((cover.pointer.back() == PointerReport{0, 0, 0, 0, 0, uint8_t(-127), 0}));
}

static void testThreeThroughFiveContacts()
{
  for (uint8_t count : {3, 4, 5}) {
    PointerConfig config;
    config.gestures = true;
    CoverFixture cover(config);
    const KeyReport held{kIdKeyboard, 0x02, 0, 0x04};
    CHECK(cover.i2cRead(packet({held}), 0));
    const auto primary = contacts(0, count, {{1, 500, 700}, {2, 700, 700}, {3, 900, 700}});
    const auto tail = count == 4 ? contacts(3, 0, {{4, 65535, 65535}})
                                 : contacts(3, 0, {{4, 65535, 65535}, {5, 0, 0}});
    CHECK(cover.i2cRead(packet({primary}), 300));
    if (count > kFingers) {
      CHECK(cover.i2cRead(packet({tail}), 301));
      CHECK(cover.continuations() == 1 && cover.pointer.empty() && cover.gestures.empty());
    }
    const auto moved = contacts(0, count, {{1, 900, 700}, {2, 1100, 700}, {3, 1300, 700}});
    CHECK(cover.i2cRead(packet({moved}), 310));
    if (count > kFingers) {
      CHECK(cover.i2cRead(packet({tail}), 311));
      CHECK(cover.continuations() == 2 && cover.pointer.empty() && cover.gestures.empty());
    }
    const KeyReport ctrl{kIdKeyboard, 0x03, 0, 0x04}, ctrlLeft{kIdKeyboard, 0x03, 0, 0x04, 0x50};
    CHECK((cover.keys == std::vector<KeyReport>{held, ctrl, ctrlLeft, ctrl, held}));
    CHECK(cover.i2cRead(packet({contacts(0, 3, {{1, 1400, 700}, {2, 1600, 700}, {3, 1800, 700}}), kLift}), 320));
    CHECK(cover.keys.size() == 5 && cover.pointer.empty() && cover.gestures.size() == 1);
    CHECK(cover.gestures.back().most == 3 && cover.gestures.back().swipe == Swipe::right && !cover.gestures.back().tap);
  }
}

static void testFrameRejectionAndResync()
{
  CoverFixture cover;
  const auto attach = packet({coverState(0x23)});
  const auto detach = packet({coverState(0x22)});
  CHECK(cover.i2cRead(attach, 0));
  const auto touch = contacts(1, 1, {{1, 1000, 700}});
  const auto full = packet({touch, media(0xe9)});
  for (size_t length = 0; length < full.size(); ++length) {
    const std::vector<uint8_t> shortRead(full.begin(), full.begin() + length);
    CHECK(!cover.i2cRead(shortRead, 1));
  }
  std::vector<uint8_t> longRead(full.begin(), full.end());
  longRead.push_back(0);
  CHECK(!cover.i2cRead(longRead, 1));
  auto wrongSum = detach;
  wrongSum.back() ^= 1;
  CHECK(!frameSumOk(wrongSum.data()) && !cover.i2cRead(wrongSum, 2));
  auto vendor = coverState(0x22);
  vendor[10] ^= 1;
  CHECK(!cover.i2cRead(packet({touch, vendor}), 3));
  vendor[5] = 255;
  CHECK(!cover.i2cRead(packet({touch, vendor}), 4));
  for (uint8_t badCount : {6, 255}) {
    auto badTouch = touch;
    badTouch.back() = badCount;
    CHECK(!cover.i2cRead(packet({coverState(0x22), badTouch}), 5));
  }
  auto overrun = packet({touch, touch});
  overrun[3 + 2 * kTouchpadBytes] = kIdTouchpad;
  seal(overrun);
  CHECK(!cover.i2cRead(overrun, 6));
  auto unknown = packet({touch});
  unknown[3 + kTouchpadBytes] = 0x77;
  seal(unknown);
  CHECK(!cover.i2cRead(unknown, 7));
  CHECK(cover.pointer.empty() && cover.consumer.empty() && cover.keys.empty() && cover.gestures.empty());
  CHECK(cover.changes.size() == 1 && cover.property(kAttachedProperty, 0) == 1);
  CHECK(cover.i2cRead(full, 8) && cover.pointer.size() == 1 && cover.consumer.size() == 1);
  CHECK((cover.pointer.back() == PointerReport{1}));

  // Noise, a truncated candidate with an embedded false sync, a bad checksum, then a valid high-bit sync and a valid detach.
  auto reattach = attach;
  reattach[0] = 0xd7;
  std::vector<uint8_t> stream{0xff, 0x00, 0x57, 0xd7, 0x39, kIdTouchpad, 0};
  stream.insert(stream.end(), wrongSum.begin(), wrongSum.end());
  stream.insert(stream.end(), reattach.begin(), reattach.end());
  stream.insert(stream.end(), detach.begin(), detach.end());
  for (size_t chunk : {1, 7, 67, 68, 256}) {
    CoverFixture replay;
    CHECK(replay.i2cRead(detach, 0)); // Make a bogus attach observable and drop HID until a valid attach.
    unsigned accepted = 0;
    for (size_t at = 0; at < stream.size(); at += chunk) {
      const size_t n = std::min(chunk, stream.size() - at);
      accepted += replay.streamRead(Bytes(stream).subspan(at, n), at + 1);
    }
    CHECK(accepted == 2 && replay.commands.size() == 5);
    CHECK((replay.changes == std::vector<Property>{{kAttachedProperty, 1}, {kAttachedProperty, 0}}));
    CHECK(replay.gestures.empty() && replay.pointer.size() == 2);
    expectRelease(replay);
  }
}

static void expectCoverProperties(const CoverFixture &cover, int attached, int hall)
{
  const int attachment = cover.property(kAttachedProperty, 0);
  const int lid = cover.property(kHallProperty, -1);
  const SsConditions sensors{.light_consumer = 1, .accel_consumer = 1, .display = 1, .lid = lid,
                             .attached = attachment == 1, .accel_with_cover = 0};
  CHECK(sensors.attached == attached && sensors.lid == hall);

  using namespace pianopolicy;
  Inputs in;
  in.attached = attachment == 1;
  in.lid = lid < 0 ? Lid::unknown : lid == 0 ? Lid::closed : Lid::open;
  in.screen = Screen::off;
  in.sleepOn = true;
  in.wakeMask = kWakeLid | kWakeKeyboard;
  in.power = Power::battery;
  in.lidSleepArmed = false;
  Config config;
  config.lid = true;
  const auto policy = view(config, in, true);
  CHECK(policy.close == (hall < 0 ? Close::none : Close::lock));
  CHECK(policy.keyboardWake == (attached == 1 && hall == 1));
  CHECK(policy.keyboardLatch == policy.keyboardWake);
}

static void testCoverBounceHallAndSleep()
{
  const auto attach = packet({coverState(0x23)});
  const auto detach = packet({coverState(0x22)});
  const auto finger = contacts(0, 1, {{1, 1000, 700}});
  const auto down = contacts(1, 1, {{1, 1000, 700}});
  const KeyReport chord{kIdKeyboard, 0x97, 0, 0x04, 0x05};
  const auto held = packet({chord, media(0x6f), down});
  const KeyReport rollover{kIdKeyboard, 0, 0, 1};
  const auto stale = packet({rollover, kLift});

  CoverFixture cover({}, true);
  expectCoverProperties(cover, 0, -1);
  cover.sampleHall(1, false, 0);
  cover.sampleHall(1, false, 1);
  expectCoverProperties(cover, 0, 1);
  for (unsigned bounce = 0; bounce < 4; ++bounce) {
    const uint64_t t = 10 + 10 * bounce;
    CHECK(cover.i2cRead(attach, t));
    expectCoverProperties(cover, 1, 1);
    CHECK(cover.i2cRead(held, t + 1));
    CHECK(cover.keys.back() == chord && cover.consumer.back()[1] == 0x6f && cover.pointer.back()[0] == 1);
    CHECK(cover.i2cRead(detach, t + 2));
    expectRelease(cover);
    expectCoverProperties(cover, 0, 1);
    const auto sizes = std::array{cover.keys.size(), cover.consumer.size(), cover.pointer.size(), cover.changes.size()};
    CHECK(cover.i2cRead(detach, t + 3) && cover.i2cRead(held, t + 4));
    CHECK((sizes == std::array{cover.keys.size(), cover.consumer.size(), cover.pointer.size(), cover.changes.size()}));
    CHECK(cover.i2cRead(attach, t + 5) && cover.i2cRead(stale, t + 6));
    expectRelease(cover);
    CHECK(cover.gestures.empty() && cover.brightnessSteps.size() == bounce + 1);
  }

  CHECK(cover.i2cRead(held, 100));
  cover.sampleHall(0, true, 101);
  cover.sampleHall(1, true, 110); // Bounce back before confirmation.
  expectCoverProperties(cover, 1, 1);
  CHECK(cover.pointer.back()[0] == 1);
  cover.sampleHall(0, true, 111);
  cover.sampleHall(0, false, 130);
  expectCoverProperties(cover, 1, 1);
  cover.sampleHall(0, false, 131);
  expectCoverProperties(cover, 1, 0);
  expectRelease(cover);
  const size_t closedKeys = cover.keys.size(), closedButtons = cover.pointer.size();
  CHECK(cover.i2cRead(held, 132));
  CHECK(cover.keys.size() == closedKeys && cover.pointer.size() == closedButtons);
  CHECK(cover.i2cRead(detach, 133));
  expectCoverProperties(cover, 0, 0);
  CHECK(cover.i2cRead(attach, 134));
  expectCoverProperties(cover, 1, 0);
  cover.sampleHall(1, true, 140);
  cover.sampleHall(1, false, 160);
  expectCoverProperties(cover, 1, 1);
  CHECK(cover.i2cRead(stale, 161) && cover.gestures.empty());
  expectRelease(cover);

  for (bool blank : {false, true}) {
    CoverFixture sleeping({}, false, blank);
    CHECK(sleeping.i2cRead(attach, 0));
    CHECK(sleeping.i2cRead(held, 300));
    const auto five = contacts(1, 5, {{1, 1000, 700}, {2, 1200, 700}, {3, 1400, 700}});
    CHECK(sleeping.i2cRead(packet({five}), 301));
    const Bytes detachedBytes(detach);
    CHECK(sleeping.streamRead(detachedBytes.first(25), 301) == 0);
    sleeping.sleep(true, 302);
    expectRelease(sleeping);
    const size_t releaseCount = sleeping.keys.size();
    sleeping.sleep(true, 303);
    CHECK(!sleeping.i2cRead(held, 304) && sleeping.streamRead(attach, 305) == 0);
    CHECK(sleeping.keys.size() == releaseCount);
    sleeping.sleep(false, 306);
    CHECK(sleeping.streamRead(detachedBytes.subspan(25), 307) == 0);
    expectCoverProperties(sleeping, 1, -1);
    CHECK(sleeping.i2cRead(stale, 308));
    expectRelease(sleeping);
    CHECK(sleeping.gestures.empty() && sleeping.continuations() == 0);
    CHECK(sleeping.i2cRead(packet({finger, kLift}), 309));
    CHECK(sleeping.gestures.size() == 1 && sleeping.gestures.back().tap == 1);
    CHECK(sleeping.i2cRead(packet({finger}), 310));
    sleeping.sleep(true, 311);
    sleeping.sleep(false, 312);
    const size_t beforeLift = sleeping.pointer.size();
    CHECK(sleeping.i2cRead(packet({kLift}), 313));
    CHECK(sleeping.pointer.size() == beforeLift && sleeping.gestures.size() == 1);
  }
}

int main()
{
  testKeysMediaAndCaps();
  testMoveTapDragScrollAndPalm();
  testThreeThroughFiveContacts();
  testFrameRejectionAndResync();
  testCoverBounceHallAndSleep();
  std::puts("cover_hid_gestures_model_host: key, LED, pointer, framing and lifecycle checks passed");
}
