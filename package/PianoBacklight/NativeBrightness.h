// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_BACKLIGHT_NATIVE_BRIGHTNESS_H
#define PIANO_BACKLIGHT_NATIVE_BRIGHTNESS_H

#include "Ktz8866.h"
#include "../PianoAuto/AndroidBrightness.h"

namespace pianobacklight {

constexpr char kNativeMarker[] = "PianoBacklightNativeV1";
constexpr char kNativeIdentity[] = "com.apple.WindowServer";
constexpr char kNativeRequest[] = "brightness-nits";
constexpr char kAndroidMarker[] = "PianoBacklightAndroidV1";
constexpr char kReferenceProperty[] = "piano-backlight-reference";
constexpr uint32_t kNativeApplyIntervalMs = 50;
// 2026-11-20 (BRIGHTNESS-SLIDER.md): BrightnessControl's -[BCAppleBacklightBrtControl setNits:error:] works for a non-DFR panel
// only when this registry property (OSData, first byte non-zero) is on the backlight service or a parent.
constexpr char kNativeOverride[] = "force-brightness-control";

inline bool nativeWriter(bool administrator, bool platform, const char *identity)
{
  if (administrator) return true;
  if (!platform || !identity) return false;
  for (unsigned i = 0; ; ++i) {
    if (identity[i] != kNativeIdentity[i]) return false;
    if (!kNativeIdentity[i]) return true;
  }
}

// 2026-11-20 (BRIGHTNESS-SLIDER.md): WindowServer sends the panel's brightness with every swap that changes it, CoreBrightness's
// own ramps (the power-on ramp, a dim) included. A request is the user's move only when it leaves the settled value by more
// than 2 % after the settle time (from the first request, a display power-on or a wake); the rest follow.
class NativeIntent {
 public:
  static constexpr uint64_t kSettleMs = 4000;
  enum Kind : uint8_t { kFollow = 0, kUserMove = 1 };
  void reset(uint64_t nowMs)
  {
    anchor_ = -1;
    settleUntil_ = nowMs + kSettleMs;
    armed_ = true;
  }
  Kind classify(int64_t fixed, uint64_t nowMs)
  {
    if (!armed_) reset(nowMs);   // the first request ever starts the settle time
    if (anchor_ < 0 || nowMs < settleUntil_) {
      anchor_ = fixed;
      return kFollow;
    }
    const int64_t d = fixed > anchor_ ? fixed - anchor_ : anchor_ - fixed;
    if (d * 50 <= anchor_) return kFollow;
    anchor_ = fixed;
    return kUserMove;
  }

 private:
  int64_t anchor_ = -1;
  uint64_t settleUntil_ = 0;
  bool armed_ = false;
};

// Nominal SDR slider scale. The endpoints model the safe code range, not a panel luminance measurement.
class NativeBrightness {
 public:
  explicit NativeBrightness(uint16_t floor, uint16_t max = kFullScale, bool reference = false)
    : floor_(floor), max_(max), reference_(reference) {}

  bool bounded() const { return max_ == PA_SDR_MAX_CODE; }
  uint32_t initialRevision() const { return bounded() ? 1 : 0; }
  uint32_t minFixed() const
  {
    return bounded() ? pa_sdr_floor_fixed(floor_) : ((500u * floor_ + kFullScale / 2) / kFullScale) << 16;
  }
  uint32_t maxFixed() const { return (bounded() ? 600u : 500u) << 16; }
  uint32_t midFixed() const { return bounded() ? (minFixed() + maxFixed()) / 2 : ((minNits() + 500u) / 2) << 16; }
  uint32_t minNits() const { return (minFixed() + 32768) >> 16; }
  uint32_t midNits() const { return (midFixed() + 32768) >> 16; }

  bool accepts(uint64_t fixed) const { return fixed >= minFixed() && fixed <= maxFixed(); }

  bool decode(uint64_t fixed, uint16_t &code, uint16_t *dbv = nullptr) const
  {
    if (!accepts(fixed)) return false;  // includes negative signed 16.16 values
    if (dbv) *dbv = bounded() ? pa_sdr_dbv(uint32_t(fixed)) : 0;
    if (reference_) {
      const uint16_t target = pa_sdr_code(uint32_t(fixed));
      code = fixed == minFixed() || target < floor_ ? floor_ : target;
    } else {
      const uint32_t lo = minFixed(), span = maxFixed() - lo;
      code = uint16_t(floor_ + ((fixed - lo) * (max_ - floor_) + span / 2) / span);
    }
    return true;
  }

  uint32_t milliNits(uint16_t code) const
  {
    if (bounded()) {
      if (code <= floor_) return pa_sdr_millinits(minFixed());
      if (code >= max_) return 600000;
      if (reference_) return pa_sdr_millinits(pa_sdr_fixed_for_code(code));
      return pa_sdr_millinits(pa_sdr_linear_fixed(floor_, code));
    }
    const uint32_t span = max_ - floor_;
    const uint32_t delta = code > floor_ ? code - floor_ : 0;
    return 1000 * minNits() + (1000 * (500u - minNits()) * delta + span / 2) / span;
  }

  bool applied(const Controller &ctl, uint16_t &code) const
  {
    if (!ctl.controlling() || !ctl.wantLit() || ctl.pending() || ctl.code() < floor_ || ctl.code() > max_) return false;
    code = ctl.code();  // pending() checks the last verified code and BL_EN on both ICs
    return true;
  }

  // The sink supplies typed properties. The same schema is exercised with a recording sink on the host.
  template <typename Sink> bool parameters(Sink &sink, uint16_t code) const
  {
    return sink.range("brightness", floor_, max_, code)
        && sink.range("BrightnessMilliNits", milliNits(floor_), maxFixed() / 65536 * 1000, milliNits(code))
        && sink.commit("IODisplayParameters");
  }

  template <typename Sink> bool calibration(Sink &sink) const
  {
    const uint32_t table[] = {minFixed(), midFixed(), maxFixed()};
    return sink.number("lMinProduct", table[0]) && sink.number("lMidProduct", table[1])
        && sink.number("lMaxProduct", table[2]) && sink.number("lMinPanel", table[0])
        && sink.number("lMidPanel", table[1]) && sink.number("lMaxPanel", table[2])
        && sink.commit("backlight-calibration-parameters")
        && sink.data("backlight-marketing-table", table, sizeof(table))
        && sink.data("LmaxProduct", &table[2], sizeof(table[2]))
        && sink.scalar("LminProduct", minNits()) && sink.scalar("LmidProduct", midNits());
  }

 private:
  uint16_t floor_, max_;
  bool reference_;
};

// The ALS consumer takes these fields together when selecting a curve and starting a fresh session.
template <typename Sink> bool publishAmbientSnapshot(Sink &sink, const Controller &ctl, uint32_t revision, bool reference)
{
  const pianoauto::Ambient &a = ctl.ambientState();
  return revision && sink.number("curve", reference) && sink.number("revision", revision)
      && sink.number("floor", ctl.floor()) && sink.number("target", uint32_t(a.lastTarget()))
      && sink.number("lux", a.lastLux()) && sink.number("valid", a.valid())
      && sink.number("fault", ctl.autoFault()) && sink.commit(pianoauto::kPropertyAmbientSnapshot);
}

}  // namespace pianobacklight
#endif
