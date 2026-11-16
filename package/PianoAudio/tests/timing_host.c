/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host checks for the runtime timing switch's two profiles and the lead-ring
 * wrap. The device's registry contract is checked here as frame relations:
 * the IO cycle is one period, while the zero-time ring is larger. */
#include <stdint.h>
#include <stdio.h>
#include "../AudioTiming.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "timing_host:%d: %s\n", __LINE__, #x); return 1; } ++checks; } while (0)

static int16_t ring[PIANO_AUDIO_RING_FRAMES * 2u];
static int16_t src[PIANO_AUDIO_PERIOD_FRAMES * 2u];
static int16_t dst[PIANO_AUDIO_PERIOD_FRAMES * 2u];

int main(void)
{
    CHECK(PIANO_AUDIO_TIMING_LEGACY == 0u && PIANO_AUDIO_TIMING_NEW == 1u);
    CHECK(PIANO_AUDIO_LEGACY_PERIOD_FRAMES == 240u);
    CHECK(PIANO_AUDIO_LEGACY_RING_FRAMES == 7680u);
    CHECK(PIANO_AUDIO_PERIOD_FRAMES == 480u);
    CHECK(PIANO_AUDIO_RING_FRAMES == 65536u);
    CHECK(PIANO_AUDIO_LEAD_FRAMES == 14400u);
    CHECK(PIANO_AUDIO_DSP_TARGET_FRAMES == 12480u);
    CHECK(PIANO_AUDIO_ZERO_TIME_PERIOD == PIANO_AUDIO_RING_FRAMES);
    CHECK(PIANO_AUDIO_ZERO_TIME_PERIOD >
          PIANO_AUDIO_LEAD_FRAMES + (PIANO_AUDIO_SAFETY_PERIODS + 1u) * PIANO_AUDIO_PERIOD_FRAMES);
    CHECK(PIANO_AUDIO_LEGACY_RING_FRAMES >
          2u * PIANO_AUDIO_LEGACY_PERIOD_FRAMES + PIANO_AUDIO_LEGACY_PERIOD_FRAMES);
    CHECK(piano_audio_ring_segment(PIANO_AUDIO_RING_FRAMES - 200u, PIANO_AUDIO_PERIOD_FRAMES,
                                   PIANO_AUDIO_RING_FRAMES) == 200u);

    for (uint32_t i = 0; i < PIANO_AUDIO_PERIOD_FRAMES * 2u; ++i) src[i] = (int16_t)(i * 3u - 700);
    piano_audio_copy_to_ring(ring, PIANO_AUDIO_RING_FRAMES, PIANO_AUDIO_RING_FRAMES - 200u,
                             src, PIANO_AUDIO_PERIOD_FRAMES, 2u);
    piano_audio_copy_from_ring(dst, ring, PIANO_AUDIO_RING_FRAMES, PIANO_AUDIO_RING_FRAMES - 200u,
                               PIANO_AUDIO_PERIOD_FRAMES, 2u);
    for (uint32_t i = 0; i < PIANO_AUDIO_PERIOD_FRAMES * 2u; ++i) CHECK(dst[i] == src[i]);
    piano_audio_zero_ring(ring, PIANO_AUDIO_RING_FRAMES, PIANO_AUDIO_RING_FRAMES - 200u,
                          PIANO_AUDIO_PERIOD_FRAMES, 2u);
    piano_audio_copy_from_ring(dst, ring, PIANO_AUDIO_RING_FRAMES, PIANO_AUDIO_RING_FRAMES - 200u,
                               PIANO_AUDIO_PERIOD_FRAMES, 2u);
    for (uint32_t i = 0; i < PIANO_AUDIO_PERIOD_FRAMES * 2u; ++i) CHECK(dst[i] == 0);

    printf("PianoAudio timing host: %u checks, legacy 240/7680 and lead 480/65536\n", checks);
    return 0;
}
