// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Exclusive native-core ownership. The kernel serializes these transitions with
// its lock; firmware waits and consumer callbacks are always outside that lock.
#pragma once
#include <stdint.h>
namespace pianovideo { namespace native {
enum class Phase { Idle, Reserved, Decoding, FrameHeld, Complete, Failed, Closing, Poisoned };
struct State {
  Phase phase = Phase::Idle;
  uint32_t id = 0, serial = 0;
  uint64_t token = 0;
  int error = 0;
  bool owns(uint32_t s) const { return s && id == s && phase != Phase::Idle && phase != Phase::Poisoned; }
  int acquire(uint32_t *s)
  {
    if (!s) return -22;
    if (phase != Phase::Idle || serial == 0xffff) return -16;
    id = 0x56350000u | ++serial; error = 0; token = 0; phase = Phase::Reserved; *s = id; return 0;
  }
  int begin(uint32_t s, uint64_t t)
  {
    if (!owns(s)) return -22;
    if (phase != Phase::Reserved) return -45;
    token = t; phase = Phase::Decoding; return 0;
  }
  void decoded(int rc)
  {
    if (phase != Phase::Decoding) return;
    if (rc && !error) error = rc;
    phase = rc ? Phase::Failed : Phase::FrameHeld;
  }
  int release(uint32_t s, uint64_t t)
  {
    if (!owns(s) || t != token || phase != Phase::FrameHeld) return -22;
    phase = Phase::Complete; return 0;
  }
  int finish(uint32_t s) const
  {
    if (!owns(s)) return -22;
    return phase == Phase::Complete ? 0 : error ? error : -45;
  }
  int closing(uint32_t s)
  {
    if (!owns(s)) return -22;
    if (phase == Phase::Decoding || phase == Phase::FrameHeld || phase == Phase::Closing) return -16;
    phase = Phase::Closing; return 0;
  }
  void closed(bool dmaRetained)
  {
    if (phase != Phase::Closing) return;
    phase = dmaRetained ? Phase::Poisoned : Phase::Idle;
    id = 0;
  }
};
} }
