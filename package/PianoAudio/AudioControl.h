/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The IOAudio2 SetControlValue scalar can be an integer step or a normalized
 * scalar.  Keep the conversion representation tolerant: the HAL's control
 * dictionary advertises integer steps, while some 27.3 paths pass a
 * Float32/Float64 scalar in the same 64-bit slot. */
#ifndef PIANO_AUDIO_CONTROL_H
#define PIANO_AUDIO_CONTROL_H
#include <stdint.h>

static inline int piano_audio_control_percent(uint64_t raw)
{
    if (raw <= 1) return raw ? 100 : 0;
    if (raw <= 100) return (int)raw;
    union { uint64_t u; double d; } d = {raw};
    if ((raw >> 52) >= 0x3fd && (raw >> 52) <= 0x3ff && d.d >= 0.0 && d.d <= 1.0)
        return (int)(d.d * 100.0 + 0.5);
    union { uint32_t u; float f; } f = {(uint32_t)raw};
    if (f.f >= 0.0f && f.f <= 1.0f) return (int)(f.f * 100.0f + 0.5f);
    return -1;
}

static inline int piano_audio_control_bool(uint64_t raw)
{
    int p = piano_audio_control_percent(raw);
    return p < 0 ? -1 : (p != 0);
}

#endif
