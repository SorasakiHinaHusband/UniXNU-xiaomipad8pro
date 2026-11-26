# Source from the installed-system runner, then call metal_sustain_installed after the desktop gate.
# Uses tabletrun.sh helpers and its live R, C and MARK variables. Does not boot or change policy.

metal_sustain_snapshot() {
  rsudo 'printf "BOOT="; /usr/sbin/sysctl -n kern.bootsessionuuid; /usr/sbin/sysctl -n machdep.piano_gpu_state; /usr/sbin/sysctl -n machdep.piano_gpu_recovery'
}

# Run under the proven console login. rssh/rsudo can return 0 for a failed remote command.
metal_sustain_launch() {
  local dir=$1 args=$2 prefix=$3
  rsudo "u=\$(/usr/bin/stat -f %u /dev/console); test \"\$u\" -ge 501 || exit 1
    /bin/launchctl asuser \"\$u\" /usr/bin/sudo -n -u \"#\$u\" -- $dir/piano-metal-sustain --onscreen $args --width 1920 --height 1280 --draws 256 > $dir/$prefix.csv 2> $dir/$prefix.log
    rc=\$?; printf \"METAL_SUSTAIN_REMOTE_RC=%s\\n\" \"\$rc\"; exit \"\$rc\""
}

metal_sustain_installed() {
  local hostdir=tooling/native/piano-metal-sustain
  local bin=${METAL_SUSTAIN_BIN:-out/piano-metal-sustain/piano-metal-sustain}
  local name=metal-sustain d want got tag lo hi old_mark
  local result="$R/$name"
  if [ "${DRY:-0}" = 1 ]; then verdict 'INFO metal-sustain dry run'; return 0; fi
  if [ ! -x "$bin" ] || [ ! -s "$C" ]; then
    verdict 'FAIL HARNESS metal-sustain: staged host binary or active recorder missing'; return 1
  fi
  mkdir -p "$result" || return 1
  rssh 'u=$(/usr/bin/stat -f %u /dev/console); test "$u" -ge 501 && test "$u" = "$(/usr/bin/id -u)" && /usr/bin/pgrep -u "$u" -x Finder >/dev/null && echo METAL_SUSTAIN_GUI_OK' > "$result/gui.txt"
  if ! grep -qx METAL_SUSTAIN_GUI_OK "$result/gui.txt"; then
    verdict 'FAIL HARNESS metal-sustain: SSH user does not own a live Finder session'; return 1
  fi
  rssh '/usr/bin/pmset -g; /usr/bin/sw_vers' > "$result/environment.txt"
  d=$(rssh 'umask 077; /usr/bin/mktemp -d /private/var/tmp/piano-metal-sustain.XXXXXXXX' |
      tr -d '\r' | grep -xE '/private/var/tmp/piano-metal-sustain\.[A-Za-z0-9]{8}')
  if ! [[ "$d" =~ ^/private/var/tmp/piano-metal-sustain\.[A-Za-z0-9]{8}$ ]]; then
    verdict 'FAIL HARNESS metal-sustain: remote temporary directory missing'; return 1
  fi
  want=$(shasum -a 256 "$bin" | cut -d ' ' -f1)
  SSH_T=60 rssh "umask 077; /usr/bin/base64 -D > $d/piano-metal-sustain && /bin/chmod 755 $d/piano-metal-sustain && /usr/bin/shasum -a 256 $d/piano-metal-sustain" \
    < <(/usr/bin/base64 -i "$bin") > "$result/staging.txt"
  got=$(awk -v p="$d/piano-metal-sustain" '$2 == p {print $1}' "$result/staging.txt")
  if [ -z "$want" ] || [ "$want" != "$got" ]; then
    verdict 'FAIL HARNESS metal-sustain: staged executable SHA-256 differs'; return 1
  fi
  SSH_T=30 metal_sustain_launch "$d" '--seconds 3' smoke > "$result/smoke-run.txt" || true
  if ! grep -qx METAL_SUSTAIN_REMOTE_RC=0 "$result/smoke-run.txt" ||
     ! pull "$name/smoke.log" "$d/smoke.log" ||
     ! grep -aqE '^WINDOW .*gpu_timed=[1-9][0-9]* ' "$result/smoke.log"; then
    verdict "FAIL metal-sustain: three-second warmup check failed ($d/smoke.log)"; return 1
  fi

  # Allow the short smoke's demand vote to settle before the pre-run snapshot.
  rssh '/bin/sleep 5; echo METAL_SUSTAIN_SETTLED' > "$result/settle.txt"
  if ! grep -qx METAL_SUSTAIN_SETTLED "$result/settle.txt"; then
    verdict 'FAIL HARNESS metal-sustain: settle check did not return'; return 1
  fi
  metal_sustain_snapshot > "$result/before.txt"
  if ! grep -aqE '^at_us=[0-9]+ lvl=[0-9]+ mhz=[0-9]+' "$result/before.txt" ||
     ! grep -aqE '^replay [01] recoveries [0-9]+' "$result/before.txt"; then
    verdict 'FAIL HARNESS metal-sustain: installed GPU state or recovery sysctl unavailable'; return 1
  fi

  tag=${d##*.}
  old_mark=$MARK
  mark
  lo=$MARK
  rsudo "printf \"PIANO_METAL_SUSTAIN %s BEGIN\\n\" $tag > /dev/console" > "$result/mark-begin.txt"
  if ! waitfor "PIANO_METAL_SUSTAIN $tag BEGIN" 10; then
    MARK=$old_mark
    verdict 'FAIL HARNESS metal-sustain: console BEGIN marker missing'; return 1
  fi
  SSH_T=650 metal_sustain_launch "$d" '--minutes 10' frames > "$result/run.txt" || true
  rsudo "printf \"PIANO_METAL_SUSTAIN %s END\\n\" $tag > /dev/console" > "$result/mark-end.txt"
  waitfor "PIANO_METAL_SUSTAIN $tag END" 10 || true
  hi=$(lines)
  MARK=$old_mark
  awk -v lo="$lo" -v hi="$hi" -v tag="$tag" '
    NR > hi { exit }
    NR > lo && index($0, "PIANO_METAL_SUSTAIN " tag " BEGIN") { capture=1 }
    capture { print }
    capture && index($0, "PIANO_METAL_SUSTAIN " tag " END") { exit }
  ' "$C" | tr -d '\r' > "$result/console.txt"
  metal_sustain_snapshot > "$result/after.txt"
  local pull_failed=0
  pull "$name/summary.log" "$d/frames.log" || pull_failed=1
  pull "$name/frames.csv" "$d/frames.csv" || pull_failed=1
  if [ "$pull_failed" -ne 0 ]; then
    verdict "FAIL HARNESS metal-sustain: evidence transfer failed ($d)"; return 1
  fi
  local grade_rc
  if python3 "$hostdir/grade.py" "$result" > "$result/grade.txt" 2>&1; then
    verdict 'PASS metal-sustain: 600-second installed Metal 3D benchmark (metal-sustain/grade.txt)'
    return 0
  else
    grade_rc=$?
  fi
  if [ "$grade_rc" -eq 2 ] && grep -q '^VOID installed Metal sustain (CONFOUND):' "$result/grade.txt"; then
    verdict 'VOID metal-sustain: external interaction in bounded console capture (metal-sustain/grade.txt)'
    return 2
  fi
  verdict 'FAIL metal-sustain: see metal-sustain/grade.txt'
  return 1
}
