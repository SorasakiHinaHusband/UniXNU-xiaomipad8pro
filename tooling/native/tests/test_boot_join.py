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
    def test_real_owner_and_one_shot_transport(self):
        source = r'''
#include "package/PianoWifi/WifiUserControl.h"
#include "tooling/native/piano-wifi-join/BootJoin.h"
#include <cassert>
using namespace pianowifi;
struct Fixture { WifiUserControl owner; int polls=0,calls=0,pauses=0; unsigned gen=0; bool fail=false,missing=false; };
int ready(void *v) {return ++((Fixture*)v)->polls >= 4;}
uint32_t receipt(void *v) {return ((Fixture*)v)->gen;}
int associate(void *v) {auto &f=*(Fixture*)v; ++f.calls; assert(f.polls>=4); if(f.fail)return 0;
  assert(f.owner.join()); if(!f.missing)++f.gen; return 1;}
void pause(void *v) {++((Fixture*)v)->pauses;}
int main() {
 for(int mode=0;mode<3;++mode) {
  Fixture f; auto stop=f.owner.disconnect(); f.owner.stopped(stop);
  for(int n=0;n<1000;++n) {f.owner.setPower(true); assert(!f.owner.allowed(f.owner.read()));}
  f.fail=mode==1; f.missing=mode==2;
  piano_boot_join_transport t={&f,ready,receipt,associate,pause};
  assert(piano_boot_join(&t,10)==(mode==0?0:mode==1?2:3)); assert(f.calls==1);
  if(!mode)assert(f.owner.allowed(f.owner.read()));
  stop=f.owner.disconnect(); f.owner.stopped(stop);
  for(int n=0;n<1000;++n)assert(!f.owner.allowed(f.owner.read()));
 }
 Fixture f; piano_boot_join_transport t={&f,ready,receipt,associate,pause};
 assert(piano_boot_join(&t,1)==1 && f.calls==0);
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
            subprocess.run(['xcrun','clang','-fobjc-arc','-I',str(W),str(path/'test.m'),'-framework','Foundation','-framework','CoreWLAN','-o',str(path/'test')],check=True,capture_output=True)
            subprocess.run([str(path/'test'),str(profile),str(path/'missing'),str(path/'link')],check=True,capture_output=True)

if __name__=='__main__': unittest.main()
