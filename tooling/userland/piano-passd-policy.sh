#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
# Native piano has no AppleKeyStore unlock contract. passd can accept a synchronous
# payment query before its coordinator exists and never reply. Disable that job
# for UID 501, preserving its prior effective override for removal. No Apple files
# or launchd databases are edited. See docs/userland.md, passd policy.
set -u
umask 077
R=${PIANO_USERLAND_ROOT:-}
STATE=$R/private/var/db/piano-userland/passd-policy
DOMAIN=gui/501
TARGET=$DOMAIN/com.apple.passd
AGENT=$DOMAIN/com.unixnu.piano-passd-policy
MODE=${1:---apply}
say() { echo "piano-passd-policy: $*"; logger -t piano-passd-policy "$*" 2>/dev/null || true; }
status() {
  say "$*"
  printf '%s\n' "$*" > "$STATE/status.new" && mv -f "$STATE/status.new" "$STATE/status"
}
fail() { status "FAIL $*"; exit 1; }
gui() { launchctl print "$DOMAIN" >/dev/null 2>&1; }
loaded() { launchctl print "$TARGET" >/dev/null 2>&1; }
override() {
  # A failed print is an error, never an absent/default override. Match the
  # complete label, not a regexp in which dots would match arbitrary characters.
  values=$(launchctl print-disabled "$DOMAIN" 2>/dev/null) || return 1
  printf '%s\n' "$values" | awk '$1 == "\"com.apple.passd\"" && $2 == "=>" {print $3; found=1} END {if (!found) print "default"}'
}
case "$MODE" in --install|--apply|--check|--remove) ;; *) say 'FAIL invalid mode'; exit 2 ;; esac
uid=$(id -u)
if [ "$MODE" = --apply ] && [ "$uid" != 501 ]; then
  say "SKIP uid=$uid (policy is only for UID 501)"; exit 0
fi
if [ "$MODE" = --install ]; then
  [ "$uid" = 0 ] || [ "${PIANO_USERLAND_NOCHOWN:-0}" = 1 ] || { say 'FAIL install requires root'; exit 1; }
  [ ! -L "$STATE" ] || { say 'FAIL state directory is a symlink'; exit 1; }
  mkdir -p "$STATE" && chmod 700 "$STATE" || exit 1
  if [ "${PIANO_USERLAND_NOCHOWN:-0}" != 1 ]; then
    chown 501:20 "$STATE" || exit 1
    # State is account-owned: never write predictable paths there as root.
    exec /usr/bin/sudo -u '#501' /bin/sh "$0" --apply
  fi
fi
if [ ! -d "$STATE" ]; then
  [ "$MODE" != --remove ] || { say 'RESTORED no prior policy'; exit 0; }
  say 'FAIL missing state directory (install the unit first)'; exit 1
fi
if [ "$MODE" = --remove ] && [ "$uid" = 0 ] && [ "${PIANO_USERLAND_NOCHOWN:-0}" != 1 ]; then
  exec /usr/bin/sudo -u '#501' /bin/sh "$0" --remove
fi
if [ "$MODE" = --remove ] && [ "$uid" = 0 ] && [ "${PIANO_USERLAND_NOCHOWN:-0}" != 1 ]; then
  exec /usr/bin/sudo -u '#501' /bin/sh "$0" --remove
fi
if [ "$MODE" = --remove ] && [ "$uid" = 0 ] && [ "${PIANO_USERLAND_NOCHOWN:-0}" != 1 ]; then
  exec /usr/bin/sudo -u '#501' /bin/sh "$0" --remove
fi
# Removal stops the agent before taking the lock, so it cannot re-disable passd
# after rollback. A failed bootout is checked rather than silently ignored.
if [ "$MODE" = --remove ] && launchctl print "$AGENT" >/dev/null 2>&1; then
  launchctl bootout "$AGENT" >/dev/null 2>&1 || fail 'cannot unload policy agent'
fi
if [ "$MODE" = --check ]; then
  if ! gui; then say 'PENDING gui/501 absent; check again after login'; exit 0; fi
  current=$(override) || { say 'FAIL cannot read disabled state'; exit 1; }
  [ "$current" = true ] || [ "$current" = disabled ] || { say 'FAIL passd is not disabled'; exit 1; }
  ! loaded || { say 'FAIL passd remains loaded'; exit 1; }
  grep -qx 'PASS disabled=true loaded=false uid=501 policy=passd1' "$STATE/status" || exit 1
  say 'PASS disabled=true loaded=false uid=501 policy=passd1'; exit 0
fi
i=0
until mkdir "$STATE/lock" 2>/dev/null; do
  i=$((i+1)); [ "$i" -lt 25 ] || { say 'FAIL state is locked; retry without deleting its lock'; exit 1; }
  sleep 0.2
done
trap 'rmdir "$STATE/lock" 2>/dev/null || true' EXIT
trap 'exit 1' HUP INT TERM
if [ "$MODE" = --remove ]; then
  if [ -f "$STATE/original" ]; then
    prior=$(cat "$STATE/original")
    case "$prior" in true|false|default) ;; *) fail 'invalid original state; receipt kept' ;; esac
    # Without an active GUI domain there is no documented CLI operation to
    # restore that domain's override. Refuse removal, retaining the entire unit
    # and receipt, so it can be retried after UID 501 logs in.
    gui || fail 'rollback needs gui/501; unit and receipt must be kept until login'
    if [ "$prior" = true ]; then
      launchctl disable "$TARGET" || fail 'restoring original disable failed'
      current=$(override) || fail 'cannot verify original disable'
      [ "$current" = true ] || [ "$current" = disabled ] || fail 'original disable not restored'
      if loaded; then launchctl bootout "$TARGET" || fail 'original disabled job remains loaded'; fi
    else
      launchctl enable "$TARGET" || fail 'enable failed; receipt kept'
      current=$(override) || fail 'cannot verify restored override'
      [ "$current" = false ] || [ "$current" = enabled ] || fail 'restored override is not enabled'
      if ! loaded; then
        launchctl bootstrap "$DOMAIN" "$R/System/Library/LaunchAgents/com.apple.passd.plist" || fail 'Apple agent bootstrap failed; receipt kept'
      fi
    fi
    status "RESTORED original=$prior uid=501" || exit 1
    rm -f "$STATE/original" || exit 1
  else status 'RESTORED policy never applied' || exit 1; fi
  exit 0
fi
if ! gui; then status 'PENDING gui/501 absent; Aqua agent applies at login'; exit $?; fi
if [ ! -f "$STATE/original" ]; then
  prior=$(override) || fail 'cannot read original disabled state'
  case "$prior" in true|disabled) prior=true ;; false|enabled) prior=false ;; default) ;; *) fail 'unrecognized original state' ;; esac
  printf '%s\n' "$prior" > "$STATE/original.new" && mv -f "$STATE/original.new" "$STATE/original" || fail 'cannot save original state'
fi
launchctl disable "$TARGET" || fail 'disable failed; no bootout attempted'
current=$(override) || fail 'cannot verify disable; no bootout attempted'
[ "$current" = true ] || [ "$current" = disabled ] || fail 'disable not persisted; no bootout attempted'
if loaded; then launchctl bootout "$TARGET" || fail 'passd bootout failed'; fi
! loaded || fail 'passd still loaded after bootout'
status 'PASS disabled=true loaded=false uid=501 policy=passd1'
