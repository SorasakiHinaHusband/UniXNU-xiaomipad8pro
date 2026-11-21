// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// piano-brightness-sync: CoreBrightness follows PianoBacklight's automatic level, so the Control Center / menu-bar brightness
// slider shows the panel's real brightness (2026-11-21, SLIDER-INIT.md; docs/drivers/backlight.md).
//
// Why: corebrightnessd takes its level from its BrightnessLevelNits preference at start (-[CBDisplayModuleSKL initialiseSDR]:
// loadNitsFromCapabilitiesTo:, then loadNitsFromPreferencesTo:) and changes it only by its own commits; nothing on macOS 27.3 reads
// a built-in panel's level back. PianoBacklight's automatic mode moves the panel in the kernel, so the slider showed CoreBrightness's
// stale value (full, while the panel was at 2-26 nits) until the user touched it.
//
// What: while PianoBacklight's automatic mode owns the lit panel and its level (IODisplayParameters BrightnessMilliNits) has settled,
// a CoreBrightness level (DisplayServicesClient DisplayNitsKey) more than 3 % away is replaced by the panel's: first announced to
// PianoBacklight (root's "piano-backlight-sync", 16.16 nits: WindowServer's commit that follows is CoreBrightness taking the panel's
// level, never the user's move), then set as DisplayNitsKey (CoreBrightness: getUserBrightnessForNits, updateUserBrightness:commit:
// YES; the slider is notified and the preference stored). The user's own level (a move ends automatic mode) and a dark panel are
// never touched. Woken by PianoBacklight's property-change messages, plus one check every 30 s; no polling loop.
//
//   piano-brightness-sync --run     the LaunchDaemon (root); exits 0 at once without piano-backlight-native=1 and piano-backlight-auto=1
//   piano-brightness-sync --check   read only: the panel's level and flags, CoreBrightness's level, the decision
// Status: /private/var/db/piano-userland/brightness-sync/status (syncs, taken, failed, the last values).
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <math.h>
#include <os/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "SyncPolicy.h"

@protocol PianoDisplayServicesClient
- (id)copyPropertyForKey:(NSString *)key;
- (BOOL)setProperty:(id)value withKey:(NSString *)key;
@end

static os_log_t gLog;
static io_service_t gBacklight;
static io_object_t gInterest;
static IONotificationPortRef gPort;
static CFRunLoopTimerRef gTimer;
static id<PianoDisplayServicesClient> gClient;
static struct pbs_state gState;
static unsigned gSyncs, gTaken, gFailed;
static uint32_t gLastSynced;
static NSString *const kStatusDir = @"/private/var/db/piano-userland/brightness-sync";

static uint64_t nowMs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }

static int bootArg(const char *word)
{
  size_t size = 0;
  if (sysctlbyname("kern.bootargs", NULL, &size, NULL, 0) || !size) return 0;
  char *args = calloc(1, size + 1);
  int found = 0;
  if (args && !sysctlbyname("kern.bootargs", args, &size, NULL, 0)) {
    const size_t n = strlen(word);
    for (char *p = args; (p = strstr(p, word)); p += n)
      if ((p == args || p[-1] == ' ') && (p[n] == ' ' || !p[n])) found = 1;
  }
  free(args);
  return found;
}

static id<PianoDisplayServicesClient> client(void)
{
  if (gClient) return gClient;
  NSBundle *bundle = [NSBundle bundleWithPath:@"/System/Library/PrivateFrameworks/CoreBrightness.framework"];
  Class cls = [bundle load] ? NSClassFromString(@"DisplayServicesClient") : Nil;
  id candidate = cls ? [[cls alloc] init] : nil;
  if ([candidate respondsToSelector:@selector(copyPropertyForKey:)] && [candidate respondsToSelector:@selector(setProperty:withKey:)])
    gClient = candidate;
  return gClient;
}

// CoreBrightness's level in millinits, -1 when it does not answer (the client is made again next time).
static int64_t coreBrightness(void)
{
  id value = [client() copyPropertyForKey:@"DisplayNitsKey"];
  if (![value isKindOfClass:NSNumber.class] || !isfinite([value doubleValue]) || [value doubleValue] < 0) {
    gClient = nil;
    return -1;
  }
  return llround([value doubleValue] * 1000.0);
}

static uint32_t panelMilliNits(io_service_t s)
{
  NSDictionary *params = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("IODisplayParameters"), kCFAllocatorDefault, 0));
  NSDictionary *nits = [params isKindOfClass:NSDictionary.class] ? params[@"BrightnessMilliNits"] : nil;
  NSNumber *value = [nits isKindOfClass:NSDictionary.class] ? nits[@"value"] : nil;
  return [value isKindOfClass:NSNumber.class] ? value.unsignedIntValue : 0;
}

static int flag(io_service_t s, CFStringRef key)
{
  CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
  const int on = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)v);
  if (v) CFRelease(v);
  return on;
}

