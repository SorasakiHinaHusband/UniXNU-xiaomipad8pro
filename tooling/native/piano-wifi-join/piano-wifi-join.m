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
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "../../../package/PianoWifi/WifiProfile.h"

static void timeline(const char *format, ...)
{
  char event[160];
  va_list args;
  va_start(args, format);
  vsnprintf(event, sizeof event, format, args);
  va_end(args);
  const int saved_errno = errno;
  fprintf(stdout, "PianoWifiBootJoinTimelineV1: time=%lld pid=%d %s\n",
          (long long)time(NULL), getpid(), event);
  fflush(stdout);
  errno = saved_errno;
}

static int finish(int status, const char *stage)
{
  timeline("exit status=%d stage=%s", status, stage);
  return status;
}

static void alarm_expired(int signo)
{
  static const char line[] = "PianoWifiBootJoinTimelineV1: exit signal=SIGALRM\n";
  (void)write(STDERR_FILENO, line, sizeof line - 1);
  // SA_RESETHAND restores the default disposition. Preserve the signal observed by launchd.
  (void)kill(getpid(), signo);
}

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
  unsigned readiness_attempt;
};
static uint32_t receipt(void *unused)
{
  (void)unused;
  char line[256] = {}; size_t n = sizeof line;
  if (sysctlbyname(PIANO_WIFI_PROFILE_SYSCTL, line, &n, NULL, 0)) {
    timeline("SET20 poll result=read-error errno=%d", errno);
    return 0;
  }
  unsigned generation = 0;
  if (!strstr(line, "source apple-set20")) {
    timeline("SET20 poll result=no-apple-generation");
    return 0;
  }
  if (sscanf(line, "set (generation %u)", &generation) != 1) generation = 0;
  timeline("SET20 poll generation=%u", generation);
  return generation;
}
static int ready(void *context)
{
  struct apple_join *a = context;
  const unsigned attempt = ++a->readiness_attempt;
  if (a->network) {
    timeline("ready attempt=%u result=cached-match", attempt);
    return 1;
  }
  // Match the installed pre-G5 boot-settle boundary, after airportd startup cleanup.
  struct timeval boot = {}; size_t n = sizeof boot;
  if (sysctlbyname("kern.boottime", &boot, &n, NULL, 0)) {
    timeline("settle attempt=%u result=read-error errno=%d", attempt, errno);
    return 0;
  }
  const time_t uptime = time(NULL) - boot.tv_sec;
  timeline("settle attempt=%u uptime=%lld ready=%d", attempt, (long long)uptime, uptime >= 120);
  if (uptime < 120) return 0;
  timeline("interface attempt=%u begin", attempt);
  a->interface = [[CWWiFiClient sharedWiFiClient] interfaceWithName:@"en0"];
  timeline("interface attempt=%u present=%d", attempt, a->interface != nil);
  if (!a->interface) return 0;
  timeline("power attempt=%u begin", attempt);
  const BOOL powered = a->interface.powerOn;
  timeline("power attempt=%u on=%d", attempt, powered);
  if (!powered) return 0;
  NSData *name = [NSData dataWithBytes:a->profile->ssid length:a->profile->ssid_len];
  NSError *error = nil;
  timeline("scan attempt=%u begin", attempt);
  NSSet<CWNetwork *> *found = [a->interface scanForNetworksWithSSID:name error:&error];
  const NSUInteger count = error ? 0 : found.count;
  timeline("scan attempt=%u has_error=%d code=%ld candidates=%lu", attempt, error != nil,
           (long)error.code, (unsigned long)count);
  if (error || !count) return 0;
  for (CWNetwork *network in found) {
    if (![network.ssidData isEqualToData:name]) continue;
    if (a->profile->flags & PianoWifiProfileBssid) {
      char bssid[18]; const uint8_t *b = a->profile->bssid;
      snprintf(bssid, sizeof bssid, "%02x:%02x:%02x:%02x:%02x:%02x", b[0],b[1],b[2],b[3],b[4],b[5]);
      if ([network.bssid caseInsensitiveCompare:[NSString stringWithUTF8String:bssid]] != NSOrderedSame) continue;
    }
    a->network = network;
    timeline("scan attempt=%u match=1", attempt);
    return 1;
  }
  timeline("scan attempt=%u match=0", attempt);
  return 0;
}
static int associate(void *context)
{
  struct apple_join *a = context;
  const struct piano_wifi_profile *p = a->profile;
  NSString *password = p->psk_len ? [[NSString alloc] initWithBytes:p->psk length:p->psk_len encoding:NSUTF8StringEncoding] : nil;
  NSError *error = nil;
  puts("piano-wifi-join: Apple boot join invoked (credentials not logged)"); fflush(stdout);
  timeline("associate begin");
  const BOOL ok = [a->interface associateToNetwork:a->network password:password error:&error];
  timeline("associate returned ok=%d has_error=%d code=%ld", ok, error != nil, (long)error.code);
  // Do not print NSError descriptions: Apple may include network identifiers.
  if (!ok) fprintf(stderr, "piano-wifi-join: Apple association failed (code %ld)\n", (long)error.code);
  return ok;
}
static void pause_join(void *unused) { (void)unused; sleep(2); }

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : PIANO_WIFI_PROFILE_PATH;
  timeline("start");
  if (geteuid() != 0) {
    fprintf(stderr, "piano-wifi-join: run as root\n");
    return finish(1, "root");
  }
  // No PianoWifi in this boot (or not at a level that joins): nothing to do. launchd may start this before the kext has
  // registered its sysctl, so it is looked for during 30 s before that conclusion.
  size_t probe = 0;
  for (int attempt = 1; ; ++attempt) {
    const int result = sysctlbyname(PIANO_WIFI_PROFILE_SYSCTL, NULL, &probe, NULL, 0);
    timeline("OID attempt=%d result=%d errno=%d", attempt, result, result ? errno : 0);
    if (!result) break;
    if (attempt == 30) return finish(0, "OID");
    sleep(1);
  }
  const int fd = safe_open(path);
  timeline("profile open=%s", fd == -2 ? "missing" : fd < 0 ? "refused" : "ok");
  if (fd == -2) return finish(0, "profile-missing");
  if (fd < 0) return finish(1, "profile-open");
  static char text[4097];
  const ssize_t got = read(fd, text, sizeof text - 1);
  timeline("profile read=%s errno=%d", got > 0 ? "ok" : got == 0 ? "empty" : "error", got < 0 ? errno : 0);
  close(fd);
  if (got <= 0) {
    wipe(text, sizeof text);
    return finish(1, "profile-read");
  }
  // The shared parser (WifiProfile.h): the kext's and this helper's rules are one, the supplicant's the same (a last line
  // without its newline is a file still being written: refused, review of 2026-11-14 R1/R2).
  struct piano_wifi_profile p;
  const char *problem = piano_wifi_profile_parse((const uint8_t *)text, (uint32_t)got, &p);
  timeline("profile parse=%s", problem ? "refused" : "ok");
  wipe(text, sizeof text);
  if (problem) {
    fprintf(stderr, "piano-wifi-join: the profile is not usable: %s; nothing handed over\n", problem);
    piano_wifi_profile_wipe(&p);
    return finish(1, "profile-parse");
  }
  // Bound both readiness and synchronous CoreWLAN/XPC calls. No retry after an
  // association (including an API failure or missing receipt).
  struct sigaction action = {};
  action.sa_handler = alarm_expired;
  action.sa_flags = SA_RESETHAND;
  sigemptyset(&action.sa_mask);
  const int handler = sigaction(SIGALRM, &action, NULL);
  alarm(360);
  timeline("readiness deadline_seconds=360 alarm_logging=%d", handler == 0);
  int rc;
  @autoreleasepool {
    struct apple_join a = { &p, nil, nil, 0 };
    struct piano_boot_join_transport transport = { &a, ready, receipt, associate, pause_join };
    rc = piano_boot_join(&transport, 120);
  }
  piano_wifi_profile_wipe(&p);
  if (rc) {
    fprintf(stderr, "piano-wifi-join: boot transaction failed stage %d; no retry\n", rc);
    static const char *const stage[] = { "complete", "readiness", "associate", "SET20" };
    return finish(1, stage[rc]);
  }
  puts("PianoWifiBootAppleJoinV1: fresh SET20/profile receipt; one boot request complete");
  return finish(0, "complete");
}
