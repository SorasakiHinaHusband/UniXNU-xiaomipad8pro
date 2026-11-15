// SPDX-License-Identifier: BSD-2-Clause-Patent
// The small, private kernel boundary used by PianoWifi's apple80211 front end.
// It carries only the station's already-established association state; SSID and
// addresses are copied into the request answer and never logged or persisted.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_WIFI_ASSOC_SNAPSHOT_VERSION 1u
#define PIANO_WIFI_ASSOC_SSID_MAX 32u

struct piano_wifi_assoc_snapshot {
  uint32_t version;
  uint8_t power;
  uint8_t associated;
  uint8_t authorized;
  uint8_t state;       // Apple80211 STATE: 0 INIT, 3 ASSOC, 4 RUN
  uint8_t ssid_length;
  uint8_t ssid[PIANO_WIFI_ASSOC_SSID_MAX];
  uint8_t bssid[6];
  uint16_t channel;
  uint32_t frequency_mhz;
  int32_t rssi_dbm;
  int32_t noise_dbm;
  uint32_t rate_mbps;
  uint8_t auth_type;  // 1=open, 2=WPA2-PSK
  uint8_t cipher;     // 0=open, 4=CCMP-128
  uint8_t op_mode;    // 1=station
  uint8_t reserved;
  uint32_t associate_result;
};

// Returns 0 and fills a version-1 snapshot.  The function is safe before the
// station starts and reports power off/idle in that case.
int piano_wifi_assoc_snapshot(struct piano_wifi_assoc_snapshot *out);

#ifdef __cplusplus
}
#endif
