// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-wifi-join: hands PianoWifi the network to join, once, from a root-only file (docs/drivers/wifi.md §13.2).
//
//   /usr/local/libexec/piano-wifi-join [FILE]      (FILE defaults to /private/var/db/piano-wifi/profile)
//
// Run as root at boot by the LaunchDaemon com.unixnu.piano-wifi-join. The file is the owner's and stays on the tablet: it must
// be a regular file owned by root with no group or other permission bits (like an ssh key), in a directory owned by root with
// none either; anything else is refused without reading it. Its lines, `key=value`, value to the end of the line:
//   ssid=NAME            the network's name, 1 to 32 bytes, as it is (no quoting)
//   security=open        or wpa2-psk
//   psk=PASSPHRASE       for wpa2-psk: 8 to 63 printable characters, or 64 hex digits
//   bssid=aa:bb:cc:dd:ee:ff   optional: the access point to prefer
//   mac=02:..:..:..:..:..     optional: the station's own address (locally administered)
// Profile bytes stay in memory. CoreWLAN owns association; the read-only kernel receipt
// proves that its SET20 published a new profile. This RunAtLoad job exits after one
// request and never overrides a later user disconnect.
#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>
#include <sys/time.h>
#include "BootJoin.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "../../../package/PianoWifi/WifiProfile.h"

static void wipe(void *p, size_t n)
{
  volatile unsigned char *at = (volatile unsigned char *)p;
  while (n--) *at++ = 0;
}

// The file's permissions, checked on the open descriptor and on its directory before a byte is read.
static int safe_open(const char *path)
{
  char dir[1024];
  const size_t len = strlen(path);
  if (len >= sizeof dir) return -1;
  memcpy(dir, path, len + 1);
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir) return -1;
  *slash = 0;
  struct stat ds;
  if (lstat(dir, &ds) != 0) return errno == ENOENT ? -2 : -1;
  if (!S_ISDIR(ds.st_mode) || ds.st_uid != 0 || (ds.st_mode & 077)) {
    fprintf(stderr, "piano-wifi-join: the profile's directory must be root's with mode 700; not read\n");
    return -1;
  }
  const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? -2 : -1;
  struct stat fs;
  if (fstat(fd, &fs) != 0 || !S_ISREG(fs.st_mode) || fs.st_uid != 0 || (fs.st_mode & 077) || fs.st_size > 4096) {
    fprintf(stderr, "piano-wifi-join: the profile must be a regular file of root's with mode 600, at most 4 KiB; not read\n");
    close(fd);
    return -1;
  }
  return fd;
}

struct apple_join {
  const struct piano_wifi_profile *profile;
  CWInterface *interface;
  CWNetwork *network;
};
static uint32_t receipt(void *unused)
{
  (void)unused;
  char line[256] = {}; size_t n = sizeof line;
  if (sysctlbyname(PIANO_WIFI_PROFILE_SYSCTL, line, &n, NULL, 0)) return 0;
  unsigned generation = 0;
  if (!strstr(line, "source apple-set20")) return 0;
  return sscanf(line, "set (generation %u)", &generation) == 1 ? generation : 0;
}
static int ready(void *context)
{
  struct apple_join *a = context;
  if (a->network) return 1;
  // Match the installed pre-G5 boot-settle boundary, after airportd startup cleanup.
  struct timeval boot = {}; size_t n = sizeof boot;
  if (sysctlbyname("kern.boottime", &boot, &n, NULL, 0) || time(NULL) - boot.tv_sec < 120) return 0;
  a->interface = [[CWWiFiClient sharedWiFiClient] interfaceWithName:@"en0"];
  if (!a->interface || !a->interface.powerOn) return 0;
  NSData *name = [NSData dataWithBytes:a->profile->ssid length:a->profile->ssid_len];
  NSError *error = nil;
  NSSet<CWNetwork *> *found = [a->interface scanForNetworksWithSSID:name error:&error];
  if (error || !found.count) return 0;
  for (CWNetwork *network in found) {
    if (![network.ssidData isEqualToData:name]) continue;
    if (a->profile->flags & PianoWifiProfileBssid) {
      char bssid[18]; const uint8_t *b = a->profile->bssid;
      snprintf(bssid, sizeof bssid, "%02x:%02x:%02x:%02x:%02x:%02x", b[0],b[1],b[2],b[3],b[4],b[5]);
      if ([network.bssid caseInsensitiveCompare:[NSString stringWithUTF8String:bssid]] != NSOrderedSame) continue;
    }
    a->network = network;
    return 1;
  }
  return 0;
}
static int associate(void *context)
{
  struct apple_join *a = context;
  const struct piano_wifi_profile *p = a->profile;
  NSString *password = p->psk_len ? [[NSString alloc] initWithBytes:p->psk length:p->psk_len encoding:NSUTF8StringEncoding] : nil;
  NSError *error = nil;
  puts("piano-wifi-join: Apple boot join invoked (credentials not logged)"); fflush(stdout);
  const BOOL ok = [a->interface associateToNetwork:a->network password:password error:&error];
  // Do not print NSError descriptions: Apple may include network identifiers.
  if (!ok) fprintf(stderr, "piano-wifi-join: Apple association failed (code %ld)\n", (long)error.code);
  return ok;
}
static void pause_join(void *unused) { (void)unused; sleep(2); }

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : PIANO_WIFI_PROFILE_PATH;
  if (geteuid() != 0) {
    fprintf(stderr, "piano-wifi-join: run as root\n");
    return 1;
  }
  // No PianoWifi in this boot (or not at a level that joins): nothing to do. launchd may start this before the kext has
  // registered its sysctl, so it is looked for during 30 s before that conclusion.
  size_t probe = 0;
  int tries = 30;
  while (sysctlbyname(PIANO_WIFI_PROFILE_SYSCTL, NULL, &probe, NULL, 0) != 0) {
    if (--tries <= 0) return 0;
    sleep(1);
  }
  const int fd = safe_open(path);
  if (fd == -2) return 0;   // no profile: nothing to join
  if (fd < 0) return 1;
  static char text[4097];
  const ssize_t got = read(fd, text, sizeof text - 1);
  close(fd);
  if (got <= 0) {
    wipe(text, sizeof text);
    return 1;
  }
  // The shared parser (WifiProfile.h): the kext's and this helper's rules are one, the supplicant's the same (a last line
  // without its newline is a file still being written: refused, review of 2026-11-14 R1/R2).
  struct piano_wifi_profile p;
  const char *problem = piano_wifi_profile_parse((const uint8_t *)text, (uint32_t)got, &p);
  wipe(text, sizeof text);
  if (problem) {
    fprintf(stderr, "piano-wifi-join: the profile is not usable: %s; nothing handed over\n", problem);
    piano_wifi_profile_wipe(&p);
    return 1;
  }
  // Bound both readiness and synchronous CoreWLAN/XPC calls. No retry after an
  // association (including an API failure or missing receipt).
  alarm(360);
  int rc;
  @autoreleasepool {
    struct apple_join a = { &p, nil, nil };
    struct piano_boot_join_transport transport = { &a, ready, receipt, associate, pause_join };
    rc = piano_boot_join(&transport, 120);
  }
  piano_wifi_profile_wipe(&p);
  if (rc) {
    fprintf(stderr, "piano-wifi-join: boot transaction failed stage %d; no retry\n", rc);
    return 1;
  }
  puts("PianoWifiBootAppleJoinV1: fresh SET20/profile receipt; one boot request complete");
  return 0;
}
