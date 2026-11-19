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

inline bool nativeWriter(bool administrator, bool platform, const char *identity)
{
  if (administrator) return true;
  if (!platform || !identity) return false;
  for (unsigned i = 0; ; ++i) {
    if (identity[i] != kNativeIdentity[i]) return false;
    if (!kNativeIdentity[i]) return true;
  }
}

// Nominal SDR slider scale. The endpoints model the safe code range, not a panel luminance measurement.
class NativeBrightness {
 public:
  explicit NativeBrightness(uint16_t floor, uint16_t max = kFullScale, bool reference = false)
    : floor_(floor), max_(max), reference_(reference) {}

  bool bounded() const { return max_ == PA_SDR_MAX_CODE; }
  uint32_t minFixed() const
  {
    return bounded() ? pa_sdr_fixed_for_code(floor_) : ((500u * floor_ + kFullScale / 2) / kFullScale) << 16;
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
      const uint32_t span = max_ - floor_;
      return pa_sdr_millinits(minFixed() + uint32_t((uint64_t(maxFixed() - minFixed()) * (code - floor_) + span / 2) / span));
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

}  // namespace pianobacklight
#endif
