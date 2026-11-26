// SPDX-License-Identifier: BSD-2-Clause-Patent
// D0 single-sink, 8b/10b DisplayPort link training and stream arithmetic.
#pragma once
#include "DpEdid.h"

namespace pianodp {

enum class DpAuxStatus : uint8_t { Ack, Nack, Timeout, Defer, ShortTransfer, IoError };
enum class DpTrainingPattern : uint8_t { Disabled, Tps1, Tps2, Tps3, Tps4 };

struct DpReceiverCaps {
  uint8_t revision = 0;
  uint32_t maxRateKhz = 0;
  unsigned maxLanes = 0;
  unsigned crDelayUs = 0, eqDelayUs = 0;
  bool enhancedFraming = false, downspread = false;
  DpTrainingPattern eqPattern = DpTrainingPattern::Tps2;
};

constexpr size_t kDpReceiverCapBytes = 16;

inline bool parseDpReceiverCaps(const uint8_t *dpcd, size_t length, DpReceiverCaps &caps)
{
  caps = {};
  if (!dpcd || length < kDpReceiverCapBytes || dpcd[0] < 0x10) return false;
  uint32_t rate;
  switch (dpcd[1]) {
    case 0x06: rate = kDpRbrKhz; break;
    case 0x0a: rate = kDpHbrKhz; break;
    case 0x14: rate = kDpHbr2Khz; break;
    case 0x1e: rate = 810000; break;
    default: return false;
  }
  const unsigned lanes = dpcd[2] & 0x1f;
  const unsigned interval = dpcd[14] & 0x7f;
  if (!validDpLaneCount(lanes) || interval > 4) return false;
  caps.revision = dpcd[0];
  caps.maxRateKhz = rate;
  caps.maxLanes = lanes;
  caps.crDelayUs = caps.revision >= 0x14 || !interval ? 100 : interval * 4000;
  caps.eqDelayUs = interval ? interval * 4000 : 400;
  caps.enhancedFraming = dpcd[2] & 0x80;
  caps.downspread = dpcd[3] & 1;
  if (caps.revision >= 0x14 && (dpcd[3] & 0x80)) caps.eqPattern = DpTrainingPattern::Tps4;
  else if (caps.revision >= 0x12 && (dpcd[2] & 0x40)) caps.eqPattern = DpTrainingPattern::Tps3;
  return true;
}

struct DpDrive {
  unsigned swing = 0, preemphasis = 0;
  constexpr bool operator==(const DpDrive &) const = default;
};

// QMP PHY table indices: row = swing, column = pre-emphasis.
constexpr DpDrive clampDpDrive(unsigned swing, unsigned preemphasis)
{
  if (swing > 3) swing = 3;
  if (preemphasis > 3 - swing) preemphasis = 3 - swing;
  return {swing, preemphasis};
}

enum class DpTrainStatus : uint8_t {
  Trained, InvalidPinLanes, InvalidCaps, AuxError, PhyError, ClockRecoveryFailed, ChannelEqualizationFailed,
};

struct DpTrainResult {
  DpTrainStatus status;
  DpAuxStatus auxStatus = DpAuxStatus::Ack;
  DpLink link;   // last attempted configuration
  unsigned attempts = 0;
  bool enhancedFraming = false;
  bool downspread = false;
};

namespace dp_train_detail {

constexpr uint32_t kReceiverCaps = 0x000, kLinkConfig = 0x100, kPatternSet = 0x102, kLaneSet = 0x103;
constexpr uint32_t kDownspreadCtrl = 0x107, kLinkStatus = 0x202;
constexpr unsigned kCrDone = 1, kEqDone = 7, kAlignDone = 1;
constexpr unsigned kCrTriesPerSwing = 4, kEqTries = 6, kMaxAttempts = 4;

constexpr uint8_t dpcdPattern(DpTrainingPattern pattern)
{
  if (pattern == DpTrainingPattern::Disabled) return 0;
  if (pattern == DpTrainingPattern::Tps4) return 7; // DPCD code, scrambling enabled
  return uint8_t(0x20 | uint8_t(pattern));          // TPS1–3 disable scrambling
}

constexpr uint8_t dpcdLaneSet(DpDrive drive)
{
  return uint8_t(drive.swing | (drive.swing == 3 ? 0x04 : 0) |
                 (drive.preemphasis << 3) | (drive.preemphasis == 3 - drive.swing ? 0x20 : 0));
}

inline bool allLanesHaveStatus(const uint8_t (&status)[6], unsigned lanes, unsigned bits)
{
  for (unsigned lane = 0; lane < lanes; ++lane)
    if (((status[lane / 2] >> (4 * (lane % 2))) & bits) != bits) return false;
  return true;
}

template <typename Io>
struct TrainAttempt {
  Io &io;
  DpTrainResult &result;
  const DpReceiverCaps &caps;
  DpDrive drive{};

