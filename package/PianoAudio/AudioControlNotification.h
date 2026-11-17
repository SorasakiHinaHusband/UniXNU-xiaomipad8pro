/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang. */
#ifndef PIANO_AUDIO_CONTROL_NOTIFICATION_H
#define PIANO_AUDIO_CONTROL_NOTIFICATION_H
#include <mach/message.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* IOAudio2 type-0 port: counted 32-byte records, not property addresses.
 * For cval the union payload starts with the applied SInt32 raw value. */
typedef struct {
    uint32_t controlID;
    uint32_t event;
    int32_t value;
    uint32_t reserved[5];
} PianoAudio2ControlNotification;

typedef struct {
    mach_msg_header_t header;
    uint32_t reserved;
    uint32_t count;
    PianoAudio2ControlNotification entry;
} PianoAudio2NotificationMessage;

#ifdef __cplusplus
#define PIANO_AUDIO_ABI_ASSERT static_assert
#else
#define PIANO_AUDIO_ABI_ASSERT _Static_assert
#endif
/* XNU's internal ports are pointers: its 32-byte header is deflated to the
 * 24-byte user header on copyout. Keep the 40-byte body intact in both ABIs. */
#ifdef KERNEL
PIANO_AUDIO_ABI_ASSERT(sizeof(mach_msg_header_t) == 32, "kernel Mach header ABI");
#else
PIANO_AUDIO_ABI_ASSERT(sizeof(mach_msg_header_t) == 24, "user Mach header ABI");
#endif
PIANO_AUDIO_ABI_ASSERT(sizeof(PianoAudio2ControlNotification) == 32, "IOAudio2 record ABI");
PIANO_AUDIO_ABI_ASSERT(sizeof(PianoAudio2NotificationMessage) == sizeof(mach_msg_header_t) + 40, "IOAudio2 native message ABI");
PIANO_AUDIO_ABI_ASSERT(offsetof(PianoAudio2NotificationMessage, count) == sizeof(mach_msg_header_t) + 4, "IOAudio2 count offset");
PIANO_AUDIO_ABI_ASSERT(offsetof(PianoAudio2NotificationMessage, entry) == sizeof(mach_msg_header_t) + 8, "IOAudio2 entries offset");
PIANO_AUDIO_ABI_ASSERT(offsetof(PianoAudio2ControlNotification, controlID) == 0, "IOAudio2 UCID offset");
PIANO_AUDIO_ABI_ASSERT(offsetof(PianoAudio2ControlNotification, event) == 4, "IOAudio2 event offset");
PIANO_AUDIO_ABI_ASSERT(offsetof(PianoAudio2ControlNotification, value) == 8, "IOAudio2 value offset");
PIANO_AUDIO_ABI_ASSERT(sizeof(PianoAudio2NotificationMessage) - sizeof(mach_msg_header_t) + 24 == 64,
                       "IOAudio2 user wire message ABI");
#undef PIANO_AUDIO_ABI_ASSERT

static inline void piano_audio_control_notification(PianoAudio2NotificationMessage *message,
                                                    mach_port_t port, uint32_t controlID, uint32_t applied)
{
    memset(message, 0, sizeof *message);
    message->header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    message->header.msgh_size = sizeof *message;
    message->header.msgh_remote_port = port;
    message->count = 1;
    message->entry.controlID = controlID;
    message->entry.event = 0x6376616cU; /* cval */
    message->entry.value = (int32_t)applied;
}
#endif