static void writeStatus(uint32_t panel, int64_t cb, const char *result)
{
  mkdir("/private/var/db/piano-userland", 0755);
  mkdir(kStatusDir.fileSystemRepresentation, 0755);
  NSString *text = [NSString stringWithFormat:@"syncs=%u taken=%u failed=%u last_panel_millinits=%u last_corebrightness_millinits=%lld "
                                               @"last_result=%s policy=sync1\n", gSyncs, gTaken, gFailed, panel, cb, result];
  [text writeToFile:[kStatusDir stringByAppendingPathComponent:@"status"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

static void arm(double seconds)
{
  const CFAbsoluteTime want = CFAbsoluteTimeGetCurrent() + seconds;
  if (gTimer && CFRunLoopTimerGetNextFireDate(gTimer) > want) CFRunLoopTimerSetNextFireDate(gTimer, want);
}

static void evaluate(void)
{
  if (!gBacklight) return;
  const uint32_t panel = panelMilliNits(gBacklight);
  const int automatic = flag(gBacklight, CFSTR("piano-backlight-auto-active")), lit = flag(gBacklight, CFSTR("piano-backlight-lit"));
  const int64_t cb = automatic && lit ? coreBrightness() : -1;
  if (gLastSynced && cb >= 0 && pbs_matches((uint32_t)cb, gLastSynced)) {   // the last sync was taken
    ++gTaken;
    os_log(gLog, "CoreBrightness took %.3f nits (%u syncs, %u taken)", gLastSynced / 1000.0, gSyncs, gTaken);
    writeStatus(panel, cb, "taken");
    gLastSynced = 0;
  }
  const enum pbs_action action = pbs_decide(&gState, nowMs(), panel, automatic, lit, cb);
  if (action == PBS_SYNC) {
    ++gSyncs;
    NSDictionary *announce = @{@"piano-backlight-sync": @(pbs_fixed(panel))};
    const kern_return_t told = IORegistryEntrySetCFProperties(gBacklight, (__bridge CFDictionaryRef)announce);
    const BOOL set = told == KERN_SUCCESS && [client() setProperty:@((float)(panel / 1000.0)) withKey:@"DisplayNitsKey"];
    if (set) gLastSynced = panel; else ++gFailed;
    if (gSyncs <= 8 || gSyncs % 25 == 0 || !set)
      os_log(gLog, "sync %u: CoreBrightness %.3f -> the panel's %.3f nits (automatic): announce %#x, set %d", gSyncs, cb / 1000.0,
             panel / 1000.0, told, set);
    writeStatus(panel, cb, set ? "set" : told != KERN_SUCCESS ? "announce-refused" : "set-refused");
    arm(1.5);
  } else if (action == PBS_WAIT) {
    arm(1.0);
  }
}

static void interest(void *refcon, io_service_t service, natural_t type, void *argument)
{
  (void)refcon;
  (void)service;
  (void)argument;
  if (type == kIOMessageServicePropertyChange) arm(PBS_SETTLE_MS / 1000.0 + 0.1);
}

static void matched(void *refcon, io_iterator_t it)
{
  (void)refcon;
  for (io_service_t s; (s = IOIteratorNext(it));) {
    if (gBacklight) {
      IOObjectRelease(s);
      continue;
    }
    gBacklight = s;
    const kern_return_t r = IOServiceAddInterestNotification(gPort, s, kIOGeneralInterest, interest, NULL, &gInterest);
    os_log(gLog, "PianoBacklight found (interest %#x): CoreBrightness follows its automatic level", r);
    writeStatus(panelMilliNits(s), -1, "started");
    arm(2.0);
  }
}

static int check(void)
{
  io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoBacklight"));
  if (!s) {
    puts("backlight=missing");
    return 69;
  }
  const uint32_t panel = panelMilliNits(s);
  const int automatic = flag(s, CFSTR("piano-backlight-auto-active")), lit = flag(s, CFSTR("piano-backlight-lit"));
  const int64_t cb = coreBrightness();
  printf("boot_args=%s\npanel_millinits=%u\nautomatic=%s\nlit=%s\ncorebrightness_millinits=%lld\nin_step=%s\n",
         bootArg("piano-backlight-native=1") && bootArg("piano-backlight-auto=1") ? "yes" : "no", panel, automatic ? "yes" : "no",
         lit ? "yes" : "no", cb, cb >= 0 && panel && pbs_matches((uint32_t)cb, panel) ? "yes" : "no");
  struct pbs_state probe = {.level = panel, .level_since = 0};
  const enum pbs_action a = pbs_decide(&probe, PBS_SETTLE_MS, panel, automatic, lit, cb);
  printf("decision=%s\n", a == PBS_SYNC ? "sync" : a == PBS_WAIT ? "wait" : "idle");
  IOObjectRelease(s);
  return 0;
}

int main(int argc, const char **argv)
{
  @autoreleasepool {
    gLog = os_log_create("com.unixnu.piano", "brightness-sync");
    if (argc == 2 && !strcmp(argv[1], "--check")) return check();
    if (argc != 2 || strcmp(argv[1], "--run")) {
      fprintf(stderr, "usage: piano-brightness-sync --run | --check\n");
      return 64;
    }
    if (!bootArg("piano-backlight-native=1") || !bootArg("piano-backlight-auto=1")) {
      os_log(gLog, "inert: no piano-backlight-native=1 and piano-backlight-auto=1");
      return 0;
    }
    if (geteuid()) {
      fprintf(stderr, "piano-brightness-sync --run: root only (piano-backlight-sync)\n");
      return 77;
    }
    gPort = IONotificationPortCreate(kIOMainPortDefault);
    io_iterator_t it = IO_OBJECT_NULL;
    if (!gPort || IOServiceAddMatchingNotification(gPort, kIOFirstMatchNotification, IOServiceMatching("PianoBacklight"), matched,
                                                   NULL, &it) != KERN_SUCCESS) {
      os_log_error(gLog, "no IOKit notification port");
      return 69;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(gPort), kCFRunLoopDefaultMode);
    gTimer = CFRunLoopTimerCreateWithHandler(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 30, 30, 0, 0,
                                             ^(CFRunLoopTimerRef t) {
                                               (void)t;
                                               @autoreleasepool {
                                                 CFRunLoopTimerSetNextFireDate(gTimer, CFAbsoluteTimeGetCurrent() + 30);
                                                 evaluate();
                                               }
                                             });
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), gTimer, kCFRunLoopDefaultMode);
    matched(NULL, it);   // arms the matching notification and takes an already published PianoBacklight
    CFRunLoopRun();
  }
  return 0;
}