  bool write(uint32_t addr, const uint8_t *data, size_t size)
  {
    result.auxStatus = io.auxWrite(addr, data, size);
    return result.auxStatus == DpAuxStatus::Ack;
  }

  bool setDrive()
  {
    return io.setPhy(result.link.rateKhz, result.link.lanes, drive.swing, drive.preemphasis);
  }

  DpTrainStatus adjustDrive(DpDrive next)
  {
    if (next == drive) return DpTrainStatus::Trained;
    drive = next;
    if (!setDrive()) return DpTrainStatus::PhyError;
    const uint8_t lane = dpcdLaneSet(drive);
    const uint8_t settings[] = {lane, lane, lane, lane};
    return write(kLaneSet, settings, result.link.lanes) ? DpTrainStatus::Trained : DpTrainStatus::AuxError;
  }

  DpTrainStatus phase(DpTrainingPattern pattern)
  {
    const bool cr = pattern == DpTrainingPattern::Tps1;
    io.setPattern(pattern);
    const uint8_t lane = dpcdLaneSet(drive);
    const uint8_t settings[] = {dpcdPattern(pattern), lane, lane, lane, lane};
    if (!write(kPatternSet, settings, 1 + result.link.lanes)) return DpTrainStatus::AuxError;

    // The total CR poll bound also handles a sink that oscillates between swing requests.
    const unsigned tries = cr ? 4 * kCrTriesPerSwing : kEqTries;
    unsigned sameSwingTries = 0;
    for (unsigned i = 0; i < tries; ++i) {
      uint8_t status[6]{};
      io.delayUs(cr ? caps.crDelayUs : caps.eqDelayUs);
      result.auxStatus = io.auxRead(kLinkStatus, status, sizeof status);
      if (result.auxStatus != DpAuxStatus::Ack) return DpTrainStatus::AuxError;

      const bool crDone = allLanesHaveStatus(status, result.link.lanes, kCrDone);
      if (cr && crDone) return DpTrainStatus::Trained;
      if (!cr && !crDone) return DpTrainStatus::ClockRecoveryFailed;
      if (!cr && (status[2] & kAlignDone) && allLanesHaveStatus(status, result.link.lanes, kEqDone))
        return DpTrainStatus::Trained;

      // As on piano's reference path, apply lane 0's drive request to every active lane.
      const DpDrive next = clampDpDrive(status[4] & 3, (status[4] >> 2) & 3);
      if (cr) {
        if (drive.swing == 3) break;
        if (next.swing != drive.swing) sameSwingTries = 0;
        else if (++sameSwingTries == kCrTriesPerSwing) break;
      }
      if (i + 1 == tries) break;
      const DpTrainStatus adjusted = adjustDrive(next);
      if (adjusted != DpTrainStatus::Trained) return adjusted;
    }
    return cr ? DpTrainStatus::ClockRecoveryFailed : DpTrainStatus::ChannelEqualizationFailed;
  }

