// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: the stock-level CPU policy of piano-cpufreq=3 (2026-11-22, the owner's "performance first" priority), selected at
// run time by machdep.piano_cpufreq_perf (boot default piano-cpufreq-perf). Portable; host-tested by tests/thermal_host.cpp.
//
// What the stock Android of this tablet does (numbers as behaviour; docs/drivers/thermal.md section 14):
//   - a virtual skin temperature: 0.500 battery + 0.131 quiet_therm - 0.009 charger1_therm + 0.089 wifi_therm + 0.232 cpu_therm
//     (the msm thermistor) + 1.6 C, polled every 2 s;
//   - per cluster a step table on that temperature (the "normal" scene): from 15 C cpu0-5 at most 2745.6 MHz and cpu6-7 at most
//     3072 MHz, lower steps as the skin warms (37 C: 2400 / 1958.4 ...; 48 C: 748.8, 44 C: 1017.6), each step with its own clear
//     point (hysteresis);
//   - the UI boosts (app launch / animation hints): cpu0-5 at 2745.6 and cpu6-7 at 4089.6 MHz for 0.4-1.5 s, allowed until the skin
//     reaches 48 C.
// Here: virtualSkin() over the PMIC ADC's thermistors (PianoThermalAdc), SkinCap the step table with its hysteresis, Burst the boost:
// macOS gives the kernel no launch/animation hint, so a burst opens at the ONSET of a load (the governor wanted more than its
// ceiling on this tick but not on the tick before), lasts at most windowMs, and the next one needs another onset and gapMs since the
// last end: a sustained load stays at the step table's cap, as on Android without hints.
#ifndef PIANO_THERMAL_PERF_H
#define PIANO_THERMAL_PERF_H
#include <stdint.h>

namespace pianothermal::perf {

// The virtual sensor's inputs, in the stock order.
enum SkinInput : unsigned { kSkinBattery = 0, kSkinQuiet, kSkinCharger, kSkinWifi, kSkinMsm, kSkinInputs };
constexpr int32_t kSkinWeight[kSkinInputs] = {500, 131, -9, 89, 232};   // per mille of the stock weight_sum 1000 (they add to 943)
constexpr int32_t kSkinWeightSum = 1000, kSkinWeightAll = 943;
constexpr int32_t kSkinOffsetMilli = 1600;
constexpr const char *kSkinChannel[kSkinInputs] = {"batt", "quiet", "charger", "wifi", "msm"};   // AdcChannels.h names

struct Skin {
  bool valid = false;
  int32_t milliC = 0;
  uint32_t used = 0;   // bit i: input i was in the sum
};

// The stock sum: sum(weight x reading) / 1000 + 1.6 C (the weights add to 943, so it reads ~1.6 C under the mean). With an input
// missing the present ones are scaled up to the full 943 (the missing one counted at their weighted mean). Valid only with the
// battery and at least one of the two skin thermistors (quiet, msm): without them the number is not the stock sensor's.
inline Skin virtualSkin(const int32_t milliC[kSkinInputs], uint32_t validMask)
{
  Skin s;
  if (!(validMask & (1u << kSkinBattery)) || !(validMask & ((1u << kSkinQuiet) | (1u << kSkinMsm)))) return s;
  int64_t sum = 0, w = 0;
  for (unsigned i = 0; i < kSkinInputs; ++i) {
    if (!(validMask & (1u << i))) continue;
    sum += int64_t(kSkinWeight[i]) * milliC[i];
    w += kSkinWeight[i];
    s.used |= 1u << i;
  }
  if (w <= 0) return Skin();
  s.milliC = int32_t(sum * kSkinWeightAll / (w * kSkinWeightSum)) + kSkinOffsetMilli;
  s.valid = true;
  return s;
}

struct Step {
  int32_t trigMilli, clrMilli;
  uint32_t khz;
};
// The normal scene's tables (trigger, clear, the cap at and above the trigger).
constexpr Step kCluster0[] = {{15000, 13000, 2745600}, {35000, 32000, 2745600}, {37000, 35000, 2400000}, {39000, 37000, 1996800},
                              {41000, 39000, 1555200}, {43000, 41000, 1555200}, {44000, 43000, 1555200}, {45000, 44000, 1363200},
                              {46000, 45000, 960000},  {48000, 46000, 748800}};
constexpr Step kCluster1[] = {{15000, 13000, 3072000}, {34000, 32000, 2649600}, {37000, 35000, 1958400}, {39000, 38000, 1958400},
                              {39500, 39000, 1689600}, {40000, 39500, 1401600}, {44000, 42000, 1017600}};
constexpr unsigned kCluster0Steps = sizeof kCluster0 / sizeof kCluster0[0], kCluster1Steps = sizeof kCluster1 / sizeof kCluster1[0];

// One cluster's step: -1 below the first trigger (no cap), else the index of the step in force. Up as far as the triggers allow,
// down only under the clear point of the step in force. Without a valid reading the step stays, and below the first trigger the
// first step's cap applies (a missing skin reading is not "cold").
class SkinCap {
 public:
  uint32_t update(const Step *t, unsigned n, bool valid, int32_t milliC)
  {
    if (!n) return 0;
    if (!valid) return step_ < 0 ? t[0].khz : t[step_].khz;
    while (step_ + 1 < int(n) && milliC >= t[step_ + 1].trigMilli) ++step_;
    while (step_ >= 0 && milliC < t[step_].clrMilli) --step_;
    return step_ < 0 ? 0 : t[step_].khz;
  }
  int step() const { return step_; }
  void reset() { step_ = -1; }

