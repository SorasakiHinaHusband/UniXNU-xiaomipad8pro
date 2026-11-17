// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Private kernel boundary, not a user ABI. The descriptor and WC view name the
// same V4 arena ledger entry; neither the CPU pointer nor the IOVA is a PA.
#pragma once
#include <stdint.h>
class IOMemoryDescriptor;
namespace pianovideo { namespace native {
struct Layout {
  uint32_t width = 320, height = 240, stride = 384, scanlines = 256;
  uint32_t yOffset = 0, uvOffset = 98304, bytes = 147456;
  uint32_t format = 0x34323076, bitdepth = 8; // 420v, linear NV12
};
struct Frame {
  IOMemoryDescriptor *memory = nullptr; // retained until frameRelease
  const volatile uint8_t *cpu = nullptr; // borrowed WC mapping, valid until release
  uint32_t length = 0, iova = 0;
  uint64_t token = 0;
  Layout layout;
};
} }
extern "C" {
bool piano_video_native_ready(void);
// Reserve the exclusive core without powering it. Native session IDs are fresh.
int piano_video_native_open(uint32_t *session);
// Worker-only, synchronous bounded operations; selector3 must never invoke these.
// decode copies and ETBs the supplied normalized Annex-B AU with the caller token.
int piano_video_native_decode(uint32_t session, const uint8_t *bytes, uint32_t size, uint64_t token,
                              pianovideo::native::Frame *frame);
void piano_video_native_frame_release(uint32_t session, pianovideo::native::Frame *frame);
// Terminal: ACK+LAST required, no restart. decode may already have sent DRAIN to
// release a delayed first frame; finish validates/completes that same transaction.
int piano_video_native_finish(uint32_t session);
// Always attempts STOP/CLOSE and PAS/routes/TLB teardown. On unconfirmed quiesce
// all DMA and dependencies remain retained and subsequent opens are refused.
int piano_video_native_close(uint32_t session);
}
