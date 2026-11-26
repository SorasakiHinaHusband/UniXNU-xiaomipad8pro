// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline D1 hotplug model for one routed SST sink. Events are serialized.
#pragma once
#include "DpExternalModes.h"

namespace pianodp {

enum class DpHotplugStatus : uint8_t {
  Disconnected, Active, InvalidPinLanes, EdidAuxError, BadEdid, TrainFailed, NoMode, StreamFailed,
};

constexpr size_t kDpEdidAuxChunk = 16;
static_assert(kEdidBlockBytes % kDpEdidAuxChunk == 0);
constexpr unsigned kDpEdidAuxAttempts = 4, kDpEdidAuxRetryDelayUs = 400;

// Io supplies the trainDpLink callbacks plus readEdid(offset, bytes, size),
// startStream(link, timing) and teardown(). readEdid maps a linear EDID offset
// to a complete I2C-over-AUX read; Ack promises the full chunk. teardown clears
// adapter state after any attempted plug, including a partially started stream.
class DpHotplug {
 public:
  DpHotplug() = default;
  DpHotplug(const DpHotplug &) = delete;
  DpHotplug &operator=(const DpHotplug &) = delete;

  DpHotplugStatus status() const { return status_; }
  DpLink link() const { return link_; }
  const DpExternalModes &modes() const { return modes_; }

  template <typename Io>
  DpHotplugStatus hpd(Io &io, bool high, bool irq, unsigned pinLanes)
  {
    if (!high) {
      disconnect(io);
      return status_;
    }
    if (status_ != DpHotplugStatus::Disconnected && pinLanes_ == pinLanes) {
      if (!irq || (status_ == DpHotplugStatus::Active && linkHealthy(io))) return status_;
    }

    disconnect(io);
    pinLanes_ = pinLanes;
    if (!validDpLaneCount(pinLanes)) return status_ = DpHotplugStatus::InvalidPinLanes;

    status_ = activate(io);
    if (status_ != DpHotplugStatus::Active) io.teardown();
    return status_;
  }

 private:
  DpHotplugStatus status_ = DpHotplugStatus::Disconnected;
  unsigned pinLanes_ = 0;
  DpLink link_{};
  DpExternalModes modes_{};

  template <typename Io>
  void disconnect(Io &io)
  {
    const bool wasActive = status_ == DpHotplugStatus::Active;
    if (wasActive) {
      link_ = {};
      modes_ = {};
    }
    status_ = DpHotplugStatus::Disconnected;
    pinLanes_ = 0;
    if (wasActive) io.teardown();
  }

  template <typename Io>
  bool linkHealthy(Io &io) const
  {
    using namespace dp_train_detail;
    uint8_t status[6]{};
    return io.auxRead(kLinkStatus, status, sizeof status) == DpAuxStatus::Ack && linkEqualized(status, link_.lanes);
  }

  template <typename Io>
  DpHotplugStatus activate(Io &io)
  {
    uint8_t bytes[kEdidMaxBytes]{};
    size_t length = kEdidBlockBytes;
    for (size_t offset = 0; offset < length; offset += kDpEdidAuxChunk) {
      for (unsigned attempt = 1;; ++attempt) {
        const DpAuxStatus reply = io.readEdid(offset, bytes + offset, kDpEdidAuxChunk);
        if (reply == DpAuxStatus::Ack) break;
        if (attempt == kDpEdidAuxAttempts || (reply != DpAuxStatus::Defer && reply != DpAuxStatus::Timeout))
          return DpHotplugStatus::EdidAuxError;
        io.delayUs(kDpEdidAuxRetryDelayUs);
      }
      if (offset + kDpEdidAuxChunk == kEdidBlockBytes) {
        if (bytes[126] >= kEdidMaxBlocks) return DpHotplugStatus::BadEdid;
        length = (size_t(bytes[126]) + 1) * kEdidBlockBytes;
      }
    }

    EdidInfo edid;
    if (parseEdid(bytes, length, edid) != EdidStatus::Ok) return DpHotplugStatus::BadEdid;
    const DpTrainResult training = trainDpLink(io, pinLanes_);
    if (training.status != DpTrainStatus::Trained) return DpHotplugStatus::TrainFailed;

    const DpExternalModes modes = buildDpExternalModes(edid, training.link, DpPixelFormat::Rgb8);
    if (!modes.count) return DpHotplugStatus::NoMode;
    if (!io.startStream(training.link, modes.modes[0])) return DpHotplugStatus::StreamFailed;
    link_ = training.link;
    modes_ = modes;
    return DpHotplugStatus::Active;
  }
};

} // namespace pianodp
