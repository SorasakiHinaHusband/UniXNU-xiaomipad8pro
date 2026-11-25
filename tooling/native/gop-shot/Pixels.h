// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

static inline uint8_t gop_shot_srgb8(uint32_t sample)
{
    return (uint8_t)((sample * 255u + 511u) / 1023u);
}

// Input is little-endian B:G:R:A in memory (8:8:8:8 or 10:10:10:2).
// Output is RGBX. Scanout is opaque; the stored alpha bits do not affect the still.
static inline void gop_shot_rgbx(uint8_t pixel[4], int packed10)
{
    const uint8_t blue = pixel[0], green = pixel[1], red = pixel[2];
    if (packed10) {
        const uint32_t word = blue | ((uint32_t)green << 8) | ((uint32_t)red << 16) | ((uint32_t)pixel[3] << 24);
        pixel[0] = gop_shot_srgb8((word >> 20) & 1023u);
        pixel[1] = gop_shot_srgb8((word >> 10) & 1023u);
        pixel[2] = gop_shot_srgb8(word & 1023u);
    } else {
        pixel[0] = red;
        pixel[2] = blue;
    }
    pixel[3] = 255;
}
