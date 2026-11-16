// SPDX-License-Identifier: BSD-2-Clause-Patent
// V0b backend: a capability-only decoder inventory.  The transport entry points
// deliberately refuse sessions until the Iris V4L2/HFI backend is wired in.
#include <stdint.h>

constexpr int kNotSupported = -45;

extern "C" {

unsigned droidvm_media_devices(void) { return 1; }
uint32_t droidvm_media_caps(unsigned device) { return device == 0 ? 0x3u : 0; }
const char *droidvm_media_card(unsigned device) { return device == 0 ? "PianoVideo decoder" : ""; }
bool droidvm_media_is_up(unsigned device) { return device == 0; }
void droidvm_media_set_sink(unsigned, void (*)(void *, const uint8_t *, uint32_t), void *) {}
void droidvm_media_set_fast(unsigned, bool) {}
int droidvm_media_open(unsigned, uint32_t *) { return kNotSupported; }
int droidvm_media_close(unsigned, uint32_t) { return kNotSupported; }
int droidvm_media_ioctl(unsigned, uint32_t, uint32_t, void *, uint32_t, uint32_t) { return kNotSupported; }
int droidvm_media_mmap(unsigned, uint32_t, uint32_t, bool, uint64_t *, uint64_t *) { return kNotSupported; }
int droidvm_media_munmap(unsigned, uint64_t) { return kNotSupported; }

}
