// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Called only by the PM worker. Hardware work stays on the live owner. */
int piano_audio_power_request(int sleep);
void piano_audio_power_ready(void);
/* Park/reanchor existing HAL readers without changing client running intent,
 * UIDs, controls, mappings or services. Bounded and safe with StopIO/StartIO. */
int piano_audio_devices_power(int sleep);
void piano_audio_producer_trace(uint32_t frames, uint32_t late_us, uint64_t sample, uint64_t host, uint64_t write_end);
#ifdef __cplusplus
}
#endif
