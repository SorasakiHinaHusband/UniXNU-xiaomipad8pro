// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "WifiUserControl.h"
#include "WifiApple80211.h"
#include <cstdio>
using namespace pianowifi;
using namespace pianowifi::a80211;
static unsigned failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int main()
{
  WifiUserControl u;
  CHECK(u.allowed(u.read()));
  // Sleep preparation can disconnect without changing logical radio power.
  // Wake re-applies ON rather than supplying a new SET20. Keep the accepted
  // profile reachable, but do not bypass a teardown which is still pending.
  const uint32_t sleepLeave = u.disconnect();
  CHECK(WifiUserControl::power(u.read()) && !u.allowed(u.read()));
  u.setPower(true);
  CHECK(u.pending(sleepLeave) && !u.allowed(u.read()));
  u.stopped(sleepLeave);
  CHECK(u.allowed(u.read()));
  const uint32_t completedLeave = u.disconnect();
  u.stopped(completedLeave);
  CHECK(!u.allowed(u.read())); // ordinary leave stays idle until a command
  const uint32_t wakeOn = u.setPower(true);
  CHECK(WifiUserControl::serial(wakeOn) == WifiUserControl::serial(completedLeave));
  CHECK(u.allowed(wakeOn) && !u.radioStop());
  CHECK(u.setPower(true) == wakeOn); // repeated ON has no further effect
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    const uint32_t leave = u.disconnect();
    CHECK(!u.allowed(u.read()) && u.pending(leave) && WifiUserControl::power(leave));
    CHECK(u.disconnect() == leave); // repeat leave is idempotent
    CHECK(u.join());               // quick new join cannot skip the old teardown
    CHECK(!u.allowed(u.read()));
    u.stopped(leave);
    CHECK(u.allowed(u.read()));
    const uint32_t off = u.setPower(false);
    CHECK(!u.join() && !u.allowed(u.read()) && u.radioStop());
    CHECK(u.setPower(false) == off);
    u.setPower(true);              // quick off/on still physically cycles
    CHECK(u.radioStop() && !u.allowed(u.read()));
    u.stopped(off);                // station down alone is insufficient
    CHECK(u.radioStop() && !u.allowed(u.read()));
    u.radioOff(u.read());          // DMA quiesced and WLAN_EN low
    CHECK(!u.radioStop() && u.allowed(u.read()));
  }
  // No-profile/failure park: there is no station left, so the idle worker
  // acknowledges the leave before using the queued SET20 as a restart cue.
  const uint32_t idleLeave = u.disconnect();
  CHECK(u.join() && !u.allowed(u.read()));
  u.stopped(idleLeave);
  CHECK(u.allowed(u.read()));
  const uint32_t first = u.disconnect();
  CHECK(u.join());
  const uint32_t later = u.disconnect();
  u.stopped(first);                // a late completion cannot consume a newer stop
  CHECK(u.pending(later));
  u.stopped(later);
  CHECK(!u.pending(later) && !u.allowed(u.read()));
  CHECK(u.join() && u.allowed(u.read()));
  const uint32_t off = u.setPower(false);
  u.setPower(true);
  u.setPower(false);               // new off between physical off and acknowledgement
  u.radioOff(off);
  CHECK(u.radioStop() && u.pending(u.read()));
  u.radioOff(u.read());
  CHECK(!WifiUserControl::power(u.read()) && !u.pending(u.read()));
  u.setPower(true);
  CHECK(u.allowed(u.read()));
  // Polling GET13/19/21 before the timer does not consume events. Exactly one
  // notification survives repeated polls, with coherent current reads.
  Association canceled = {}, newJoin = {}, failed = {};
  canceled.result = 2;
  CHECK(!associationLinkDownEvent(canceled, newJoin)); // canceled2 -> fresh pending0 must not abort auto-join
  failed.result = 2;
  CHECK(associationLinkDownEvent(newJoin, failed));
  Association oldLink = {}; oldLink.associated = oldLink.authorized = true; oldLink.result = 1;
  CHECK(associationLinkDownEvent(oldLink, newJoin));
  State st;
  Association connected = {};
  connected.associated = connected.authorized = true;
  observeAssociation(st, connected, true, true);
  for (unsigned i = 0; i < 4; ++i) {
    const Association old = observeAssociation(st, Association{}, false, false);
    CHECK(associationTransition(old, st.association).disconnected);
    CHECK(!st.power && st.eventPower);
  }
  Association old = observeAssociation(st, Association{}, false, true);
  CHECK(associationTransition(old, st.association).disconnected && !st.eventPower);
  old = observeAssociation(st, Association{}, false, true);
  CHECK(!associationTransition(old, st.association).disconnected);
  std::printf("user_control_host: %s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