  DpTrainStatus run()
  {
    if (!setDrive()) return DpTrainStatus::PhyError;
    const uint8_t linkConfig[] = {uint8_t(result.link.rateKhz / 27000),
                                  uint8_t(result.link.lanes | (caps.enhancedFraming ? 0x80 : 0))};
    const uint8_t coding[] = {uint8_t(caps.downspread ? 0x10 : 0), 1}; // 0.5% spread, 8b/10b coding
    if (!write(kLinkConfig, linkConfig, sizeof linkConfig) || !write(kDownspreadCtrl, coding, sizeof coding))
      return DpTrainStatus::AuxError;

    DpTrainStatus status = phase(DpTrainingPattern::Tps1);
    if (status == DpTrainStatus::Trained) status = phase(caps.eqPattern);

    // A failed AUX transaction may already have reached the sink. Always retire
    // the source pattern and try to retire the sink pattern, preserving the first AUX error.
    const uint8_t disabled = 0;
    const DpAuxStatus stopped = io.auxWrite(kPatternSet, &disabled, 1);
    io.setPattern(DpTrainingPattern::Disabled);
    if (status != DpTrainStatus::AuxError && stopped != DpAuxStatus::Ack) {
      result.auxStatus = stopped;
      return DpTrainStatus::AuxError;
    }
    return status;
  }
};

} // namespace dp_train_detail

// Io supplies auxRead/auxWrite(addr, buf, len) returning DpAuxStatus (Ack means
// the full length), setPhy(rateKhz, lanes, swing, preemphasis) returning bool,
// setPattern(DpTrainingPattern) and delayUs(us). The adapter owns power and timing
// prerequisites. The model performs one direct sink's native AUX training.
template <typename Io>
DpTrainResult trainDpLink(Io &io, unsigned pinLanes)
{
  using namespace dp_train_detail;
  DpTrainResult result{.status = DpTrainStatus::InvalidPinLanes, .link = {}};
  if (!validDpLaneCount(pinLanes)) return result;

  uint8_t bytes[kDpReceiverCapBytes]{};
  result.auxStatus = io.auxRead(kReceiverCaps, bytes, sizeof bytes);
  if (result.auxStatus != DpAuxStatus::Ack) {
    result.status = DpTrainStatus::AuxError;
    return result;
  }
  DpReceiverCaps caps;
  if (!parseDpReceiverCaps(bytes, sizeof bytes, caps)) {
    result.status = DpTrainStatus::InvalidCaps;
    return result;
  }
  const uint32_t maxRate = caps.maxRateKhz < kDpHbr2Khz ? caps.maxRateKhz : kDpHbr2Khz;
  DpLink link{maxRate, pinLanes < caps.maxLanes ? pinLanes : caps.maxLanes};
  result.enhancedFraming = caps.enhancedFraming;
  result.downspread = caps.downspread;

  while (result.attempts < kMaxAttempts) {
    result.link = link;
    ++result.attempts;
    result.status = TrainAttempt<Io>{io, result, caps}.run();
    if (result.status == DpTrainStatus::ClockRecoveryFailed && link.rateKhz > kDpRbrKhz) {
      link.rateKhz = link.rateKhz == kDpHbr2Khz ? kDpHbrKhz : kDpRbrKhz;
    } else if ((result.status == DpTrainStatus::ClockRecoveryFailed ||
                result.status == DpTrainStatus::ChannelEqualizationFailed) && link.lanes > 1) {
      link.lanes /= 2;
      link.rateKhz = maxRate;  // reference ladder restarts the rate search on the narrower link
    } else {
      break;
    }
  }
  return result;
}

struct DpMsaTu {
  uint16_t hTotal = 0, hStart = 0, hActive = 0, hBlank = 0, hSyncWidth = 0;
  uint16_t vTotal = 0, vStart = 0, vActive = 0, vBlank = 0, vSyncWidth = 0;
  bool hSyncPositive = false, vSyncPositive = false;
  uint8_t misc0 = 0, misc1 = 0;
  uint32_t mvid = 0, nvid = 0;
  uint8_t tuSize = 0, validSymbols = 0;
  // Exact average per lane = validSymbolsFloor + numerator / denominator.
  uint8_t validSymbolsFloor = 0;
  uint32_t validSymbolsNumerator = 0, validSymbolsDenominator = 0;
};

// Progressive RGB 8 bpc SST, asynchronous stream clock. The exact TU ratio
// lets a later controller adapter choose a fractional stuffing schedule.
constexpr bool computeDpMsaTu(const DpTiming &mode, DpLink link, DpMsaTu &out)
{
  out = {};
  if (!edidModeFits(mode, link) || mode.interlaced || !mode.separateSync ||
      mode.hSyncWidth > 0x7fff || mode.vSyncWidth > 0x7fff)
    return false;

  constexpr uint8_t tuSize = 64;
  constexpr uint32_t nvid = 0x8000;
  const uint64_t mvid = (uint64_t(mode.pixelKhz) * nvid + link.rateKhz / 2) / link.rateKhz;
  const uint64_t tuNumerator = uint64_t(mode.pixelKhz) * kDpRgb8BitsPerPixel * tuSize;
  const uint32_t tuDenominator = uint32_t(payloadMbps(link.lanes, link.rateKhz / 100) * 1000);
  const uint64_t validSymbols = (tuNumerator + tuDenominator - 1) / tuDenominator;
  if (!mvid || mvid > 0xffffff || validSymbols > tuSize - 2u) return false; // allow FS and FE in each full TU

  out.hTotal = uint16_t(mode.hActive + mode.hBlank);
  out.vTotal = uint16_t(mode.vActive + mode.vBlank);
  out.hStart = uint16_t(mode.hBlank - mode.hSyncOffset);
  out.vStart = uint16_t(mode.vBlank - mode.vSyncOffset);
  out.hActive = mode.hActive;
  out.vActive = mode.vActive;
  out.hBlank = mode.hBlank;
  out.vBlank = mode.vBlank;
  out.hSyncWidth = mode.hSyncWidth;
  out.vSyncWidth = mode.vSyncWidth;
  out.hSyncPositive = mode.hSyncPositive;
  out.vSyncPositive = mode.vSyncPositive;
  out.misc0 = 0x20;  // 8 bpc, RGB, asynchronous clock
  out.mvid = uint32_t(mvid);
  out.nvid = nvid;
  out.tuSize = tuSize;
  out.validSymbols = uint8_t(validSymbols);
  out.validSymbolsFloor = uint8_t(tuNumerator / tuDenominator);
  out.validSymbolsNumerator = uint32_t(tuNumerator % tuDenominator);
  out.validSymbolsDenominator = tuDenominator;
  return true;
}

} // namespace pianodp
