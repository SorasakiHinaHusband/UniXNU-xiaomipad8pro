// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Host tests of SyncPolicy.h:
//   cc -std=c11 -Wall -Wextra -Werror sync_policy_host.c -o /tmp/sync_policy_host && /tmp/sync_policy_host
#include "../SyncPolicy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
  struct pbs_state s = {0};
  // The installed boot of 2026-11-21 11:42: CoreBrightness at its 600-nit preference, the panel at the automatic 26.16 nits.
  assert(pbs_decide(&s, 1000, 26160, 1, 1, 600000) == PBS_WAIT);            // first sight of the level: settle
  assert(pbs_decide(&s, 1500, 26160, 1, 1, 600000) == PBS_WAIT);
  assert(pbs_decide(&s, 2000, 26160, 1, 1, 600000) == PBS_SYNC);            // settled 1 s: give CoreBrightness 26.16 nits
  assert(s.synced_level == 26160 && s.repeats == 1);
  assert(pbs_decide(&s, 3500, 26160, 1, 1, 26159) == PBS_IDLE && s.repeats == 0);   // taken
  // The room darkens: the automatic ramp moves the level every 250 ms; nothing until it rests.
  for (uint32_t t = 4000, level = 23160; level > 5000; t += 250, level -= 3000)
    assert(pbs_decide(&s, t, level, 1, 1, 26159) == PBS_WAIT);
  assert(pbs_decide(&s, 6000, 5225, 1, 1, 26159) == PBS_WAIT);
  assert(pbs_decide(&s, 6900, 5225, 1, 1, 26159) == PBS_WAIT);
  assert(pbs_decide(&s, 7000, 5225, 1, 1, 26159) == PBS_SYNC);
  // Within 3 % or 0.2 nit: in step, nothing to do (CoreBrightness's own rounding).
  assert(pbs_matches(5225, 5225) && pbs_matches(5400, 5225) && !pbs_matches(5500, 5225));
  assert(pbs_matches(2293, 2400) && pbs_matches(600000, 590000) && !pbs_matches(580000, 600000));
  assert(pbs_decide(&s, 9000, 5225, 1, 1, 5300) == PBS_IDLE);
  // The user's level (a slider move ended automatic mode) and a dark panel are never touched.
  struct pbs_state u = {0};
  assert(pbs_decide(&u, 1000, 432138, 0, 1, 600000) == PBS_IDLE);
  assert(pbs_decide(&u, 9000, 432138, 0, 1, 600000) == PBS_IDLE);
  assert(pbs_decide(&u, 1000, 2293, 1, 0, 600000) == PBS_IDLE);
  assert(pbs_decide(&u, 9000, 2293, 1, 0, 600000) == PBS_IDLE);
  assert(pbs_decide(&u, 9000, 0, 1, 1, 600000) == PBS_IDLE);
  // Automatic again (a wake): the level is seen afresh and settles first.
  assert(pbs_decide(&u, 10000, 2293, 1, 1, 432000) == PBS_WAIT);
  assert(pbs_decide(&u, 11000, 2293, 1, 1, 432000) == PBS_SYNC);
  // CoreBrightness unknown (corebrightnessd restarting): nothing.
  struct pbs_state k = {0};
  assert(pbs_decide(&k, 0, 2293, 1, 1, -1) == PBS_WAIT);
  assert(pbs_decide(&k, 5000, 2293, 1, 1, -1) == PBS_IDLE);
  // A level CoreBrightness never takes: 2 s apart, three tries, then a minute's back-off; a new level tries again at once.
  struct pbs_state r = {0};
  assert(pbs_decide(&r, 0, 100000, 1, 1, 600000) == PBS_WAIT);
  assert(pbs_decide(&r, 1000, 100000, 1, 1, 600000) == PBS_SYNC);
  assert(pbs_decide(&r, 2000, 100000, 1, 1, 600000) == PBS_WAIT);         // inside the 2 s gap
  assert(pbs_decide(&r, 3000, 100000, 1, 1, 600000) == PBS_SYNC);
  assert(pbs_decide(&r, 5000, 100000, 1, 1, 600000) == PBS_SYNC && r.repeats == 3);
  assert(pbs_decide(&r, 7000, 100000, 1, 1, 600000) == PBS_IDLE);         // backing off
  assert(pbs_decide(&r, 40000, 100000, 1, 1, 600000) == PBS_IDLE);
  assert(pbs_decide(&r, 67001, 100000, 1, 1, 600000) == PBS_SYNC);        // after the minute: once more
  struct pbs_state n = r;
  n.backoff_until = 0;
  assert(pbs_decide(&n, 70000, 90000, 1, 1, 600000) == PBS_WAIT);
  assert(pbs_decide(&n, 71000, 90000, 1, 1, 600000) == PBS_SYNC && n.repeats == 1);
  // 16.16 for piano-backlight-sync: 26.16 nits, the 2.293 floor, 600 nits.
  assert(pbs_fixed(26160) == 1714422 && pbs_fixed(2293) == 150274 && pbs_fixed(600000) == 39321600);
  puts("sync policy: boot preference, settle, automatic ramp, tolerance, user level, dark, wake, unknown, back-off and 16.16 passed");
  return 0;
}
