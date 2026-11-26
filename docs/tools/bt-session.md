# Bluetooth keyboard and mouse owner session

## Read-only analysis before the session

On November 20, LE Connection Complete succeeded and bluetoothd read the peer version and features. It then waited in
`LE_READY_WAIT_FOR_LENGTH` until a supervision timeout (HCI reason `0x08`) or a local cancel. GATT, SMP and HID had yet to start.
The November 26 installed results confirmed controller startup and the LE event-mask change. The change makes Data Length Change
and PHY Update Complete visible to bluetoothd when the controller emits them. The first keyboard attempt should test that change
on the daily configuration. [Driver notes §8.5](../drivers/bluetooth.md#85-le-link-ready-2026-11-21-host-tested-tablet-result-pending)
· [November 26 results](../results-2026-11-26.md#v131y12m-installed)

The November 22 trace gives a second lead. About 90% of LE reports were at or below −101 dBm, stronger reports clustered near Wi-Fi
scans, and 121 Classic inquiry completions yielded zero results. The shared 2.4 GHz front end is a hypothesis. A matched retry
with the same peripheral, pairing channel and position is the closest comparison available in this session.
[Driver notes §8.6](../drivers/bluetooth.md#86-radio-health-bt-radio1-2026-11-26-host-tested-tablet-result-pending)

| Hypothesis | Evidence to look for in the marked attempt |
|---|---|
| The keyboard stopped advertising, was on the wrong channel, or reconnected elsewhere | Owner confirms the pairing LED and isolates other hosts. With radio reports still arriving, no matching row and no LE Create Connection after the click put the failure before initiation. Aggregate RSSI alone cannot identify the peer. |
| Wi-Fi activity is starving Bluetooth, or the Bluetooth RF path is weak | Compare `piano-bt-radio-le-rssi`, `piano-bt-radio-le-reports`, `piano-bt-radio-classic-results` and `piano-bt-radio-inquiries` deltas with Wi-Fi on and off at the same distance. A stronger histogram plus a successful link off Wi-Fi supports coexistence. The same weak pattern in both windows leaves antenna, controller RF and peer behavior open. |
| Controller initiation or first connection events fail | Find LE Create Connection or Extended Create Connection, its Command Status, then LE Connection Complete. A rejected command, no completion, a nonzero completion such as `0x3e`, and a successful completion followed by Disconnection Complete reason `0x08` are different boundaries. Check requested and negotiated interval, latency and supervision timeout before attributing `0x08` to the link-ready wait. |
| The mask change is missing after daemon startup or reset | Require `piano-bt-h4-le-masks-patched` and the boot console's `LE event mask ...101f -> ...185f` (or another mask with bits 6 and 11 set), plus HCI `0x2001` Command Complete status `0x00`. A host-side trace records the mask before PianoBt rewrites it. |
| Length or PHY procedure is absent, or bluetoothd stays in link-ready despite events | For the successful connection handle, compare LE Remote Features Complete, Data Length Change (`0x07`), PHY Update Complete (`0x0c`), daemon `LE_READY_WAIT_FOR_LENGTH`/ready messages, and the first ATT/SMP traffic. The optional link-ready assist has separate `piano-bt-h4-lerdy-injected-*` counters. A missing controller event with the mask set can also mean the peer never negotiated a change. |
| Transport corruption or a controller/daemon reset interrupted the attempt | Inspect `piano-bt-host-ready`, H4 backpressure/resync/oversized counters, console `TX-timeouts` or `H4 TX failed`, and the daemon PID or HCI Reset. A counter drop is a discontinuity. |
| The link is ready but security or HID fails | First ATT/SMP traffic moves the diagnosis past the earlier stall. Check Encryption Change, daemon pairing/security/key-store errors, then HID publication and the actual typing, clicking and scrolling checks. A mouse or keyboard using Classic has its own inquiry, Connection Complete and HID path. |
