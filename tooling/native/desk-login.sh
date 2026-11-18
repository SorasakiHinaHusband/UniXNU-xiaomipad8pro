#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Log the owner in on a desktop image (desk, desk-dev: docs/attended-desktop.md) from this Mac, over PianoDwc3's keyboard
# (piano-usb=2 piano-usb-hid=1, tooling/native/piano-input.py), with the password he left in the ignored private/login.secret. The
# password is handed to piano-input.py on stdin (`type --stdin`): never echoed, never in argv or ps, never written anywhere; this
# script prints only what it waited for and when.
#
#   desk-login.sh [--console FILE] [--timeout SECS] [--secret FILE] [--webcam-dir DIR] [--now]
#
# The input transaction is: non-text wake, NEW lit observation, selected account
# and secure-field focus proof, clear, one stdin type. Finder is marked BEFORE
# typing, then a new Finder and matching console UID prove success. --now only
# skips waiting for SecurityAgent; it never bypasses readiness.
# DESK_LOGIN_INPUT / DESK_LOGIN_PROBE replace transports for offline tests.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
MAIN=$(git rev-parse --git-common-dir); MAIN=$(cd "$MAIN/.." && pwd -P)
CONSOLE= TIMEOUT=600 SECRET=$MAIN/private/login.secret NOW=0 LOGIN_UID=${DESK_LOGIN_UID:-501} WEBCAM_DIR=
while [ $# -gt 0 ]; do
  case $1 in
    --console) CONSOLE=$2; shift ;;
    --timeout) TIMEOUT=$2; shift ;;
    --secret) SECRET=$2; shift ;;
    --webcam-dir) WEBCAM_DIR=$2; shift ;;
    --uid) LOGIN_UID=$2; shift ;;
    --now) NOW=1 ;;
    *) echo "desk-login: unknown argument" >&2; exit 2 ;;
  esac
  shift
done
case $LOGIN_UID in ''|*[!0-9]*|0) echo 'desk-login: a nonzero target UID is required' >&2; exit 2;; esac
[ -s "$SECRET" ] || { echo "desk-login: secret is missing or empty" >&2; exit 2; }
case $(cd "$(dirname "$SECRET")" && pwd -P)/ in "$D/private/"*|"$D/out/"*|"$MAIN/private/"*) ;; *)
  echo "desk-login: secret is not under this repository's private/ or out/" >&2; exit 2 ;; esac
read -r -a INPUT <<< "${DESK_LOGIN_INPUT:-python3 tooling/native/piano-input.py}"
if [ -n "$WEBCAM_DIR" ]; then
  mkdir -p -- "$WEBCAM_DIR"
  command -v imagesnap >/dev/null
  command -v ffmpeg >/dev/null
