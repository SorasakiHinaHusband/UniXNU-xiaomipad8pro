/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The two IOAudio2 timing profiles used by PianoAudio.
 *
 * The legacy profile is the known-good 5 ms engine. The lead profile keeps a
 * 300 ms producer lead in a separate zero-time ring; its HAL IO cycle is one
 * 10 ms period. The registry's `io buffer frame size` is the cycle, never the
 * mapped ring capacity.
 */
#ifndef PIANO_AUDIO_TIMING_H
#define PIANO_AUDIO_TIMING_H
#include <stdint.h>

#define PIANO_AUDIO_RATE 48000u
#define PIANO_AUDIO_LEGACY_PERIOD_FRAMES 240u
#define PIANO_AUDIO_LEGACY_RING_FRAMES (32u * PIANO_AUDIO_LEGACY_PERIOD_FRAMES)

#define PIANO_AUDIO_PERIOD_FRAMES 480u
#define PIANO_AUDIO_RING_FRAMES 65536u
#define PIANO_AUDIO_IN_FLIGHT 4u
#define PIANO_AUDIO_LEAD_FRAMES (30u * PIANO_AUDIO_PERIOD_FRAMES)
#define PIANO_AUDIO_DSP_TARGET_FRAMES (PIANO_AUDIO_LEAD_FRAMES - PIANO_AUDIO_IN_FLIGHT * PIANO_AUDIO_PERIOD_FRAMES)
#define PIANO_AUDIO_ZERO_TIME_PERIOD PIANO_AUDIO_RING_FRAMES
#define PIANO_AUDIO_ENGINE_LATENCY_FRAMES PIANO_AUDIO_PERIOD_FRAMES
#define PIANO_AUDIO_SAFETY_PERIODS 2u

#define PIANO_AUDIO_TIMING_LEGACY 0u
#define PIANO_AUDIO_TIMING_NEW 1u

static inline uint32_t piano_audio_ring_segment(uint32_t frame, uint32_t count, uint32_t ring_frames)
{
    const uint32_t at = frame % ring_frames;
    const uint32_t room = ring_frames - at;
    return count < room ? count : room;
}

static inline void piano_audio_copy_to_ring(int16_t *ring, uint32_t ring_frames, uint32_t frame,
                                            const int16_t *src, uint32_t count, uint32_t channels)
{
    uint32_t done = 0;
    while (done < count) {
        const uint32_t n = piano_audio_ring_segment(frame + done, count - done, ring_frames);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < channels; ++c)
                ring[((frame + done + i) % ring_frames) * channels + c] = src[(done + i) * channels + c];
        done += n;
    }
}

static inline void piano_audio_copy_from_ring(int16_t *dst, const int16_t *ring, uint32_t ring_frames, uint32_t frame,
                                              uint32_t count, uint32_t channels)
{
    uint32_t done = 0;
    while (done < count) {
        const uint32_t n = piano_audio_ring_segment(frame + done, count - done, ring_frames);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < channels; ++c)
                dst[(done + i) * channels + c] = ring[((frame + done + i) % ring_frames) * channels + c];
        done += n;
    }
}

static inline void piano_audio_zero_ring(int16_t *ring, uint32_t ring_frames, uint32_t frame,
                                         uint32_t count, uint32_t channels)
{
    uint32_t done = 0;
    while (done < count) {
        const uint32_t n = piano_audio_ring_segment(frame + done, count - done, ring_frames);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < channels; ++c)
                ring[((frame + done + i) % ring_frames) * channels + c] = 0;
        done += n;
    }
}

#endif
