#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
umask 077
export LC_ALL=C
export PATH="/tmp/c920-shim:/opt/homebrew/bin:$PATH"
GUI_TOOLS=$(cd "$(dirname "$0")" && pwd -P)
GUI_REPO=$(cd "$GUI_TOOLS/../../.." && pwd -P)
harness=${PIANO_GUI_HARNESS:-$HOME/piano-harness}/bin
# shellcheck source=/dev/null
source "${PIANO_HARNESS_ENV:-$harness/../piano-harness.env}"
main_repo=${PIANO_HARNESS_REPO:?harness must name its working tree}
cd "$main_repo"
# shellcheck source=/dev/null
source "$harness/helper-guard.sh"
if is_helper; then echo 'Run from the main-session Mac shell.' >&2; exit 2; fi
[[ $# -le 1 ]] || { echo 'Usage: run-installed.sh [local-ssid-file]' >&2; exit 2; }
export GUI_SSID_FILE=${1:-$HOME/.config/piano/wifi.secret}
# Check tools before taking the tablet lock.
for cmd in xcrun python3 ffmpeg imagesnap; do command -v "$cmd" >/dev/null; done
for path in /tmp/tssh /tmp/tsudo /tmp/tmo.sh; do [[ -x $path ]]; done
export GUI_REDACT="$harness/redact-wifi.py"
[[ -r $GUI_REDACT ]]
[[ ! -e /tmp/piano-tablet.recovery-required ]]
[[ -f $GUI_SSID_FILE && -r $GUI_SSID_FILE ]] || { echo 'The Mac SSID file is missing.' >&2; exit 2; }

result_parent="$main_repo/private/host-checks/$(date +%Y%m%d)-runs"
mkdir -p "$result_parent"
R=$(mktemp -d "$result_parent/gui-checks.XXXXXXXX")
scratch=$(mktemp -d "${TMPDIR:-/tmp}/piano-gui.XXXXXXXX")
lock=/tmp/piano-tablet.lock
lock_owned=''
upload=''
GUI_REMOTE=''
note() { printf '%s %s\n' "$(date +%T)" "$*" | tee -a "$R/run.log"; }
verdict() { printf '%s\n' "$*" >> "$R/verdict.txt"; note "$*"; }
fexpect() { grep -Eq -- "$2" "$R/$1"; }
rssh() { /tmp/tmo.sh "${SSH_T:-12}" /tmp/tssh "$1"; }
rsudo() { /tmp/tmo.sh "${SSH_T:-12}" /tmp/tsudo "$1" </dev/null; }
run_remote() {
    rsudo "$GUI_REMOTE/session.sh $1 $GUI_BOOT $GUI_UID ${*:2}"
}
cleanup() {
    local rc=$?
    trap - EXIT
    set +e
    if [[ -n $GUI_REMOTE ]]; then
        run_remote cleanup > "$R/remote-cleanup.txt" 2>&1
        if ! fexpect remote-cleanup.txt '^GUI_CLEANUP_OK$'; then
            verdict "FAIL installed cleanup did not confirm $GUI_REMOTE"
            rc=2
        fi
    fi
    if [[ -n $upload ]]; then
        if ! rssh "cd $upload && (test ! -f target || unlink target) && (test ! -f gui || unlink gui) &&
            (test ! -f session.sh || unlink session.sh) && rmdir $upload" > "$R/staging-cleanup.txt" 2>&1; then
            verdict "FAIL staging cleanup did not confirm $upload"
            rc=2
        fi
    fi
    if [[ -n $lock_owned ]]; then
        unlink "$lock/owner"
        rmdir "$lock"
    fi
    for file in gui shot.sh; do [[ ! -e "$scratch/$file" ]] || unlink "$scratch/$file"; done
    rmdir "$scratch"
    note "Results in $R"
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Reuse tabletrun's shot helper alone. Sourcing the runner would start its boot path.
awk '/^shot\(\)/ { copying=1 } /^MARK=0$/ { exit } copying { print }' "$harness/tabletrun.sh" > "$scratch/shot.sh"
bash -n "$scratch/shot.sh"
# shellcheck source=/dev/null
source "$scratch/shot.sh"
declare -F shot >/dev/null
DRY=0 OBS_MODE=1 USB_SILENCE_S=0
log() { note "$@"; }

# Local compilation only. Transfer the two fixed files after compiling and taking the lock.
/bin/bash "$GUI_TOOLS/build.sh" "$scratch/gui"
mkdir "$lock" || { note 'Tablet lock is held'; exit 2; }
lock_owned=1
printf 'installed GUI checks %s\n' "$$" > "$lock/owner"
[[ ! -e /tmp/piano-tablet.recovery-required ]]

# shellcheck disable=SC2016
identity=$(rssh '
    b=$(/usr/sbin/sysctl -n kern.bootsessionuuid) &&
    v=$(/usr/sbin/sysctl -n kern.osversion) &&
    u=$(/usr/bin/stat -f %u /dev/console) &&
    a=$(/usr/sbin/sysctl -n kern.bootargs) &&
    echo "$b $v $u" && echo "$a"')
read -r GUI_BOOT build GUI_UID <<< "$identity"
[[ $GUI_BOOT =~ ^[0-9A-Fa-f-]{36}$ && $build == 26C5035r ]]
printf '%s\n' "$identity" | awk 'NR==2 {
    for (i=1; i<=NF; i++) if ($i ~ /^data-persist=/) { n++; ok=($i=="data-persist=2") }
} END { exit !(n==1 && ok) }'
printf 'boot=%s build=%s\n' "$GUI_BOOT" "$build" > "$R/installed.txt"
# Reuse the installed desk-login / unlock behavior with no brightness-key setup.
if ! "$main_repo/tooling/native/desk-login.sh" --state > "$R/desk-login.txt" 2>&1; then
    DESK_LOGIN_BRIGHTEN=0 "$main_repo/tooling/native/desk-login.sh" --now --timeout 120 \
        --secret "$main_repo/private/login.secret" --webcam-dir "$R" >> "$R/desk-login.txt" 2>&1
fi
GUI_UID=$(rssh '/usr/bin/stat -f %u /dev/console')
[[ $GUI_UID =~ ^[0-9]+$ && $GUI_UID -ge 501 ]]
[[ $(rssh '/usr/bin/id -u') == "$GUI_UID" ]]

out=$(rssh 'umask 077; /usr/bin/mktemp -d /private/var/tmp/piano-gui.XXXXXXXX')
[[ $out =~ ^/private/var/tmp/piano-gui\.[a-zA-Z0-9]{8}$ ]]
upload=$out
sha() { shasum -a 256 "$1" | cut -d ' ' -f1; }
for name in gui session.sh; do
    if [[ $name == gui ]]; then src="$scratch/gui"; else src="$GUI_TOOLS/session.sh"; fi
    expected="$(sha "$src")  $upload/$name"
    receipt=''
    for _ in 1 2 3; do
        receipt=$(rssh "umask 077; /usr/bin/base64 -D > $upload/$name && /usr/bin/shasum -a 256 $upload/$name" \
            < <(/usr/bin/base64 < "$src")) || continue
        [[ $receipt != "$expected" ]] || break
    done
    [[ $receipt == "$expected" ]] || { note "Transfer could not verify $name"; exit 2; }
done
a=$(sha "$scratch/gui")
b=$(sha "$GUI_TOOLS/session.sh")
# Install and verify root-owned copies before execution; the console can read/execute gui.
out=$(rsudo "d=\$(mktemp -d /private/var/run/piano-gui.XXXXXXXX) &&
    install -m 0755 $upload/gui \$d/gui && install -m 0700 $upload/session.sh \$d/session.sh &&
    a=\$(shasum -a 256 \$d/gui) && b=\$(shasum -a 256 \$d/session.sh) &&
    if test \"\${a%% *}\" = $a && test \"\${b%% *}\" = $b && chmod 755 \$d; then echo \$d; else
        unlink \$d/gui; unlink \$d/session.sh; rmdir \$d; exit 2; fi")
[[ $out =~ ^/private/var/run/piano-gui\.[a-zA-Z0-9]{8}$ ]]
GUI_REMOTE=$out
export GUI_TOOLS GUI_REPO GUI_REMOTE GUI_BOOT GUI_UID R DRY OBS_MODE USB_SILENCE_S
export GUI_TARGET="$upload/target"
export -f rssh rsudo run_remote shot note log verdict fexpect
# shellcheck source=/dev/null
source "$GUI_TOOLS/gui-checks.spec"
steps
