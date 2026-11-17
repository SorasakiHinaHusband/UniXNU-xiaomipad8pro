#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang. All rights reserved.
set -eu
# Match the parsed final value, rather than accepting an earlier duplicate arg.
level=$(/usr/sbin/sysctl -n kern.bootargs | /usr/bin/awk '{for(i=1;i<=NF;i++)if($i~/^piano-bt=/){split($i,a,"=");level=a[2]}} END {print level}')
[ "$level" = 5 ] || exit 0
/usr/local/libexec/piano-bt-runtime-mount
# Hardware setup includes a firmware-partition wait; do not spend the daemon's
# serial-open timeout before the transport can answer. This read has no power
# side effect. Each kernel wait is bounded at 30 seconds.
i=0
ready=0
while [ "$i" -lt 4 ]; do
    boot=$(/usr/sbin/sysctl -n kern.qbt_boot 2>/dev/null || printf unavailable)
    if [ "$boot" = 0 ]; then ready=1; break; fi
    i=$((i + 1))
    /bin/sleep 1
done
[ "$ready" = 1 ] || { printf '%s\n' 'piano-bt-path1 runtime: transport_not_ready=1'; exit 1; }
# The stock job definitions retain Apple's mach services and sandbox policy.
/bin/launchctl kickstart -k system/com.apple.BlueTool
/bin/launchctl kickstart -k system/com.apple.bluetoothd
printf '%s\n' 'piano-bt-path1 runtime: stock services restarted on verified views'
