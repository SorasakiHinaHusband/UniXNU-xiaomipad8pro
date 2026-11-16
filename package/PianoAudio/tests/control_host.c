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
    CHECK(piano_audio_control_percent(0) == 0);
    CHECK(piano_audio_control_percent(1) == 100);
    CHECK(piano_audio_control_percent(25) == 25);
    CHECK(piano_audio_control_percent(100) == 100);
    CHECK(piano_audio_control_percent(f.u) == 25);
    CHECK(piano_audio_control_percent(d.u) == 60);
    CHECK(piano_audio_control_bool(0) == 0 && piano_audio_control_bool(1) == 1);
    CHECK(piano_audio_control_bool(f.u) == 1 && piano_audio_control_bool(0xdeadbeefULL) == -1);
    printf("audio control host: %u checks\n", checks);
    return 0;
}
