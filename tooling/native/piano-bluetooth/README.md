# Piano Bluetooth path A runtime

This is the version-locked user-space half of the native Bluetooth path for
macOS 27.3 (26C5035r). It needs PianoBluetooth's level-5 POSIX transport,
`/dev/cu.BLTH`, and the IOBluetoothFamily singleton retained by the collection.
It does not provide controller firmware or pair a device.

`python3 tooling/native/piano-bluetooth/build-runtime.py --bluetoothd ORIGINAL --output EXTERNAL_DIRECTORY`

The original executable and every generated artifact stay outside the
repository. The builder checks the thin arm64e SHA and UUID before patching,
keeps the daemon's entitlements and identifier, and signs all executable
artifacts. It builds but never runs the daemon or mount helper on the host.
`report.json` records hashes and CDHashes for the test image's trust cache.

The generated `runtime/bin/`, `runtime/lib/` and `runtime/runtime.sha256` are installed, root-owned,
under `/private/var/piano/bluetooth-path1`. The generated `runtime/helpers/` contains the mount helper and probe. The helper
`piano-bt-runtime-mount` is installed in `/usr/local/libexec`; it is signed with
`com.apple.private.bindfs-allow`. It validates the four runtime files, mounts
the library directory before the executable directory as read-only bindfs
union views, and verifies the mounted files and lower `dyld`/`notifyd` entries.
The installed `piano-bluetooth-start` script only activates on `piano-bt=5` and then restarts the existing
Apple launchd services. The sealed system files and their job definitions stay
on the lower view. All staging and trust-cache inclusion belong to the shared
image recipe, not to this builder.

`piano-bt-probe --scan 20` reports local controller power and bounded
classic/LE discovery counts (20 seconds per radio, with an overall IPC deadline). It never connects, pairs, reads keys, logs names,
logs addresses, or prints advertisement data. A TCC refusal is reported as a
client-authorization failure rather than an RF result. `--status` queries the
ObjC host-controller power state only; the existence of the default controller
object alone does not establish that the controller is available.
