# SPDX-License-Identifier: BSD-2-Clause-Patent
# pool-host.sh: the host-side functions of v13.1y10's installer (out/v131-install/install-v131y10.sh of the v13.1y10 package: phase,
# stop_capture, fb, abl, wait_abl, disk_boot, ms, read_ledger, publish_ledger), copied for tooling/pool/pool-install.sh, which sources
# the package copies of install-functions.sh and install-gate.sh first (rssh, rsudo, put, settle, desk_login, create_remote, go_abl,
# battery_preflight, live_boot, the silent gates). publish_ledger takes the verified image and SHA-256 as arguments. ms names the
# boot image by a variable. No device action runs when this file is sourced.
phase() { PHASE=$1; printf '%s\n' "$PHASE" > "${ROUND:?}/phase.next"; mv -f "${ROUND:?}/phase.next" "${ROUND:?}/phase"; }
stop_capture() { if [ -n "${CAP:-}" ]; then kill "$CAP" 2>/dev/null || true; wait "$CAP" 2>/dev/null || true; CAP=""; fi; }
fb() { sh "$WORK/tooling/native/tmo.sh" 15 fastboot -s "${PIANO_SERIAL:?}" "$@" 2>&1; }
abl() { fb getvar current-slot | grep -Eq '^(\(bootloader\) )?current-slot: a[[:space:]]*$'; }
wait_abl() { local i; for i in $(seq 1 24); do abl && return 0; nap 5; done; return 1; }
disk_boot() {
    abl || return 1
    python3 "$WORK/tooling/native/usb-console.py" --port 0 --log-dir "$ROUND/usb" \
        < /dev/null > "$CONSOLE" 2>&1 &
    CAP=$!
    nap 2
    kill -0 "$CAP" || return 2
    fb reboot >/dev/null || return 1
    settle || return 1
    [ "$(rssh 'sw_vers -productVersion')" = 27.3 ] || return 1
    grep -aq '^\[usb-console\] connected:' "$CONSOLE" || return 2
}
ms() {
    local log=$1
    shift
    python3 "$MAINT" --session "$MSESSION" --boot-image "${BOOT_IMAGE:-boot-piano-macos-persistent-v5-signed.img}" "$@" > "$log" 2>&1
}
pool_gate_args() {
    local old profile
    [ -f "${POOL_BASE_GATE_SPEC:-}" ] && [ -f "${POOL_PROFILE_ARGS:-}" ] || return 1
    old=$(sed -n 's/^NEED_ARGS="\(.*\)"$/\1/p' "$POOL_BASE_GATE_SPEC") && [ -n "$old" ] || return 1
    profile=$(cat "$POOL_PROFILE_ARGS") && [ -n "$profile" ] || return 1
    python3 - "$old" "$profile" <<'PY'
import sys
needed = dict(arg.partition('=')[::2] for arg in sys.argv[1].split())
needed.update(arg.partition('=')[::2] for arg in ('piano-touch=2 ' + sys.argv[2]).split())
print(' '.join(f'{key}={value}' for key, value in needed.items()))
PY
}
read_ledger() {
    python3 - "${3:-$PIANO_PERSIST_OUT/installed.json}" "$1" "$2" <<'PY'
import json, sys
from pathlib import Path
record = json.loads(Path(sys.argv[1]).read_text())
if [record['image'], record['sha256']] != sys.argv[2:]:
    raise SystemExit('installed ledger does not match the verified ESP')
PY
}
publish_ledger() {   # IMAGE SHA256: main's installed observer reads these paths; a ledger is only copied after read-back
    local ledger=${POOL_MAIN_LEDGER:-$MAIN/out/persistent} img=$1 want=$2 f
    local next=$ledger/installed.json.$$.new
    read_ledger "$img" "$want" || return 1
    for f in "${img%.img}.json" "$img"; do
        if [ ! -f "$ledger/$f" ]; then
            cp -p "${PIANO_PERSIST_OUT:?}/${f:?}" "${ledger:?}/${f:?}.$$.new" &&
                mv -f "${ledger:?}/${f:?}.$$.new" "${ledger:?}/${f:?}" || return 1
        fi
    done
    [ "$(cache_sha "$ledger/$img")" = "$want" ] &&
        cmp -s "$ledger/${img%.img}.json" "$PIANO_PERSIST_OUT/${img%.img}.json" || return 1
    cp "${PIANO_PERSIST_OUT:?}/installed.json" "$next" && read_ledger "$img" "$want" "$next" &&
        mv -f "$next" "${ledger:?}/installed.json" && return 0
    rm -f "$next"
    return 1
}
