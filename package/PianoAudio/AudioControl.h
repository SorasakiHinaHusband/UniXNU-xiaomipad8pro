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

#endif
