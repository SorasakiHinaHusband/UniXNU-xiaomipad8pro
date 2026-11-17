// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Bounded V4 layout, from VPU35 gen2 size facts at the upstream pin in UPSTREAM.
// This is deliberately only the 320x240, 8-bit, four-pipe AVC self-test, not a general size calculator.
#pragma once
#include <stdint.h>
#include "VideoHw.h"

namespace pianovideo
{
namespace session
{
constexpr uint32_t kArenaBase = kIovaBase + kUcRegionBytes, kArenaBytes = 64 * 1024 * 1024;
constexpr unsigned kArenaPages = kArenaBytes / 4096, kMaxN = 64, kSlots = 140;
constexpr uint32_t kInputBytes = 7077888, kRawBytes = 147456, kDpbBytes = 155648, kUvOffset = 98304;
constexpr uint32_t comvBytes(unsigned n) { return 43008 * n + 512; }
enum BufferType : uint32_t { Bitstream = 1, Raw = 2, Partial = 5, Dpb = 6, Bin = 7, Line = 8, Comv = 10, NonComv = 11, Persist = 12 };
constexpr uint32_t scratchType[5] = {Bin, Comv, NonComv, Line, Partial};
constexpr uint32_t scratchBytes(unsigned i, unsigned n)
{
  return i == 0 ? 5652480 : i == 1 ? comvBytes(n) : i == 2 ? 557056 : i == 3 ? 685824 : i == 4 ? 196608 : 0;
}
struct Buffer {
  uint32_t type = 0, index = 0, off = 0, capacity = 0, size = 0, port = 0;
  bool queued = false, pendingRelease = false, readonly = false, released = false;
  uint32_t iova() const { return kArenaBase + off; }
};
struct Layout {
  Buffer buf[kSlots] = {};
  unsigned used = 0;
  uint32_t bytes = 0;
  bool add(uint32_t type, unsigned index, uint32_t capacity, uint32_t port)
  {
    const uint64_t off = (uint64_t(bytes) + 16383) & ~16383ull;
    const uint64_t end = off + ((uint64_t(capacity) + 16383) & ~16383ull) + 16384; // invalid guard, including XNU page boundary
    if (!capacity || used == kSlots || end > kArenaBytes)
      return false;
    Buffer &b = buf[used++];
    b.type = type;
    b.index = index;
    b.off = uint32_t(off);
    b.capacity = capacity;
    b.size = capacity;
    b.port = port;
    bytes = uint32_t(end);
    return true;
  }
  bool init()
  {
    used = bytes = 0;
    if (!add(Bitstream, 0, kInputBytes, 1) || !add(Persist, 0, 498176, 0))
      return false;
    for (unsigned set = 0; set < 2; ++set)
      for (unsigned i = 0; i < 5; ++i)
        if (!add(scratchType[i], set, scratchBytes(i, kMaxN), 1))
          return false;
    for (unsigned i = 0; i < kMaxN; ++i)
      if (!add(Dpb, i, kDpbBytes, 2) || !add(Raw, i, kRawBytes, 2))
        return false;
    return used == kSlots;
  }
  Buffer *find(uint32_t type, uint32_t index)
  {
    for (unsigned i = 0; i < used; ++i)
      if (buf[i].type == type && buf[i].index == index)
        return &buf[i];
    return nullptr;
  }
  bool mapped(unsigned page) const
  {
    const uint32_t off = page * 4096;
    for (unsigned i = 0; i < used; ++i)
      if (off >= buf[i].off && off - buf[i].off < ((buf[i].capacity + 4095) & ~4095u))
        return true;
    return false;
  }
};
// CPU hash reads active pixels only. Both kernel SHA256 and the independent host implementation use this traversal.
template <class Update> void activeRows(const volatile uint8_t *base, Update update)
{
  uint8_t row[320];
  for (unsigned plane = 0; plane < 2; ++plane)
    for (unsigned y = 0; y < (plane ? 120u : 240u); ++y) {
      const uint32_t off = (plane ? kUvOffset : 0) + 384 * y;
      for (unsigned x = 0; x < 320; ++x)
        row[x] = base[off + x];
      update(row, sizeof row);
    }
}
inline void hexDigest(const uint8_t digest[32], char text[65])
{
  const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i) {
    text[2 * i] = hex[digest[i] >> 4];
    text[2 * i + 1] = hex[digest[i] & 15];
  }
  text[64] = 0;
}
inline bool sameText(const char *a, const char *b)
{
  for (unsigned i = 0; i < 65; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}
} // namespace session
} // namespace pianovideo
