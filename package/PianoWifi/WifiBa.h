// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 8: the receive block-ack agreements (boot 10s) and the reorder unit's queue memory.
//
// Boot 10r (2026-11-14) associated as HT and VHT and received 4.3 Mbit/s of TCP, against legacy's 22: the access point asked for
// a block-ack agreement on TIDs 0, 7 and 3 (window 64, A-MSDU allowed) about once a second, and nobody answered. An agreement is
// two things here: the reorder unit's queue of that TID set up for the window (a fresh descriptor, WifiAssoc.h reoQueueInit, handed
// to the firmware by its reorder-queue command with the window, which also tells the hardware to answer A-MPDUs with block acks),
// and our ADDBA response (802.11 9.6.4.3) sent as a management frame. A DELBA of the access point's sets the TID back to a
// window of one. A request is answered after the queue's command, so the firmware has the window before the first A-MPDU.
//
// The queue memory (ReoPool): the reorder unit keeps queue descriptors in its own cache and writes them back when it pleases; only
// a host command of its command ring (which this driver does not run) flushes one. Boot 10r's joins 4 to 7 reused the memory of
// the queues of an earlier QoS peer (two blocks alternated) and the access point's first frames of each new association (its
// four-way handshake's message 1, on a TID the earlier peer had used) were never delivered: inferred, the unit used the old
// peer's cached queue with its old sequence window and dropped them; joins whose QoS queues were fresh memory all worked. So every
// join's queues and every agreement's queue are taken from a pool that hands out each address once while the chip stays powered
// (a pass's restart powers it down and the pool starts again); when the pool is used up it starts again from its beginning,
// counted and logged.
//
// Written from IEEE 802.11 (the action frames' fields) and the firmware's own behaviour (docs/drivers/wifi.md section 3); no code
// of another driver is here. Nothing here allocates, logs or sleeps; addresses never leave it.
#pragma once
#include <stdint.h>
#include "WifiHw.h"
#include "WifiAssoc.h"

