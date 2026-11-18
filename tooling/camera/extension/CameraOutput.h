// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Output-only geometry and pacing. The hardware sensor remains at 15 fps.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { kPianoNativeFps = 15, kPianoCompatMaxFps = 30, kPianoCompatWidth = 640, kPianoCompatHeight = 480 };
#define PIANO_OUTPUT_MAX_SOURCE_AGE_NS UINT64_C(250000000)

typedef struct {
  uint32_t width, height, minFps, maxFps;
} PianoOutputFormat;

typedef struct {
  uint32_t x, y, width, height;
} PianoOutputRect;

static inline PianoOutputFormat pianoOutputFormat(uint32_t nativeWidth, uint32_t nativeHeight, unsigned index)
{
  PianoOutputFormat f = {nativeWidth, nativeHeight, kPianoNativeFps, kPianoNativeFps};
  if (index == 1) {
    f.width = kPianoCompatWidth;
    f.height = kPianoCompatHeight;
    f.maxFps = kPianoCompatMaxFps;
  }
  return f;
}

static inline int pianoOutputDurationValid(unsigned index, int64_t value, int32_t timescale)
{
  if (index > 1 || value <= 0 || timescale <= 0) return 0;
  const int maxFps = index ? kPianoCompatMaxFps : kPianoNativeFps;
  return (__int128)value * maxFps >= timescale && (__int128)value * kPianoNativeFps <= timescale;
}

static inline int pianoOutputSourceFresh(uint64_t now, uint64_t source)
{
  return source && now >= source && now - source <= PIANO_OUTPUT_MAX_SOURCE_AGE_NS;
}

// Fit the entire source, rounding at most half a pixel, then center black letterboxing.
static inline PianoOutputRect pianoOutputFit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
  PianoOutputRect r = {0, 0, 0, 0};
  if (!sw || !sh || !dw || !dh) return r;
  if ((uint64_t)dw * sh <= (uint64_t)dh * sw) {
    r.width = dw;
    r.height = (uint32_t)(((uint64_t)sh * dw + sw / 2) / sw);
    if (!r.height) r.height = 1;
  } else {
    r.height = dh;
    r.width = (uint32_t)(((uint64_t)sw * dh + sh / 2) / sh);
    if (!r.width) r.width = 1;
  }
  r.x = (dw - r.width) / 2;
  r.y = (dh - r.height) / 2;
  return r;
}

// Source is immutable BGRA. Pixel-center nearest sampling avoids any temporary frame allocation.
static inline void pianoOutputScaleBGRA(const uint8_t *src, uint32_t sw, uint32_t sh, size_t ss,
                                        uint8_t *dst, uint32_t dw, uint32_t dh, size_t ds)
{
  const PianoOutputRect r = pianoOutputFit(sw, sh, dw, dh);
  if (!r.width || !r.height) return;
  if (sw == dw && sh == dh) {
    for (uint32_t y = 0; y < dh; ++y) memcpy(dst + y * ds, src + y * ss, (size_t)dw * 4);
    return;
  }
  for (uint32_t y = 0; y < dh; ++y) {
    uint8_t *row = dst + y * ds;
    for (uint32_t x = 0; x < dw; ++x) {
      row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
      row[x * 4 + 3] = 255;
    }
    if (y < r.y || y >= r.y + r.height) continue;
    const uint32_t sy = (uint32_t)(((uint64_t)(y - r.y) * 2 + 1) * sh / ((uint64_t)r.height * 2));
    for (uint32_t x = 0; x < r.width; ++x) {
      const uint32_t sx = (uint32_t)(((uint64_t)x * 2 + 1) * sw / ((uint64_t)r.width * 2));
      memcpy(row + (size_t)(r.x + x) * 4, src + sy * ss + (size_t)sx * 4, 4);
    }
  }
}