fi
probe() {
  if [ -n "${DESK_LOGIN_PROBE:-}" ]; then
    local command
    read -r -a command <<< "$DESK_LOGIN_PROBE"
    "${command[@]}" "$1" "$LOGIN_UID"
  else
    # tssh owns address fallback. Credentials only on sudo stdin, never argv.
    { cat "$SECRET"; printf '\n'; } | /bin/bash /tmp/tssh "sudo -S -p '' /usr/local/libexec/piano-login-readiness $1 $LOGIN_UID" 2>/dev/null
  fi
}
start=$(date +%s)
left() { echo $(( TIMEOUT - ($(date +%s) - start) )); }
since() { echo $(( $(date +%s) - start )); }
lines() { wc -l < "$CONSOLE" | tr -d ' '; }
after() { tail -n +"$(( $1 + 1 ))" "$CONSOLE" | grep -a -c -E "$2" > /dev/null; }
# Require an observation AFTER the wake mark, and reject a subsequent power-off
# or black observation. An old 99% frame can never satisfy this predicate.
fresh_lit() {
  awk -v mark="$1" '
    BEGIN {on=1}
    /NativeGopDisplay: display off/ {on=0;lit=0}
    /NativeGopDisplay: display on/ {on=1}
    NR>mark && /NativeGopDisplay: .*last frame / {
      v=$0;sub(/^.*last frame /,"",v);sub(/%.*$/, "",v);lit=(on && v+0>=50)
    }
    END {exit !lit}' "$CONSOLE"
}
prove_lit() {
  if [ -n "$WEBCAM_DIR" ]; then
    /bin/bash tooling/native/panel-lit.sh "$WEBCAM_DIR/$2.jpg"
  else
    fresh_lit "$1"
  fi
}
if [ -z "$CONSOLE" ]; then
  while [ "$(left)" -gt 0 ]; do
    CONSOLE=$(ls -t logs/usb-console/*-port0.txt 2>/dev/null | head -n 1 || true)
    [ -n "$CONSOLE" ] && [ $(( $(date +%s) - $(stat -f %m "$CONSOLE") )) -lt 120 ] && break
    CONSOLE=; sleep 3
  done
fi
[ -n "$CONSOLE" ] && [ -f "$CONSOLE" ] || { echo 'desk-login: no live console; nothing typed' >&2; exit 2; }
echo "desk-login: watching $CONSOLE"
if [ "$NOW" = 0 ]; then
  until grep -aq 'PianoA830: SecurityAgent opens the accelerator' "$CONSOLE"; do
    [ "$(left)" -gt 0 ] || { echo 'desk-login: no SecurityAgent; nothing typed' >&2; exit 2; }
    sleep 3
  done
fi
until "${INPUT[@]}" status >/dev/null 2>&1; do
  [ "$(left)" -gt 0 ] || { echo 'desk-login: input unavailable; nothing typed' >&2; exit 2; }
  sleep 2
done
wake_mark=$(lines)
"${INPUT[@]}" key shift >/dev/null
echo "desk-login: non-text wake sent; fresh-frame mark=$wake_mark"
until prove_lit "$wake_mark" after-wake; do
  [ "$(left)" -gt 0 ] || { echo 'desk-login: no NEW lit frame; nothing typed' >&2; exit 2; }
  sleep 2
done
# Native AX proof fails closed on another account, missing secure field, or
# unavailable Accessibility access. It never inspects the password's value.
proof=$(probe prepare) || { echo 'desk-login: selected account/password focus unconfirmed; nothing typed' >&2; exit 2; }
printf '%s\n' "$proof" | grep -qx 'selected_login_account=yes'
printf '%s\n' "$proof" | grep -qx 'focused_password_field=yes'
"${INPUT[@]}" key cmd+a >/dev/null
"${INPUT[@]}" key backspace >/dev/null
proof=$(probe ready) || { echo 'desk-login: password focus lost; nothing typed' >&2; exit 2; }
printf '%s\n' "$proof" | grep -qx 'selected_login_account=yes'
printf '%s\n' "$proof" | grep -qx 'focused_password_field=yes'
prove_lit "$wake_mark" before-input || { echo 'desk-login: display became dark or no fresh webcam proof; nothing typed' >&2; exit 2; }
echo 'desk-login: NEW lit frame, selected account and focused cleared password field confirmed'
mark=$(lines); typed=$(since)
"${INPUT[@]}" type --stdin --enter < "$SECRET" >/dev/null
echo "desk-login: the password (not shown) and Return typed once at $(since)s"
for ((i=0; i<90; i++)); do
  if after "$mark" 'PianoA830: Finder opens the accelerator'; then
    proof=$(probe desktop) || proof=
    if printf '%s\n' "$proof" | grep -qx 'console_user_matches=yes'; then
      echo "desk-login: the desktop: NEW Finder and console user confirmed $(( $(since)-typed ))s after password"; exit 0
    fi
  fi
  sleep 2
done
echo 'desk-login: no NEW Finder plus console-user proof; no second input' >&2
exit 1
