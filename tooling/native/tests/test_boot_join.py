#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Offline acceptance of the actual boot transaction and SET22 owner latch."""
import subprocess
import tempfile
import unittest
from pathlib import Path

W = Path(__file__).resolve().parents[3]

class BootJoinTests(unittest.TestCase):
    def test_airportd_kick_starts_fallback_once(self):
        source = (W / 'tooling/userland/piano-airportd-kick/piano-airportd-kick.sh').read_text()
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            calls = path / 'calls'
            marker = path / 'marker'
            scripts = {
                'sysctl': 'case "$2" in\n  machdep.*) echo 1 ;;\n  *) echo fixture-boot ;;\nesac',
                'ioreg': """echo '"piano-wifi-apple80211-answering"'
echo '"BSD Name" = "en0"'""",
                'launchctl': f"""echo "$*" >> '{calls}'
case "$1 $2" in
  'kickstart -k') test ! -e '{path / 'fail-kick'}' ;;
  'kickstart system/com.unixnu.piano-wifi-join') test -f '{marker}' ;;
  'print system/com.apple.airportd') echo '   pid = 42' ;;
  *) exit 1 ;;
esac""",
            }
            for name, script in scripts.items():
                tool = path / name
                tool.write_text(f'#!/bin/sh\n{script}\n')
                tool.chmod(0o755)
            for original, replacement in {
                '/usr/sbin/sysctl': path / 'sysctl',
                '/usr/sbin/ioreg': path / 'ioreg',
                '/bin/launchctl': path / 'launchctl',
                '/var/run/piano-airportd-kick': marker,
                '/dev/console': path / 'console',
                '/usr/bin/logger': '/usr/bin/true',
                'sleep ': '/usr/bin/true ',
            }.items():
                self.assertIn(original, source)
                source = source.replace(original, str(replacement))
            script = path / 'kick.sh'
            script.write_text(source)
            (path / 'fail-kick').touch()
            self.assertNotEqual(subprocess.run(['/bin/sh', str(script)], capture_output=True).returncode, 0)
            self.assertFalse(marker.exists())
            self.assertNotIn('kickstart system/com.unixnu.piano-wifi-join', calls.read_text())
            (path / 'fail-kick').unlink()
            calls.unlink()
            result = subprocess.run(['/bin/sh', str(script)], check=True, capture_output=True, text=True)
            self.assertIn('PianoWifiAirportdKickV1 helper=started', result.stdout)
            self.assertEqual(marker.read_text(), 'fixture-boot\n')
            subprocess.run(['/bin/sh', str(script)], check=True, capture_output=True)
            self.assertEqual([line for line in calls.read_text().splitlines() if line.startswith('kickstart ')],
                             ['kickstart -k system/com.apple.airportd', 'kickstart system/com.unixnu.piano-wifi-join'])

    def test_native_profile_idempotence(self):
        # A real CoreWiFi external-form round trip backed by a synthetic store.
        # The test never opens the host's Keychain or sends airportd a write.
        source = r'''
#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>
#include <assert.h>
static NSDictionary *records;
static unsigned adds;
@interface FixtureStore : NSObject
- (instancetype)initWithType:(NSInteger)type identifier:(NSString *)identifier;
- (void)activate;
- (NSDictionary *)dictionaryRepresentation;
@end
@implementation FixtureStore
- (instancetype)initWithType:(NSInteger)type identifier:(NSString *)identifier {
  assert(type == 1 && [identifier isEqualToString:@"com.apple.wifi.known-networks"]);
  return [super init];
}
- (void)activate {}
- (NSDictionary *)dictionaryRepresentation { return records; }
@end
static Class fixture_class(NSString *name) {
  return [name isEqualToString:@"CWFKeyValueStore"] ? FixtureStore.class : NSClassFromString(name);
}
#define NSClassFromString fixture_class
#define CWAddNetworkProfile fixture_add
#define main production_main
#include "tooling/native/piano-wifi-join/piano-wifi-join.m"
#undef main
@interface CWFNetworkProfile (Fixture)
@property (readonly) NSDictionary *externalForm;
- (void)setSystemMode:(BOOL)on;
- (void)setAutoJoinDisabled:(BOOL)disabled;
@end
OSStatus fixture_add(CWNetworkProfile *legacy, SFAuthorization *authorization) {
  assert(!authorization);
  ++adds;
  CWFNetworkProfile *profile = [legacy coreWiFiNetworkProfile];
  [profile setSystemMode:YES];
  NSMutableDictionary *next = [records mutableCopy];
  next[profile.identifier] = profile.externalForm;
  records = next;
  return errSecSuccess;
}
int main(void) {
  @autoreleasepool {
    struct piano_wifi_profile p;
    const char input[] = "ssid=fixture-network\nsecurity=open\n";
    assert(!piano_wifi_profile_parse((const uint8_t *)input,sizeof input-1,&p));
    NSData *ssid = [NSData dataWithBytes:p.ssid length:p.ssid_len];
    NSString *key = @"wifi.network.ssid.fixture-network";
    NSString *other = @"wifi.network.ssid.fixture-other";
    records = @{other:@{}};
    assert(provision(&p) == SavedReady && adds == 1 && records.count == 2 && records[other]);
    assert(provision(&p) == SavedReady && adds == 1);
    CWFNetworkProfile *native = [[fixture_class(@"CWFNetworkProfile") alloc] initWithExternalForm:records[key]];
    assert([native.identifier isEqualToString:key] && native.isSystemMode);
    [native setAutoJoinDisabled:YES];
    records = @{key:native.externalForm};
    assert(provision(&p) == SavedPreserved && adds == 1);
    [native setAutoJoinDisabled:NO];
    records = @{key:native.externalForm, other:native.externalForm};
    assert(provision(&p) == SavedPreserved && adds == 1);
    records = nil;
    assert(provision(&p) == SavedUnavailable && adds == 1);
    piano_wifi_profile_wipe(&p);
    CWMutableNetworkProfile *legacy = [CWMutableNetworkProfile networkProfile];
    legacy.ssidData = ssid;
    legacy.security = kCWSecurityWPA2Personal;
    native = [legacy coreWiFiNetworkProfile];
    [native setSystemMode:YES];
    assert(native.supportedSecurityTypes == 0x50);
    records = @{key:native.externalForm};
    assert(known_profile_state(records,ssid,0x50,YES) == KnownMatches);
    assert(known_profile_state(records,ssid,0x10,NO) == KnownMatches);
    assert(known_profile_state(records,ssid,0x10,YES) == KnownPreserve);
  }
}
'''
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            (path / 'test.m').write_text(source)
            subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', '-I', str(W),
                            str(path / 'test.m'), '-framework', 'Foundation', '-framework', 'CoreWLAN',
                            '-framework', 'CoreLocation', '-framework', 'Security', '-o', str(path / 'test')],
                           check=True, capture_output=True)
            subprocess.run([str(path / 'test')], check=True, capture_output=True)

    def test_real_owner_and_one_shot_transport(self):
        source = r'''
#include "package/PianoWifi/WifiUserControl.h"
#include "tooling/native/piano-wifi-join/BootJoin.h"
#include <cassert>
using namespace pianowifi;
struct Fixture { WifiUserControl owner; int polls=0,calls=0,pauses=0; unsigned gen=0; bool fail=false,missing=false,skip=false; };
int ready(void *v) {auto &f=*(Fixture*)v; return f.skip ? -1 : ++f.polls >= 4;}
uint32_t receipt(void *v) {return ((Fixture*)v)->gen;}
int associate(void *v) {auto &f=*(Fixture*)v; ++f.calls; assert(f.polls>=4); if(f.fail)return 0;
  assert(f.owner.join()); if(!f.missing)++f.gen; return 1;}
void pause(void *v) {++((Fixture*)v)->pauses;}
int main() {
 for(int mode=0;mode<3;++mode) {
  Fixture f; auto stop=f.owner.disconnect(); f.owner.stopped(stop);
  assert(!f.owner.allowed(f.owner.read()));
  // Reapplied ON restores permission, but cannot publish the cold-boot profile.
  const auto on=f.owner.setPower(true);
  assert(f.owner.allowed(on) && f.owner.serial(on)==f.owner.serial(stop));
  for(int n=0;n<1000;++n) assert(f.owner.setPower(true)==on);
  assert(!f.gen && !f.calls);
  f.fail=mode==1; f.missing=mode==2;
  piano_boot_join_transport t={&f,ready,receipt,associate,pause};
  assert(piano_boot_join(&t,10)==(mode==0?0:mode==1?2:3)); assert(f.calls==1);
  if(!mode)assert(f.owner.allowed(f.owner.read()));
  stop=f.owner.disconnect(); f.owner.stopped(stop);
  assert(!f.owner.allowed(f.owner.read()));
 }
 Fixture f; piano_boot_join_transport t={&f,ready,receipt,associate,pause};
 assert(piano_boot_join(&t,1)==1 && f.calls==0);
 f.skip=true;
 assert(piano_boot_join(&t,10)==PianoBootJoinSkipped && f.calls==0);
}
'''
        with tempfile.TemporaryDirectory() as d:
            path=Path(d); (path/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(W),str(path/'test.cpp'),'-o',str(path/'test')],check=True,capture_output=True)
            subprocess.run([str(path/'test')],check=True,capture_output=True)

    def test_actual_profile_safety(self):
        # Only ownership is synthesized; actual open/no-follow/mode/size/parser run.
        source = r'''
#import <Foundation/Foundation.h>
#include <sys/stat.h>
static int fixture_lstat(const char *p, struct stat *s) {int e=lstat(p,s);if(!e)s->st_uid=0;return e;}
static int fixture_fstat(int fd, struct stat *s) {int e=fstat(fd,s);if(!e)s->st_uid=0;return e;}
#define lstat fixture_lstat
#define fstat fixture_fstat
#define main production_main
#include "tooling/native/piano-wifi-join/piano-wifi-join.m"
#undef main
#include <assert.h>
int main(int argc,char **argv) {
 (void)argc; int fd=safe_open(argv[1]); assert(fd>=0); close(fd);
 char boot[64]; assert(boot_uuid(boot,sizeof boot));
 FILE *marker=fopen(argv[1],"w"); assert(marker && fprintf(marker,"%s\n",boot)>0); fclose(marker);
 assert(kick_completed(argv[1],boot));
 assert(!kick_completed(argv[1],"01234567-0000-0000-0000-000000000000"));
 assert(!kick_completed(argv[2],boot) && !kick_completed(argv[3],boot));
 chmod(argv[1],0644); assert(!kick_completed(argv[1],boot)); chmod(argv[1],0600);
 chmod(argv[1],0644); assert(safe_open(argv[1])<0); chmod(argv[1],0600);
 assert(safe_open(argv[2])==-2); assert(safe_open(argv[3])<0);
 struct piano_wifi_profile p; const char *good="ssid=fixture\nsecurity=wpa2-psk\npsk=fixture-only\n";
 assert(!piano_wifi_profile_parse((const uint8_t*)good,(uint32_t)strlen(good),&p)); piano_wifi_profile_wipe(&p);
 assert(piano_wifi_profile_parse((const uint8_t*)good,(uint32_t)strlen(good)-1,&p)); piano_wifi_profile_wipe(&p);
}
'''
        with tempfile.TemporaryDirectory() as d:
            path=Path(d); path.chmod(0o700); profile=path/'profile';profile.write_text('fixture\n');profile.chmod(0o600)
            (path/'link').symlink_to(profile);(path/'test.m').write_text(source)
            subprocess.run(['xcrun','clang','-fobjc-arc','-I',str(W),str(path/'test.m'),'-framework','Foundation','-framework','CoreWLAN','-framework','CoreLocation','-framework','Security','-o',str(path/'test')],check=True,capture_output=True)
            subprocess.run([str(path/'test'),str(profile),str(path/'missing'),str(path/'link')],check=True,capture_output=True)

    def test_location_readiness_gates_the_scan(self):
        source = r'''
#include <libproc.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
static int pid_before, pid_after, reads, count;
static uint64_t seconds;
static int fixture_list(void *buffer, int size) {
  if (!buffer) return count;
  assert(size >= count * (int)sizeof(pid_t));
  pid_t *pids = buffer;
  for (int i = 0; i < count; ++i) pids[i] = (reads ? pid_after : pid_before) + i;
  ++reads;
  return count;
}
static int fixture_name(int pid, void *buffer, uint32_t size) {
  (void)pid;
  return snprintf(buffer, size, "locationd");
}
static uint64_t fixture_clock(clockid_t clock) {
  (void)clock;
  return seconds * 1000000000ULL;
}
#define proc_listallpids fixture_list
#define proc_name fixture_name
#define clock_gettime_nsec_np fixture_clock
#define main production_main
#include "tooling/native/piano-wifi-join/piano-wifi-join.m"
#undef main
static int check(struct location_readiness *state, uint64_t at, int before,
                 int after, int candidates, char *query) {
  seconds = at; pid_before = before; pid_after = after; count = candidates; reads = 0;
  return location_ready(state, query, 1);
}
int main(void) {
  struct location_readiness state = {};
  assert(!check(&state, 120, 0, 0, 0, "/usr/bin/true"));
  assert(!check(&state, 370, 100, 100, 1, "/usr/bin/true"));
  assert(!check(&state, 379, 101, 101, 1, "/usr/bin/true"));
  assert(!check(&state, 388, 101, 101, 1, "/usr/bin/true"));
  assert(reads == 1);
  assert(!check(&state, 389, 101, 102, 1, "/usr/bin/true"));
  assert(reads == 2);
  assert(!check(&state, 390, 102, 102, 2, "/usr/bin/true"));
  assert(!check(&state, 391, 102, 102, 1, "/usr/bin/true"));
  assert(!check(&state, 401, 102, 102, 1, "/usr/bin/false"));
  assert(!check(&state, 402, 102, 102, 1, "/usr/bin/true"));
  assert(check(&state, 412, 102, 102, 1, "/usr/bin/true"));
  struct apple_join a = { .kick_time = 400 };
  assert(!ready(&a)); /* grace precedes any real CoreWLAN call */
  reads = 0;
  assert(!location_allows_join(&state, "/usr/bin/false", 1, 59) && reads == 0);
  assert(location_allows_join(&state, "/usr/bin/false", 2, 60) && reads == 0);
}
'''
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            (path / 'test.m').write_text(source)
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-I', str(W), str(path / 'test.m'),
                            '-framework', 'Foundation', '-framework', 'CoreWLAN',
                            '-framework', 'CoreLocation', '-framework', 'Security', '-o', str(path / 'test')],
                           check=True, capture_output=True)
            subprocess.run([str(path / 'test')], check=True, capture_output=True)

if __name__=='__main__': unittest.main()