 private:
  int step_ = -1;
};

struct BurstConfig {
  uint32_t khz = 0;              // the burst's cap (0: no burst)
  uint32_t windowMs = 1500;      // the longest burst (the stock first-launch boost)
  uint32_t gapMs = 500;          // from the end of one burst to the next onset that may open one
  int32_t limitMilli = 48000;    // no burst from this skin temperature up (the stock boost limit)
};

class Burst {
 public:
  // One load decision at nowMs: wantsMore = the governor wanted more than its ceiling without the burst (its last decision);
  // allowed = the profile allows bursts (Balanced) and the cluster governs. True while the burst is open.
  bool tick(const BurstConfig &c, uint64_t nowMs, bool wantsMore, bool allowed, bool skinValid, int32_t skinMilli)
  {
    const bool onset = wantsMore && !wanted_;
    wanted_ = wantsMore;
    const bool hot = skinValid && skinMilli >= c.limitMilli;
    if (open_ && (!allowed || hot || !c.khz || nowMs - startMs_ >= c.windowMs || !wantsMore)) close(nowMs);
    if (!open_ && onset && allowed && !hot && c.khz && c.windowMs && (!ended_ || nowMs - endMs_ >= c.gapMs)) {
      open_ = true;
      startMs_ = nowMs;
      ++windows_;
    }
    return open_;
  }
  bool open() const { return open_; }
  uint32_t windows() const { return windows_; }
  uint64_t openMs(uint64_t nowMs) const { return totalMs_ + (open_ ? nowMs - startMs_ : 0); }

