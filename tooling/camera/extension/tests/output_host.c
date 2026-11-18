// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "../CameraOutput.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static void geometry(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
  PianoOutputRect r = pianoOutputFit(sw, sh, dw, dh);
  assert(r.x == x && r.y == y && r.width == w && r.height == h);
  if (r.width > 1 && r.height > 1) {
    assert((uint64_t)r.width * sh <= (uint64_t)r.height * sw + sw / 2);
    assert((uint64_t)r.height * sw <= (uint64_t)r.width * sh + sh / 2);
  }
  const size_t ss = sw * 4 + 12, ds = dw * 4 + 20;
  uint8_t *src = malloc(ss * sh), *dst = malloc(ds * dh + 32);
  assert(src && dst);
  memset(src, 0xdd, ss * sh);
  memset(dst, 0xee, ds * dh + 32);
  for (uint32_t sy = 0; sy < sh; ++sy)
    for (uint32_t sx = 0; sx < sw; ++sx) {
      uint8_t *p = src + sy * ss + sx * 4;
      p[0] = (uint8_t)sx; p[1] = (uint8_t)sy; p[2] = 37; p[3] = 255;
    }
  pianoOutputScaleBGRA(src, sw, sh, ss, dst, dw, dh, ds);
  for (uint32_t dy = 0; dy < dh; ++dy) {
    for (uint32_t dx = 0; dx < dw; ++dx) {
      const uint8_t *p = dst + dy * ds + dx * 4;
      assert(p[3] == 255);
      if (dx < r.x || dx >= r.x + r.width || dy < r.y || dy >= r.y + r.height)
        assert(p[0] == 0 && p[1] == 0 && p[2] == 0);
      else {
        uint32_t sx = (uint32_t)(((uint64_t)(dx - r.x) * 2 + 1) * sw / ((uint64_t)w * 2));
        uint32_t sy = (uint32_t)(((uint64_t)(dy - r.y) * 2 + 1) * sh / ((uint64_t)h * 2));
        assert(p[0] == (uint8_t)sx && p[1] == (uint8_t)sy && p[2] == 37);
      }
    }
    for (size_t i = dw * 4; i < ds; ++i) assert(dst[dy * ds + i] == 0xee);
  }
  for (size_t i = 0; i < 32; ++i) assert(dst[ds * dh + i] == 0xee);
  free(src); free(dst);
}

static void cadence(void)
{
  // A real source every two 30 fps ticks: one unique and one repeated output per source.
  uint64_t source = 0, sequence = 0, published = 0, repeats = 0, lastSequence = 0;
  for (uint64_t tick = 0; tick < 30; ++tick) {
    uint64_t now = UINT64_C(1000000000) + tick * UINT64_C(1000000000) / 30;
    if (!(tick % 2)) { source = now; ++sequence; }
    if (pianoOutputSourceFresh(now, source)) {
      ++published;
      if (lastSequence == sequence) ++repeats;
      lastSequence = sequence;
    }
  }
  assert(published == 30 && sequence == 15 && repeats == 15);
  // No source means no frame. A stalled source expires after 250 ms, including across clock errors.
  assert(!pianoOutputSourceFresh(1, 0));
  assert(!pianoOutputSourceFresh(999, 1000));
  assert(pianoOutputSourceFresh(source + PIANO_OUTPUT_MAX_SOURCE_AGE_NS, source));
  assert(!pianoOutputSourceFresh(source + PIANO_OUTPUT_MAX_SOURCE_AGE_NS + 1, source));
}

int main(void)
{
  PianoOutputFormat native = pianoOutputFormat(816, 459, 0), compat = pianoOutputFormat(816, 459, 1);
  assert(native.width == 816 && native.height == 459 && native.minFps == 15 && native.maxFps == 15);
  assert(compat.width == 640 && compat.height == 480 && compat.minFps == 15 && compat.maxFps == 30);
  assert(pianoOutputDurationValid(0, 2, 30));
  assert(!pianoOutputDurationValid(0, 1, 30));
  assert(pianoOutputDurationValid(1, 1, 15) && pianoOutputDurationValid(1, 1, 24));
  assert(pianoOutputDurationValid(1, 1, 30) && pianoOutputDurationValid(1, 1001, 30000));
  assert(!pianoOutputDurationValid(1, 1, 31) && !pianoOutputDurationValid(1, 1, 14));
  assert(!pianoOutputDurationValid(1, INT64_MAX, INT32_MAX));
  assert(!pianoOutputDurationValid(1, -1, 30) && !pianoOutputDurationValid(1, 1, 0));
  assert(!pianoOutputDurationValid(2, 1, 15));
  geometry(816, 459, 640, 480, 0, 60, 640, 360);
  geometry(1020, 768, 640, 480, 1, 0, 638, 480);
  geometry(816, 459, 816, 459, 0, 0, 816, 459);
  geometry(1280, 720, 640, 480, 0, 60, 640, 360);
  geometry(1, 1000, 640, 480, 319, 0, 1, 480);
  cadence();
  puts("camera output geometry/durations/freshness/cadence PASS");
  return 0;
}
