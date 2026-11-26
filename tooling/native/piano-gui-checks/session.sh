#!/bin/bash
# Runs on the installed tablet. TCC attribution and csreq follow the harness tcc-seed.sh.
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
dir=${BASH_SOURCE[0]%/*}
op=$1
boot=$2
uid=$3
shift 3
[[ $EUID == 0 && $uid =~ ^[0-9]+$ && $uid -ge 501 && $dir =~ ^/private/var/run/piano-gui\.[[:xdigit:]]{32}$ ]]
[[ $(sysctl -n kern.bootsessionuuid) == "$boot" ]]
args=$(sysctl -n kern.bootargs)
awk '{ for (i=1; i<=NF; i++) if ($i ~ /^data-persist=/) { n++; ok=($i=="data-persist=2") } }
     END { exit !(n==1 && ok) }' <<< "$args"

db='/Library/Application Support/com.apple.TCC/TCC.db'
wrapper=/usr/libexec/sshd-keygen-wrapper
where="service='kTCCServiceAccessibility' AND client='$wrapper' AND client_type=1"
sql() { sqlite3 -batch -bail -noheader -separator ' ' -cmd '.timeout 5000' "$db" "$@"; }
restart_tccd() {
    local pid
    pid=$(pgrep -u "$uid" -x tccd) || return 1
    [[ $pid =~ ^[0-9]+$ ]] && kill -KILL "$pid"
}

if [[ $op == seed || $op == cleanup ]]; then
    # A timed-out SSH command can still be finishing the seed on the tablet.
    exec /usr/bin/lockf -k -t 20 "$dir/session-lock" "$dir/session.sh" "$op-locked" "$boot" "$uid"
fi

if [[ $op == cleanup-locked ]]; then
    restart_failed=0
    if [[ -s "$dir/seed-owner" ]]; then
        read -r row stamp req < "$dir/seed-owner"
        [[ $row =~ ^[1-9][0-9]*$ && $stamp =~ ^[0-9]+$ && $req =~ ^([0-9a-f]{2})+$ ]]
        deleted=$(sql "DELETE FROM access WHERE $where AND rowid=$row AND last_modified=$stamp
             AND auth_value=2 AND auth_reason=4 AND auth_version=1 AND flags=0
             AND csreq=X'$req'; SELECT changes();")
        if [[ $deleted == 1 ]]; then restart_tccd || restart_failed=1; fi
    fi
    rm -f "$dir/gui" "$dir/seed-owner" "$dir/session.sh" "$dir/session-lock" "$dir"/req.*
    rmdir "$dir"
    [[ $restart_failed == 0 ]]
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
    seed-locked)
        # The RAM seed also sets Camera rows. On installed Data, touch only the Accessibility row,
        # preserve an existing policy, and remember the exact temporary row for cleanup.
        [[ -f $db && ! -L $db && ! -e "$dir/seed-owner" ]]
        reqfile=$(mktemp "$dir/req.XXXXXXXX")
        trap 'unlink "$reqfile"' EXIT
        printf 'identifier "com.apple.sshd-keygen-wrapper" and anchor apple\n' | csreq -r- -b "$reqfile"
        req=$(xxd -p "$reqfile" | tr -d '\n')
        [[ $req =~ ^([0-9a-f]{2})+$ ]]
        stamp=$(date +%s)
        # Close the root-only ownership receipt before committing. Only an inserted
        # row produces a receipt, including when another writer already has a policy.
        umask 077
        sql <<SQL
            BEGIN IMMEDIATE;
.output $dir/seed-owner
            INSERT INTO access (service, client, client_type, auth_value, auth_reason, auth_version, csreq, flags, last_modified)
            SELECT 'kTCCServiceAccessibility', '$wrapper', 1, 2, 4, 1, X'$req', 0, $stamp
            WHERE NOT EXISTS (SELECT 1 FROM access WHERE $where)
            RETURNING rowid, last_modified, lower(hex(csreq));
.output stdout
            COMMIT;
SQL
        [[ -s "$dir/seed-owner" ]] || { echo GUI_TCC_EXISTING_POLICY; exit 2; }
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
    ax-trust|wifi-menu|wifi-find|wifi-click|wifi-off|wifi-on|display-open|display-read|display-set|cursor)
        asuser "$op" "$@"
        ;;
    *) exit 2 ;;
esac
