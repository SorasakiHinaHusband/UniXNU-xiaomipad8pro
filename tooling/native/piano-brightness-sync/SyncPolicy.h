// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// piano-brightness-sync's decision (host-tested: tests/sync_policy_host.c). CoreBrightness never reads the panel back: its level
// starts from its BrightnessLevelNits preference and changes only by its own commits, so while PianoBacklight's automatic mode
// owns the panel the Control Center slider shows a stale value (2026-11-21, SLIDER-INIT.md). The daemon gives CoreBrightness the
// panel's level once that level has settled and differs from CoreBrightness's by more than 3 % (and 0.2 nit).
#ifndef PIANO_BRIGHTNESS_SYNC_POLICY_H
#define PIANO_BRIGHTNESS_SYNC_POLICY_H
#include <stdint.h>

enum {
  PBS_SETTLE_MS = 1000,    // the panel's level unchanged this long (the automatic ramp has settled)
  PBS_GAP_MS = 2000,       // at least this long between two syncs
  PBS_REPEATS = 3,         // syncs of one level CoreBrightness did not take before backing off
  PBS_BACKOFF_MS = 60000,
};
enum pbs_action { PBS_IDLE = 0, PBS_WAIT = 1, PBS_SYNC = 2 };   // nothing to do / ask again in about a second / sync now

struct pbs_state {
  uint32_t level;           // the panel's level as last seen (millinits), 0 none
  uint64_t level_since;     // when it took that value (ms)
  uint64_t last_sync;       // the last sync (ms), 0 none
  uint32_t synced_level;    // the level it gave
  uint32_t repeats;         // syncs of synced_level since CoreBrightness last matched the panel
  uint64_t backoff_until;
};

// Two levels in millinits within 3 % of the second, or 0.2 nit.
static inline int pbs_matches(uint32_t a, uint32_t b)
{
  const uint32_t d = a > b ? a - b : b - a;
  return d <= 200 || (uint64_t)d * 100 <= (uint64_t)b * 3;
}

// now: ms; panel: PianoBacklight's applied level (IODisplayParameters BrightnessMilliNits value, millinits); automatic, lit: its
// piano-backlight-auto-active and piano-backlight-lit; corebrightness: DisplayNitsKey in millinits, < 0 unknown.
static inline enum pbs_action pbs_decide(struct pbs_state *s, uint64_t now, uint32_t panel, int automatic, int lit,
                                         int64_t corebrightness)
{
  if (!panel || !automatic || !lit) {   // the user's level (CoreBrightness owns it), or dark: never touched
    s->level = 0;
    return PBS_IDLE;
  }
  if (panel != s->level) {
    s->level = panel;
    s->level_since = now;
    return PBS_WAIT;
  }
  if (now - s->level_since < PBS_SETTLE_MS) return PBS_WAIT;
  if (corebrightness < 0 || corebrightness > 0xffffffffll) return PBS_IDLE;
  if (pbs_matches((uint32_t)corebrightness, panel)) {
    s->repeats = 0;
    return PBS_IDLE;
  }
  if (now < s->backoff_until) return PBS_IDLE;
  if (s->last_sync && now - s->last_sync < PBS_GAP_MS) return PBS_WAIT;
  if (panel != s->synced_level) {
    s->repeats = 0;
  } else if (s->repeats >= PBS_REPEATS) {
    s->repeats = 0;
    s->backoff_until = now + PBS_BACKOFF_MS;
    return PBS_IDLE;
  }
  ++s->repeats;
  s->synced_level = panel;
  s->last_sync = now;
  return PBS_SYNC;
}

// millinits as PianoBacklight's signed 16.16 nits (piano-backlight-sync)
static inline int64_t pbs_fixed(uint32_t millinits) { return ((int64_t)millinits * 65536 + 500) / 1000; }

#endif
