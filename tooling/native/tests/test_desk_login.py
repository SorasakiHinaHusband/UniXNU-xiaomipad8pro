#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Real script, dummy stdin, no USB/SSH; historical lit-frame reproduction."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

W = Path(__file__).resolve().parents[3]
class LoginTests(unittest.TestCase):
    def run_case(self, mode):
        out=W/'out';out.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as d:
            p=Path(d); secret=p/'dummy.secret';secret.write_text('fixture-only\n');secret.chmod(0o600)
            console=p/'console';console.write_text('PianoA830: SecurityAgent opens the accelerator\nNativeGopDisplay: last frame 99.4% non-black\nNativeGopDisplay: display off\n')
            trace=p/'trace';hid=p/'hid';probe=p/'probe'
            hid.write_text('''#!/bin/sh
echo "$*" >> "$TRACE"
case "$1" in
status) [ "$MODE" != noinput ];;
key) if [ "$2" = shift ] && [ "$MODE" != stale ]; then
 echo 'NativeGopDisplay: display on' >> "$CONSOLE"
 echo 'NativeGopDisplay: last frame 98.1% non-black' >> "$CONSOLE"
 fi;;
type) cat >/dev/null; echo 'PianoA830: Finder opens the accelerator' >> "$CONSOLE";;
esac
''');hid.chmod(0o700)
            probe.write_text('''#!/bin/sh
echo "probe $1" >> "$TRACE"
[ "$MODE" != wrongaccount ] || exit 1
if [ "$1" = desktop ]; then echo console_user_matches=yes; else
 echo selected_login_account=yes; echo focused_password_field=yes; fi
''');probe.chmod(0o700)
            (p/'sleep').write_text('#!/bin/sh\n/bin/sleep 0.02\n');(p/'sleep').chmod(0o700)
            env=dict(os.environ,PATH=str(p)+':'+os.environ['PATH'],DESK_LOGIN_INPUT=str(hid),DESK_LOGIN_PROBE=str(probe),TRACE=str(trace),CONSOLE=str(console),MODE=mode)
            r=subprocess.run(['/bin/bash',str(W/'tooling/native/desk-login.sh'),'--console',str(console),'--timeout','1','--secret',str(secret)],env=env,capture_output=True,text=True,timeout=5)
            calls=trace.read_text().splitlines() if trace.exists() else []
            if mode=='ready':
                self.assertEqual(r.returncode,0,(r.stdout,r.stderr,calls))
                self.assertEqual(calls,['status','key shift','probe prepare','key cmd+a','key backspace','probe ready','type --stdin --enter','probe desktop'])
                self.assertIn('NEW Finder and console user confirmed',r.stdout)
            else:
                self.assertNotEqual(r.returncode,0)
                self.assertNotIn('type --stdin --enter',calls)
            self.assertNotIn('fixture-only',r.stdout+r.stderr)
    def test_stale_lit_current_off(self): self.run_case('stale')
    def test_wrong_account(self): self.run_case('wrongaccount')
    def test_input_missing(self): self.run_case('noinput')
    def test_new_frame_one_input_fast_finder(self): self.run_case('ready')
if __name__=='__main__': unittest.main()