 private:
  void close(uint64_t nowMs)
  {
    open_ = false;
    ended_ = true;
    endMs_ = nowMs;
    totalMs_ += nowMs - startMs_;
  }
  bool open_ = false, wanted_ = false, ended_ = false;
  uint64_t startMs_ = 0, endMs_ = 0, totalMs_ = 0;
  uint32_t windows_ = 0;
};

// The perf cap handed to the Governor (Governor::setRuntimeCaps): the step table's, raised to the burst's while one is open; 0 (no
// cap) only when the skin is below the first trigger and no burst is open.
inline uint32_t perfCapKhz(uint32_t skinKhz, bool burstOpen, uint32_t burstKhz)
{
  if (!burstOpen || !burstKhz) return skinKhz;
  if (!skinKhz) return 0;
  return burstKhz > skinKhz ? burstKhz : skinKhz;
}

// Load decisions between downs for a down hold of downMs at tickMs per decision (at least 1).
inline uint32_t downTicksFor(uint32_t downMs, uint32_t tickMs)
{
  const uint32_t n = tickMs ? (downMs + tickMs - 1) / tickMs : 1;
  return n ? n : 1;
}

// ------------------------------------------------------------------------------------------------- the launch and input boosts
// (owner 2026-11-22 01:02: short boosts as on Android; CPU-PERF.md section 7). The stock perf HAL's hints on this SoC
// (perfboostsconfig.xml, Target sun): first app launch 0x1081 = cpu0-5 min = max 2745.6 MHz, cpu6-7 min = max 4089.6 MHz for
// 1.5 s, sched_boost 1, GPU min_pwrlevel 9 (443 MHz) .. max_pwrlevel 2 (967 MHz), kgsl touch_wake; next launch 0x1082 the same
// for 0.4 s; animation 0x1083 the CPU pin for 0.4 s; scroll / drag 0x1080 / 0x1087 sched_boost 2 and WALT adaptive floors
// (0.6-0.8 / 1.0-1.3 GHz) while the gesture lasts. macOS gives the kernel no such hints, so the events are our own: an exec of an
// app bundle's executable (a kauth file-operation listener) for the launch, and any HID report our input kexts send
// (PianoPerfInput.h) for the input boost.

// The executable of an app bundle: ".app/Contents/MacOS/" in the path, and not an XPC service or a framework's helper.
inline bool isAppLaunchPath(const char *p)
{
  if (!p) return false;
  auto has = [p](const char *needle) {
    for (const char *s = p; *s; ++s) {
      const char *a = s, *b = needle;
      while (*a && *b && *a == *b) ++a, ++b;
      if (!*b) return true;
    }
    return false;
  };
  return has(".app/Contents/MacOS/") && !has("/XPCServices/") && !has(".framework/");
}

// The short name of the app (the bundle's last path component without ".app") into out, for the log.
inline void appName(const char *p, char *out, unsigned size)
{
  if (!size) return;
  out[0] = 0;
  if (!p) return;
  const char *end = nullptr;
  for (const char *s = p; *s; ++s)
    if (s[0] == '.' && s[1] == 'a' && s[2] == 'p' && s[3] == 'p' && s[4] == '/') end = s;
  if (!end) return;
  const char *start = end;
  while (start > p && start[-1] != '/') --start;
  unsigned n = 0;
  while (start < end && n + 1 < size) out[n++] = *start++;
  out[n] = 0;
}

enum class BoostKind : uint8_t { none = 0, input = 1, launch = 2 };
inline const char *boostName(BoostKind k) { return k == BoostKind::launch ? "launch" : k == BoostKind::input ? "input" : "none"; }

// Which boost holds at nowNs: a launch within launchMs of its exec, else an input event within inputMs of the last one. An event
// stamped a little after nowNs (written by another thread after this one read the clock) counts as now.
inline BoostKind boostAt(uint64_t nowNs, uint64_t launchNs, uint32_t launchMs, uint64_t inputNs, uint32_t inputMs)
{
  auto within = [nowNs](uint64_t at, uint32_t ms) {
    if (!at || !ms) return false;
    if (at >= nowNs) return true;
    return nowNs - at < uint64_t(ms) * 1000000ull;
  };
  if (within(launchNs, launchMs)) return BoostKind::launch;
  if (within(inputNs, inputMs)) return BoostKind::input;
  return BoostKind::none;
}

// High Power (Profile::performance, macOS Energy Mode): the perf cap is the cluster's top level (piano-cpufreq-hpm0/1, default the
// tables' tops 3532.8 / 4320 MHz), over the firmware's sustained level unless the firmware refused such a level this boot; no skin
// table, no onset burst (the ceiling is already the top), the boosts as in Balanced; the hard caps, the power cap and the thermal
// limit (piano-cpufreq-hot) stay on top in Governor::ceilingFor.
struct HighPowerCap {
  uint32_t khz;
  bool aboveSustained;
};
inline HighPowerCap highPowerCap(uint32_t topKhz, bool aboveRefused) { return HighPowerCap{topKhz, !aboveRefused}; }

}  // namespace pianothermal::perf

#endif
