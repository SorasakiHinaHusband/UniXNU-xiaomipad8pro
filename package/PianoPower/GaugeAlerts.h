// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Read-only gauge alerts. ISC level 3 asserts DTPT; an unrecognized ISC
// byte is unknown, never an alarm level or permission to raise current.
#ifndef PIANO_POWER_GAUGE_ALERTS_H
#define PIANO_POWER_GAUGE_ALERTS_H
#include <stdint.h>

namespace pianopower {
struct GaugeAlerts {
  uint8_t isc[2] = {}, soa[2] = {};
  // Same per-field bits as PianoCharge.SafetyValid (bit 0 is CP control).
  uint32_t valid = 0, alarms = 0;

  void sampled(uint32_t readValid)
  {
    valid = readValid & 0x1e;
    for (unsigned i = 0; i < 2; ++i) {
      const uint32_t ib = 2u << (2 * i), sb = 4u << (2 * i);
      if (isc[i] > 3) valid &= ~ib;
      // Only a recognized, successful read can replace an earlier vote.
      // A missing other field cannot hide a confirmed alarm on this one.
      if (valid & ib) alarms = isc[i] == 3 ? alarms | ib : alarms & ~ib;
      if (valid & sb) alarms = soa[i] ? alarms | sb : alarms & ~sb;
    }
  }
  bool complete() const { return valid == 0x1e; }
  bool active() const { return alarms != 0; }
};
}  // namespace pianopower
#endif
