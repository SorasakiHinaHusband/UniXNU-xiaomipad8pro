// SPDX-License-Identifier: BSD-2-Clause-Patent
// HCI over a UART (H4, Bluetooth Core Specification Vol 4 Part A): the packet indicators, an encoder for commands and ACL data, and an
// incremental decoder that turns the byte stream from the controller into whole packets. Portable (host tests in tests/bt_host.cpp);
// no allocation, bounded buffers, every length checked before it is used.
//
// The Qualcomm parts add single-byte in-band sleep messages (IBS) between packets: 0xfc WAKE_ACK, 0xfd WAKE_IND, 0xfe SLEEP_IND (the
// Ubuntu piano kernel's hci_qca.c, read as the specification of the wire protocol). They are not H4 indicators, so the decoder
// reports them as such wherever a packet may start. Command packets are decoded too: the UART loopback self test sees its own.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pianobt {

enum : uint8_t { kH4Command = 0x01, kH4Acl = 0x02, kH4Sco = 0x03, kH4Event = 0x04, kH4Iso = 0x05 };
enum : uint8_t { kIbsWakeAck = 0xfc, kIbsWakeInd = 0xfd, kIbsSleepInd = 0xfe };
enum : uint8_t { kEvCommandComplete = 0x0e, kEvCommandStatus = 0x0f, kEvHardwareError = 0x10, kEvVendor = 0xff };
inline bool isIbs(uint8_t b) { return b == kIbsWakeAck || b == kIbsWakeInd || b == kIbsSleepInd; }

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// An H4 command: 01, opcode (little endian), parameter length, parameters. Returns the frame's length, 0 when it does not fit.
inline size_t h4Command(uint8_t *out, size_t cap, uint16_t opcode, const uint8_t *params, uint8_t len)
{
  if (cap < size_t(4) + len || (len && !params)) return 0;
  out[0] = kH4Command;
  out[1] = uint8_t(opcode);
  out[2] = uint8_t(opcode >> 8);
  out[3] = len;
  for (uint8_t i = 0; i < len; ++i) out[4 + i] = params[i];
  return size_t(4) + len;
}
// An H4 ACL packet: 02, handle (12 bits) with packet boundary and broadcast flags (bits 15:12), length (little endian), data.
inline size_t h4Acl(uint8_t *out, size_t cap, uint16_t handle, uint8_t flags, const uint8_t *data, uint16_t len)
{
  if (cap < size_t(5) + len || handle > 0xeff || (len && !data)) return 0;
  const uint16_t h = uint16_t(handle | uint16_t(flags & 0xfu) << 12);
  out[0] = kH4Acl;
  out[1] = uint8_t(h);
  out[2] = uint8_t(h >> 8);
  out[3] = uint8_t(len);
  out[4] = uint8_t(len >> 8);
  for (uint16_t i = 0; i < len; ++i) out[5 + i] = data[i];
  return size_t(5) + len;
}

// The byte-stream decoder. feed() hands each complete packet to sink(type, packet, length), the packet being its header and
// payload without the indicator byte, and each IBS byte to sink(byte, nullptr, 0). A byte that cannot start a packet is skipped and
// counted (resynchronisation); a packet longer than the buffer is skipped whole and counted.
class H4Decoder {
 public:
  static constexpr size_t kMax = 4 + 1024;   // the longest header (ACL/ISO: 4) and 1024 payload bytes
  struct Stats {
    uint64_t packets = 0, ibs = 0, resyncs = 0, oversized = 0, bytes = 0;
  };

  template <class Sink> size_t feed(const uint8_t *in, size_t n, Sink &&sink)
  {
    for (size_t i = 0; i < n; ++i) {
      const uint8_t b = in[i];
      ++stats_.bytes;
      if (!type_) {
        if (isIbs(b)) {
          ++stats_.ibs;
          sink(b, static_cast<const uint8_t *>(nullptr), size_t(0));
          continue;
        }
        header_ = headerLength(b);
        if (!header_) {
          ++stats_.resyncs;
          continue;
        }
        type_ = b;
        have_ = 0;
        need_ = header_;
        skip_ = false;
        continue;
      }
      if (!skip_) buf_[have_] = b;
      ++have_;
      if (have_ < need_) continue;
      if (need_ == header_) {
        const size_t payload = payloadLength();
        need_ = header_ + payload;
        if (need_ > kMax) {
          skip_ = true;
          ++stats_.oversized;
        }
        if (payload) continue;
      }
      if (!skip_) {
        ++stats_.packets;
        sink(type_, static_cast<const uint8_t *>(buf_), have_);
      }
      type_ = 0;
    }
    return n;
  }
  bool idle() const { return type_ == 0; }
  const Stats &stats() const { return stats_; }
  void reset()
  {
    type_ = 0;
    have_ = need_ = header_ = 0;
    skip_ = false;
  }

 private:
  static size_t headerLength(uint8_t type)
  {
    switch (type) {
    case kH4Command: return 3;
    case kH4Acl: return 4;
    case kH4Sco: return 3;
    case kH4Event: return 2;
    case kH4Iso: return 4;
    default: return 0;
    }
  }
  size_t payloadLength() const
  {
    switch (type_) {
    case kH4Command: return buf_[2];
    case kH4Acl: return le16(buf_ + 2);
    case kH4Sco: return buf_[2];
    case kH4Event: return buf_[1];
    case kH4Iso: return le16(buf_ + 2) & 0x3fffu;
    default: return 0;
    }
  }
  uint8_t buf_[kMax] = {};
  uint8_t type_ = 0;
  size_t have_ = 0, need_ = 0, header_ = 0;
  bool skip_ = false;
  Stats stats_;
};

