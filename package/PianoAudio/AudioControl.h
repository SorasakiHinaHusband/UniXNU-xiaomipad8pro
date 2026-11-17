/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* IOAudio2 selector 2 carries integer raw steps, already converted by HAL
 * from the public scalar/dB properties. The UInt64 slot is not a float. */
#ifndef PIANO_AUDIO_CONTROL_H
#define PIANO_AUDIO_CONTROL_H
#include <stdint.h>

/* IOAudio2 uses the IOAudioFamily type/subtype pair: the base class is the
 * control type and class is its subtype.  In particular, a device volume is
 * levl/vlme, while the output mute is togl/mute. */
#define PIANO_AUDIO_CONTROL_BASE_LEVEL 0x6c65766cU   /* 'levl' */
#define PIANO_AUDIO_CONTROL_CLASS_VOLUME 0x766c6d65U /* 'vlme' */
#define PIANO_AUDIO_CONTROL_BASE_TOGGLE 0x746f676cU  /* 'togl' */
#define PIANO_AUDIO_CONTROL_CLASS_MUTE 0x6d757465U   /* 'mute' */
#define PIANO_AUDIO_CONTROL_SCOPE_OUTPUT 0x6f757470U /* 'outp' */
#define PIANO_AUDIO_CONTROL_SCOPE_INPUT 0x696e7074U  /* 'inpt' */
#define PIANO_AUDIO_CONTROL_ELEMENT_MAIN 0U
#define PIANO_AUDIO_VOLUME_MIN_DB (-36)

static inline int piano_audio_control_percent(uint64_t raw)
{
    return raw <= 100 ? (int)raw : -1;
}

static inline int piano_audio_control_bool(uint64_t raw)
{
    return raw <= 1 ? (int)raw : -1;
}

/* Shared by selector 2, publication and notifications. Each device owns its
 * state: the input never writes the speaker's transport control. */
static inline uint32_t piano_audio_control_scope(int input)
{ return input ? PIANO_AUDIO_CONTROL_SCOPE_INPUT : PIANO_AUDIO_CONTROL_SCOPE_OUTPUT; }
static inline const char *piano_audio_level_key(int input)
{ return input ? "piano-audio-input-level" : "piano-audio-volume"; }
static inline const char *piano_audio_mute_key(int input)
{ return input ? "piano-audio-input-mute" : "piano-audio-mute"; }
static inline int piano_audio_input_percent(uint64_t raw)
{
    if (raw <= 100) return raw == 1 ? 100 : (int)raw;
    if (raw >> 32) {
        union { uint64_t u; double d; } d = {raw};
        return d.d >= 0.0 && d.d <= 1.0 ? (int)(d.d * 100.0 + 0.5) : -1;
    }
    union { uint32_t u; float f; } f = {(uint32_t)raw};
    /* Integer garbage must not become a subnormal Float32 zero. */
    return (f.u & 0x7f800000U) && f.f >= 0.0f && f.f <= 1.0f ? (int)(f.f * 100.0f + 0.5f) : -1;
}
static inline int piano_audio_control_decode(int input, uint32_t id, uint64_t raw)
{
    if (id != 1 && id != 2) return -1;
    const int p = input ? piano_audio_input_percent(raw) : piano_audio_control_percent(raw);
    return id == 1 || p < 0 ? p : p != 0;
}

#endif
