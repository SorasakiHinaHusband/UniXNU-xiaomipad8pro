// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the IOAudio2 selector-2 scalar representations.
#include <stdint.h>
#include <stdio.h>
#include "../AudioControl.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } ++checks; } while (0)

int main(void)
{
    union { uint32_t u; float f; } f;
    union { uint64_t u; double d; } d;
    f.f = 0.25f; d.d = 0.60;
    CHECK(PIANO_AUDIO_CONTROL_BASE_LEVEL == 0x6c65766cU);
    CHECK(PIANO_AUDIO_CONTROL_CLASS_VOLUME == 0x766c6d65U);
    CHECK(PIANO_AUDIO_CONTROL_BASE_TOGGLE == 0x746f676cU);
    CHECK(PIANO_AUDIO_CONTROL_CLASS_MUTE == 0x6d757465U);
    CHECK(PIANO_AUDIO_CONTROL_SCOPE_OUTPUT == 0x6f757470U && PIANO_AUDIO_CONTROL_ELEMENT_MAIN == 0U);
    CHECK(PIANO_AUDIO_VOLUME_MIN_DB == -36);
    CHECK(piano_audio_control_percent(0) == 0);
    CHECK(piano_audio_control_percent(1) == 100);
    CHECK(piano_audio_control_percent(25) == 25);
    CHECK(piano_audio_control_percent(100) == 100);
    CHECK(piano_audio_control_percent(f.u) == 25);
    CHECK(piano_audio_control_percent(d.u) == 60);
    CHECK(piano_audio_control_percent((uint64_t)25) == 25);
    CHECK(piano_audio_control_percent((uint64_t)60) == 60);
    CHECK(piano_audio_control_percent((uint64_t)100) == 100);
    CHECK(piano_audio_control_bool(0) == 0 && piano_audio_control_bool(1) == 1);
    CHECK(piano_audio_control_bool(f.u) == 1 && piano_audio_control_bool(0xdeadbeefULL) == -1);
    printf("audio control host: %u checks\n", checks);
    return 0;
}
