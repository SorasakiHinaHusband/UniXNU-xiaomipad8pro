#!/bin/bash
# Runs on the installed tablet. TCC attribution and csreq follow the harness tcc-seed.sh.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
dir=${BASH_SOURCE[0]%/*}
op=$1
boot=$2
uid=$3
shift 3
[[ $EUID == 0 && $uid =~ ^[0-9]+$ && $uid -ge 501 && $dir =~ ^/private/var/run/piano-gui\.[a-zA-Z0-9]{8}$ ]]
[[ $(sysctl -n kern.bootsessionuuid) == "$boot" ]]
args=$(sysctl -n kern.bootargs)
awk '{ for (i=1; i<=NF; i++) if ($i ~ /^data-persist=/) { n++; ok=($i=="data-persist=2") } }
     END { exit !(n==1 && ok) }' <<< "$args"

db='/Library/Application Support/com.apple.TCC/TCC.db'
wrapper=/usr/libexec/sshd-keygen-wrapper
where="service='kTCCServiceAccessibility' AND client='$wrapper' AND client_type=1"
sql() { sqlite3 -batch -bail -cmd '.timeout 5000' "$db" "$1"; }
restart_tccd() {
    local pid
    pid=$(pgrep -u "$uid" -x tccd) || return 1
    [[ $pid =~ ^[0-9]+$ ]] && kill -KILL "$pid"
}

if [[ $op == cleanup ]]; then
    if [[ -f "$dir/seed-owner" ]]; then
        read -r stamp req < "$dir/seed-owner"
        [[ $stamp =~ ^[0-9]+$ && $req =~ ^[0-9a-f]+$ ]]
        deleted=$(sql "DELETE FROM access WHERE $where AND last_modified=$stamp
             AND auth_value=2 AND auth_reason=4 AND csreq=X'$req'; SELECT changes();")
        if [[ $deleted == 1 ]]; then restart_tccd; fi
    fi
    unlink "$dir/gui"
    if [[ -f "$dir/seed-owner" ]]; then unlink "$dir/seed-owner"; fi
    unlink "$dir/session.sh"
    rmdir "$dir"
    echo GUI_CLEANUP_OK
    exit
fi

[[ $(stat -f %u /dev/console) == "$uid" ]]
pgrep -u "$uid" -x Finder >/dev/null
launchctl print "gui/$uid" >/dev/null
registry=$(ioreg -n Root -d1 -w0)
[[ $registry == *IOConsoleUsers* && $registry != *'"CGSSessionScreenIsLocked"=Yes'* ]]
asuser() { launchctl asuser "$uid" /usr/bin/sudo -n -u "#$uid" -- "$dir/gui" "$@"; }
case $op in
    seed)
        # The RAM seed also sets Camera rows. On installed Data, touch only the Accessibility row,
        # preserve an existing policy, and remember the exact temporary row for cleanup.
        [[ -f $db && ! -L $db && ! -e "$dir/seed-owner" ]]
        [[ $(sql "SELECT count(*) FROM access WHERE $where;") == 0 ]] ||
            { echo GUI_TCC_EXISTING_POLICY; exit 2; }
        reqfile=$(mktemp "$dir/req.XXXXXXXX")
        trap 'unlink "$reqfile"' EXIT
        printf 'identifier "com.apple.sshd-keygen-wrapper" and anchor apple\n' | csreq -r- -b "$reqfile"
        req=$(xxd -p "$reqfile" | tr -d '\n')
        [[ $req =~ ^([0-9a-f]{2})+$ ]]
        stamp=$(date +%s)
        (umask 077; printf '%s %s\n' "$stamp" "$req" > "$dir/seed-owner")
        changed=$(sql "BEGIN IMMEDIATE;
            INSERT INTO access (service, client, client_type, auth_value, auth_reason, auth_version, csreq, flags, last_modified)
            SELECT 'kTCCServiceAccessibility', '$wrapper', 1, 2, 4, 1, X'$req', 0, $stamp
            WHERE NOT EXISTS (SELECT 1 FROM access WHERE $where);
            SELECT changes(); COMMIT;")
        [[ $changed == 1 ]] || { unlink "$dir/seed-owner"; exit 2; }
        restart_tccd
        echo GUI_TCC_SEEDED
        ;;
    wifi-state|wifi-disassociate)
        "$dir/gui" "$op" "$@"
        ;;
    display-sample)
        asuser "$op"
        ioreg -r -c PianoBacklight -l -w0
        ;;
    ax-trust|wifi-menu|wifi-list|wifi-find|wifi-click|wifi-off|wifi-on|display-open|display-read|display-set|cursor)
        asuser "$op" "$@"
        ;;
    *) exit 2 ;;
esac