namespace pianowifi {

constexpr uint8_t kDot11Action = 0xd0;              // the management subtype "action"
constexpr uint8_t kBaCategory = 3;                  // block ack
constexpr uint8_t kBaAddbaRequest = 0, kBaAddbaResponse = 1, kBaDelba = 2;
constexpr uint8_t kBaNone = 0xff;                   // baParse: not a block-ack frame of ours
constexpr uint16_t kDot11StatusDeclined = 37;       // "the request has been declined"
constexpr uint32_t kBaWindowMax = 64;               // HT and VHT's largest buffer (HE's 256 is not spoken)
constexpr uint32_t kBaTids = 8;                     // the QoS user priorities an agreement can name
constexpr uint32_t kBaRequestBody = 9, kBaResponseBody = 9, kBaDelbaBody = 6;
constexpr uint32_t kBaFrameSlot = 128;              // one response frame per TID in the action frame buffer
constexpr uint32_t kBaDescBase = 0x0ba00000;        // the management frames' completion ids of the responses (TID in the low bits)
// piano-wifi-rxba's values
enum BaMode : uint32_t { kBaModeOff = 0, kBaModeAccept = 1, kBaModeDecline = 2, kBaModeAcceptAmsdu = 3 };

inline const char *baModeName(uint32_t mode)
{
  switch (mode) {
  case kBaModeOff: return "not answered";
  case kBaModeAccept: return "accepted (no A-MSDU)";
  case kBaModeDecline: return "declined";
  case kBaModeAcceptAmsdu: return "accepted (A-MSDU as asked)";
  default: return "?";
  }
}

struct BaRequest {
  uint8_t dialog = 0, tid = 0;
  bool amsdu = false, immediate = false;
  uint16_t window = 0, timeout = 0, ssn = 0;
};

struct BaDelbaInfo {
  uint8_t tid = 0;
  bool initiator = false;   // the access point ends its own (the originator's) agreement
  uint16_t reason = 0;
};

namespace badetail {
inline uint32_t get16(const uint8_t *at) { return uint32_t(at[0] | at[1] << 8); }
inline void put16(uint8_t *at, uint32_t v)
{
  at[0] = uint8_t(v);
  at[1] = uint8_t(v >> 8);
}
inline bool same(const uint8_t *a, const uint8_t *b)
{
  for (unsigned i = 0; i < 6; ++i)
    if (a[i] != b[i]) return false;
  return true;
}
}  // namespace badetail

// A management frame: a block-ack action of the network's, addressed to us. Returns its action (kBaAddbaRequest with `req`
// filled, kBaDelba with `del`, kBaAddbaResponse) or kBaNone; every field is checked against the frame's length.
inline uint8_t baParse(const uint8_t *f, uint32_t length, const uint8_t *own, const uint8_t *bssid, BaRequest &req,
                       BaDelbaInfo &del)
{
  using namespace badetail;
  if (!f || !own || !bssid || length < kDot11HeaderBytes + 2) return kBaNone;
  if ((f[0] & (kDot11FcTypeMask | kDot11FcSubtypeMask)) != kDot11Action || (f[1] & 0x40)) return kBaNone;   // not protected
  if (!same(f + 4, own) || !same(f + 10, bssid) || !same(f + 16, bssid)) return kBaNone;
  const uint8_t *b = f + kDot11HeaderBytes;
  const uint32_t n = length - kDot11HeaderBytes;
  if (b[0] != kBaCategory) return kBaNone;
  switch (b[1]) {
  case kBaAddbaRequest: {
    if (n < kBaRequestBody) return kBaNone;
    const uint32_t params = get16(b + 3);
    req.dialog = b[2];
    req.amsdu = (params & 1u) != 0;
    req.immediate = (params & 2u) != 0;
    req.tid = uint8_t((params >> 2) & 0xfu);
    req.window = uint16_t((params >> 6) & 0x3ffu);
    req.timeout = uint16_t(get16(b + 5));
    req.ssn = uint16_t(get16(b + 7) >> 4);
    return kBaAddbaRequest;
  }
  case kBaDelba: {
    if (n < kBaDelbaBody) return kBaNone;
    const uint32_t params = get16(b + 2);
    del.initiator = (params & (1u << 11)) != 0;
    del.tid = uint8_t((params >> 12) & 0xfu);
    del.reason = uint16_t(get16(b + 4));
    return kBaDelba;
  }
  case kBaAddbaResponse: return n >= kBaResponseBody ? kBaAddbaResponse : kBaNone;
  default: return kBaNone;
  }
}

// What a request gets under piano-wifi-rxba: nothing (off), a decline, or an agreement with its window and A-MSDU bit.
struct BaAnswer {
  bool answer = false, accept = false, amsdu = false;
  uint16_t status = 0, window = 0;
  const char *why = "";
};

inline BaAnswer baDecide(const BaRequest &r, uint32_t mode)
{
  BaAnswer a;
  if (mode == kBaModeOff || mode > kBaModeAcceptAmsdu) {
    a.why = "piano-wifi-rxba=0";
    return a;
  }
  a.answer = true;
  if (mode == kBaModeDecline) {
    a.status = kDot11StatusDeclined;
    a.why = "piano-wifi-rxba=2";
    return a;
  }
  if (r.tid >= kBaTids) {
    a.status = kDot11StatusDeclined;
    a.why = "a TID above 7";
    return a;
  }
  if (!r.immediate) {
    a.status = kDot11StatusDeclined;
    a.why = "a delayed block ack";
    return a;
  }
  a.accept = true;
  a.window = uint16_t(r.window == 0 || r.window > kBaWindowMax ? kBaWindowMax : r.window);
  a.amsdu = mode == kBaModeAcceptAmsdu && r.amsdu;
  return a;
}

// Our ADDBA response: the request's dialog token, TID, policy and timeout; the status; the window and A-MSDU bit we accept.
inline uint32_t baBuildResponse(uint8_t *f, uint32_t capacity, const uint8_t *own, const uint8_t *bssid, uint16_t seq,
                                const BaRequest &r, const BaAnswer &a)
{
  using namespace badetail;
  const uint32_t bytes = kDot11HeaderBytes + kBaResponseBody;
  if (!f || capacity < bytes || !own || !bssid) return 0;
  for (uint32_t i = 0; i < bytes; ++i) f[i] = 0;
  f[0] = kDot11Action;
  for (unsigned i = 0; i < 6; ++i) {
    f[4 + i] = bssid[i];
    f[10 + i] = own[i];
    f[16 + i] = bssid[i];
  }
  put16(f + 22, uint32_t(seq) << kStaFrameSeqShift);
  uint8_t *b = f + kDot11HeaderBytes;
  b[0] = kBaCategory;
  b[1] = kBaAddbaResponse;
  b[2] = r.dialog;
  put16(b + 3, a.status);
  const uint32_t window = a.accept ? a.window : 0;
  put16(b + 5, (a.amsdu ? 1u : 0u) | (r.immediate ? 2u : 0u) | uint32_t(r.tid & 0xfu) << 2 | (window & 0x3ffu) << 6);
  put16(b + 7, a.accept ? r.timeout : 0u);
  return bytes;
}

// ------------------------------------------------------------------------------------------ the reorder queues' memory
// A pool of window memory that hands out each address once (512-byte steps: a QoS queue descriptor's size, and a multiple of
// the unit's 128-byte alignment) until it is used up, then starts again.
struct ReoPool {
  uint8_t *va = nullptr;
  uint64_t bus = 0;
  uint32_t bytes = 0, used = 0;
  uint32_t takes = 0, wraps = 0;
};

constexpr uint32_t kReoPoolStep = kReoQueueQosBytes;   // 512
constexpr uint32_t kReoPoolBytes = 2u << 20;           // 2 MiB: about 240 joins' 17 queues, or 4096 agreements

inline uint32_t reoPoolRound(uint32_t n) { return (n + kReoPoolStep - 1) / kReoPoolStep * kReoPoolStep; }

inline void reoPoolInit(ReoPool &p, uint8_t *va, uint64_t bus, uint32_t bytes)
{
  p = ReoPool{};
  p.va = va;
  p.bus = bus;
  p.bytes = bytes / kReoPoolStep * kReoPoolStep;
}

// n bytes never handed out before (since the pool began or last started again), zeroed; false when the pool has no memory.
inline bool reoPoolTake(ReoPool &p, uint32_t n, uint8_t **va, uint64_t *bus, bool *wrapped)
{
  const uint32_t want = reoPoolRound(n);
  if (wrapped) *wrapped = false;
  if (!p.va || !want || want > p.bytes) return false;
  if (p.used + want > p.bytes) {
    p.used = 0;
    ++p.wraps;
    if (wrapped) *wrapped = true;
  }
  uint8_t *at = p.va + p.used;
  for (uint32_t i = 0; i < want; ++i) at[i] = 0;
  if (va) *va = at;
  if (bus) *bus = p.bus + p.used;
  p.used += want;
  ++p.takes;
  return true;
}

}  // namespace pianowifi
