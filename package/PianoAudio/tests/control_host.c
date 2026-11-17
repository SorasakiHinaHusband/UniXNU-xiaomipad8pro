// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Integer selector-2 values and an independent IOAudio2 receiver wire decoder.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../AudioControl.h"
#include "../AudioControlNotification.h"

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } ++checks; } while (0)

static uint32_t word(const unsigned char *b, size_t offset)
{
    return (uint32_t)b[offset] | (uint32_t)b[offset+1] << 8 |
           (uint32_t)b[offset+2] << 16 | (uint32_t)b[offset+3] << 24;
}

// Deliberately no sender struct/offsetof/constants: these are the observed
// receiver's offsets (count at B+0x1c, records at B+0x20 with stride 32).
static int receiver(const void *wire, size_t bytes, uint32_t id, int32_t value)
{
    const unsigned char *b = wire;
    if (bytes != 64 || word(b, 4) != bytes || word(b, 0x18) != 0 || word(b, 0x1c) != 1) return 0;
    const size_t entry = 0x20;
    if (word(b, entry) != id || word(b, entry+4) != 0x6376616cU ||
        (int32_t)word(b, entry+8) != value) return 0;
    for (size_t i = entry+12; i < bytes; ++i) if (b[i]) return 0;
    return 1;
}

// Replay the XNU kernel's 32-byte pointer-port header and its copyout
// contraction to a 24-byte user header. The body bytes come from the actual
// serializer above, not a second hand-written cval body.
static void kernel_copyout(const PianoAudio2NotificationMessage *actual, unsigned char wire[64])
{
    struct KernelMessage {
        uint32_t bits, size;
        uint64_t remote, local;
        uint32_t voucher, id;
        unsigned char body[40];
    } kernel;
    _Static_assert(sizeof kernel == 72, "kernel message replay size");
    _Static_assert(offsetof(struct KernelMessage, body) == 32, "kernel body replay offset");
    memset(&kernel, 0, sizeof kernel);
    kernel.bits = actual->header.msgh_bits;
    kernel.size = sizeof kernel;
    kernel.remote = actual->header.msgh_remote_port;
    memcpy(kernel.body, (const unsigned char *)actual + 24, 40);
    const uint32_t user_header[6] = {
        kernel.bits, kernel.size - 8, (uint32_t)kernel.remote,
        (uint32_t)kernel.local, kernel.voucher, kernel.id
    };
    memcpy(wire, user_header, 24);
    memcpy(wire + 24, kernel.body, 40);
}

int main(void)
{
    CHECK(PIANO_AUDIO_CONTROL_BASE_LEVEL == 0x6c65766cU);
    CHECK(PIANO_AUDIO_CONTROL_CLASS_VOLUME == 0x766c6d65U);
    CHECK(PIANO_AUDIO_CONTROL_BASE_TOGGLE == 0x746f676cU);
    CHECK(PIANO_AUDIO_CONTROL_CLASS_MUTE == 0x6d757465U);
    CHECK(PIANO_AUDIO_CONTROL_SCOPE_OUTPUT == 0x6f757470U && PIANO_AUDIO_CONTROL_ELEMENT_MAIN == 0U);
    CHECK(PIANO_AUDIO_VOLUME_MIN_DB == -36);
    for (uint64_t raw = 0; raw <= 100; ++raw) {
        const int applied = piano_audio_control_percent(raw);
        CHECK(applied == (int)raw);
        PianoAudio2NotificationMessage message;
        memset(&message, 0xa5, sizeof message);
        piano_audio_control_notification(&message, 123, 1, (uint32_t)applied);
        CHECK(receiver(&message, sizeof message, 1, (int32_t)raw));
        unsigned char wire[64];
        kernel_copyout(&message, wire);
        CHECK(receiver(wire, sizeof wire, 1, (int32_t)raw));
        CHECK(memcmp(wire, &message, sizeof wire) == 0);
        const unsigned char *b = (const unsigned char *)&message;
        CHECK(word(b, 0) == 19 && word(b, 8) == 123);
        CHECK(word(b, 12) == 0 && word(b, 16) == 0 && word(b, 20) == 0);
        CHECK(!receiver(&message, sizeof message - 1, 1, (int32_t)raw));
    }
    const uint64_t bad[] = {101, 102, UINT32_MAX, 0x100000001ULL, UINT64_MAX,
                           0x3e800000, 0x3f800000, 0x3fe3333333333333ULL};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; ++i) {
        CHECK(piano_audio_control_percent(bad[i]) == -1);
        CHECK(piano_audio_control_bool(bad[i]) == -1);
    }
    CHECK(piano_audio_control_bool(2) == -1);
    for (uint64_t raw = 0; raw <= 1; ++raw) {
        const int applied = piano_audio_control_bool(raw);
        CHECK(applied == (int)raw);
        PianoAudio2NotificationMessage message;
        memset(&message, 0xa5, sizeof message);
        piano_audio_control_notification(&message, 123, 2, (uint32_t)applied);
        CHECK(receiver(&message, sizeof message, 2, (int32_t)raw));
        unsigned char wire[64];
        kernel_copyout(&message, wire);
        CHECK(receiver(wire, sizeof wire, 2, (int32_t)raw));
    }
    // Old property-address sender: 56 bytes, cval where count belongs.
    uint32_t old[14] = {19, 56, 123, 0, 0, 0, 0, 0x6376616c, 0x6f757470, 0, 1};
    CHECK(!receiver(old, sizeof old, 1, 50));
    uint32_t corrupt[16] = {19, 64, 123, 0, 0, 0, 0, 1, 1, 0x6376616c, 50};
    CHECK(receiver(corrupt, sizeof corrupt, 1, 50));
    corrupt[7] = 0x6376616c;
    CHECK(!receiver(corrupt, sizeof corrupt, 1, 50));
    corrupt[7] = 1; corrupt[10] = 0x6f757470;
    CHECK(!receiver(corrupt, sizeof corrupt, 1, 50));
    corrupt[10] = 50; corrupt[15] = 1;
    CHECK(!receiver(corrupt, sizeof corrupt, 1, 50));
    printf("audio control host: %u checks (integer raw + independent counted-cval receiver)\n", checks);
    return 0;
}
