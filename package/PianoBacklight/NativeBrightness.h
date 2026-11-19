// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_BACKLIGHT_NATIVE_BRIGHTNESS_H
#define PIANO_BACKLIGHT_NATIVE_BRIGHTNESS_H

#include "Ktz8866.h"

namespace pianobacklight {

constexpr char kNativeMarker[] = "PianoBacklightNativeV1";
constexpr char kNativeIdentity[] = "com.apple.WindowServer";
constexpr char kNativeRequest[] = "brightness-nits";
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
  explicit NativeBrightness(uint16_t floor) : floor_(floor) {}

  uint32_t minNits() const { return (500u * floor_ + kFullScale / 2) / kFullScale; }
  uint32_t midNits() const { return (minNits() + 500u) / 2; }

  bool decode(uint64_t fixed, uint16_t &code) const
  {
    const uint32_t lo = minNits() << 16, hi = 500u << 16;
    if (fixed < lo || fixed > hi) return false;  // includes negative signed 16.16 values
    code = uint16_t(floor_ + ((fixed - lo) * (kFullScale - floor_) + (hi - lo) / 2) / (hi - lo));
    return true;
  }

  uint32_t milliNits(uint16_t code) const
  {
    const uint32_t span = kFullScale - floor_;
    const uint32_t delta = code > floor_ ? code - floor_ : 0;
    return 1000 * minNits() + (1000 * (500u - minNits()) * delta + span / 2) / span;
  }

  bool applied(const Controller &ctl, uint16_t &code) const
  {
    if (!ctl.controlling() || !ctl.wantLit() || ctl.pending() || ctl.code() < floor_) return false;
    code = ctl.code();  // pending() checks the last verified code and BL_EN on both ICs
    return true;
  }

  // The sink supplies typed properties. The same schema is exercised with a recording sink on the host.
  template <typename Sink> bool parameters(Sink &sink, uint16_t code) const
  {
    return sink.range("brightness", floor_, kFullScale, code)
        && sink.range("BrightnessMilliNits", minNits() * 1000, 500000, milliNits(code))
        && sink.commit("IODisplayParameters");
  }

  template <typename Sink> bool calibration(Sink &sink) const
  {
    const int32_t table[] = {int32_t(minNits() << 16), int32_t(midNits() << 16), 500 << 16};
    return sink.number("lMinProduct", table[0]) && sink.number("lMidProduct", table[1])
        && sink.number("lMaxProduct", table[2]) && sink.number("lMinPanel", table[0])
        && sink.number("lMidPanel", table[1]) && sink.number("lMaxPanel", table[2])
        && sink.commit("backlight-calibration-parameters")
        && sink.data("backlight-marketing-table", table, sizeof(table))
        && sink.data("LmaxProduct", &table[2], sizeof(table[2]))
        && sink.scalar("LminProduct", minNits()) && sink.scalar("LmidProduct", midNits());
  }

 private:
  uint16_t floor_;
};

}  // namespace pianobacklight
#endif
