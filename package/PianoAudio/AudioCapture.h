/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang. */
#ifndef PIANO_AUDIO_CAPTURE_H
#define PIANO_AUDIO_CAPTURE_H
#include <stdint.h>

/* Input raw 0..100 follows the published linear -36..0 dB map. Mute is
 * separate; output keeps its proven curve and safety chain. */
static inline int32_t piano_audio_input_q15(int percent)
{
    static const int16_t gain[101] = {
        519, 541, 564, 588, 613, 639, 666, 694, 723, 754, 786, 819, 854, 890, 928,
        967, 1008, 1051, 1095, 1141, 1190, 1240, 1293, 1347, 1404, 1464, 1526, 1590, 1657, 1728,
        1801, 1877, 1956, 2039, 2125, 2215, 2309, 2407, 2509, 2615, 2725, 2841, 2961, 3086, 3217,
        3353, 3495, 3643, 3797, 3958, 4125, 4300, 4482, 4671, 4869, 5075, 5290, 5514, 5747, 5990,
        6244, 6508, 6783, 7070, 7369, 7681, 8006, 8345, 8698, 9066, 9450, 9850, 10267, 10701, 11154,
        11626, 12118, 12631, 13165, 13723, 14303, 14909, 15539, 16197, 16882, 17597, 18342, 19118, 19927, 20770,
        21649, 22565, 23520, 24515, 25553, 26634, 27761, 28936, 30160, 31437, 32767
    };
    return percent < 0 || percent > 100 ? -1 : gain[percent];
}

typedef struct {
    uint64_t frames, nonzero, squares, clipped;
    int64_t sum;
    uint32_t peak;
} AudioCapturePcm;
static inline void audio_capture_sample(AudioCapturePcm *s, int16_t v)
{
    const int32_t a = v < 0 ? -(int32_t)v : v;
    ++s->frames;
    s->nonzero += v != 0;
    s->sum += v;
    s->squares += (uint64_t)((int64_t)v * v);
    s->clipped += v == -32768 || v == 32767;
    if ((uint32_t)a > s->peak) s->peak = (uint32_t)a;
}

typedef struct {
    uint64_t requested, delivered, startup, missing, muted, tail;
    uint64_t first_delivery; /* HAL frame coordinate + 1, zero means absent */
    AudioCapturePcm pcm;    /* after digital level/mute, before HAL/codec */
    uint64_t mute_nonzero;
} AudioCaptureHal;
#endif
