#!/bin/bash
# Opened with "open -a Terminal" by the hands-free LE test, so the peripheral runs with Terminal's Bluetooth permission (over ssh,
# also as root, CoreBluetooth reports unauthorized). Its arguments are in le-peripheral.request next to it: SECONDS NONCE LOG.
D=$(cd "$(dirname "$0")" && pwd)
read -r SECONDS_ NONCE LOG < "$D/le-peripheral.request"
"$D/le-peripheral" "$SECONDS_" "$NONCE" > "$LOG" 2>&1
echo "exit=$?" >> "$LOG"
exit 0