// The host's LE event mask, patched in the host-to-controller byte stream (piano-bt-lemask; docs/drivers/bluetooth.md, "LE link
// ready"). The 27.3 bluetoothd sends a type-zero controller the LE event mask 0x1f (our runtime patch adds 0x1000), which masks LE
// Data Length Change (bit 6) and LE PHY Update Complete (bit 11). Its link-ready state machine still waits for them after the
// remote features (LE_READY_WAIT_FOR_LENGTH, then _WAIT_FOR_PHY): an LE link connects and never reaches GATT. The patcher ORs both
// bits into an LE Set Event Mask (0x2001, 8 parameter bytes) that enables LE Connection Complete (bit 0) and leaves every other byte
// of the stream as it is. It is stateful, so a command split across writes is patched too; it never shifts or drops a byte. A byte
// that cannot start a packet is passed on and counted (the host's stream is well formed; the controller answers such a byte).
class H4LeMaskPatcher {
 public:
  static constexpr uint16_t kOpcode = 0x2001;
  static constexpr uint8_t kAdd0 = 0x40;   // bit 6: LE Data Length Change
  static constexpr uint8_t kAdd1 = 0x08;   // bit 11: LE PHY Update Complete
  struct Seen {
    uint64_t from = 0, to = 0;   // the host's mask and the one sent
  };
  struct Stats {
    uint64_t masks = 0, patched = 0, resyncs = 0;
  };

  // Patches data[0..n) in place when enabled. sink(seen) is called once per complete LE Set Event Mask command.
  template <class Sink> void patch(uint8_t *data, size_t n, bool enabled, Sink &&sink)
  {
    for (size_t i = 0; i < n; ++i) {
      uint8_t &b = data[i];
      if (!type_) {
        if (isIbs(b)) continue;
        header_ = headerLength(b);
        if (!header_) {
          ++stats_.resyncs;
          continue;
        }
        type_ = b;
        have_ = 0;
        mask_ = false;
        continue;
      }
      if (have_ < header_) {
        hdr_[have_++] = b;
        if (have_ < header_) continue;
        remain_ = payloadLength();
        if (type_ == kH4Command && le16(hdr_) == kOpcode && hdr_[2] == 8) {
          mask_ = true;
          enabled_ = enabled;
          seen_ = Seen{};
          at_ = 0;
        }
        if (!remain_) type_ = 0;
        continue;
      }
      if (mask_) {
        seen_.from |= uint64_t(b) << (8 * at_);
        if (at_ == 0) add_ = enabled_ && (b & 0x01);
        if (add_ && at_ == 0) b = uint8_t(b | kAdd0);
        if (add_ && at_ == 1) b = uint8_t(b | kAdd1);
        seen_.to |= uint64_t(b) << (8 * at_);
        ++at_;
      }
      if (--remain_ == 0) {
        if (mask_) {
          ++stats_.masks;
          if (seen_.to != seen_.from) ++stats_.patched;
          sink(seen_);
        }
        type_ = 0;
      }
    }
  }
  const Stats &stats() const { return stats_; }

 private:
  static size_t headerLength(uint8_t type)
  {
    switch (type) {
    case kH4Command: return 3;
    case kH4Acl: return 4;
    case kH4Sco: return 3;
    case kH4Iso: return 4;
    default: return 0;   // events do not travel from the host
    }
  }
  size_t payloadLength() const
  {
    switch (type_) {
    case kH4Command: return hdr_[2];
    case kH4Acl: return le16(hdr_ + 2);
    case kH4Sco: return hdr_[2];
    case kH4Iso: return le16(hdr_ + 2) & 0x3fffu;
    default: return 0;
    }
  }
  uint8_t hdr_[4] = {};
  uint8_t type_ = 0;
  size_t header_ = 0, have_ = 0, remain_ = 0;
  bool mask_ = false, enabled_ = false, add_ = false;
  unsigned at_ = 0;
  Seen seen_;
  Stats stats_;
};

// Events: [code][parameter length][parameters].
struct CommandComplete {
  uint8_t ncmd = 0;
  uint16_t opcode = 0;
  const uint8_t *ret = nullptr;   // the return parameters (the first is the status of most commands)
  size_t retLen = 0;
};
inline bool parseCommandComplete(const uint8_t *ev, size_t len, CommandComplete &cc)
{
  if (len < 5 || ev[0] != kEvCommandComplete || size_t(ev[1]) + 2 != len || ev[1] < 3) return false;
  cc.ncmd = ev[2];
  cc.opcode = le16(ev + 3);
  cc.ret = ev + 5;
  cc.retLen = len - 5;
  return true;
}
struct CommandStatus {
  uint8_t status = 0, ncmd = 0;
  uint16_t opcode = 0;
};
inline bool parseCommandStatus(const uint8_t *ev, size_t len, CommandStatus &cs)
{
  if (len != 6 || ev[0] != kEvCommandStatus || ev[1] != 4) return false;
  cs.status = ev[2];
  cs.ncmd = ev[3];
  cs.opcode = le16(ev + 4);
  return true;
}

}  // namespace pianobt
