# Bluetooth results, 2026-11-17

The bt-path2 RAM test booted without the earlier sysctl panic and published
its serial endpoint and Bluetooth service. It stopped before controller setup:
firmware mount returned -3; the public probe reported power off and no discovery
callbacks. The original BT1 power, shared-rail and UART loopback results remain
valid; bt-path2 returned before attempting those operations.

Host analysis reproduced the firmware rejection using a saved Bluetooth
partition: its 8 MiB physical prefix declares a 64 MiB FAT16 volume. All 27
firmware files fit inside the physical partition and match the third-party
upstream's size/hash pins. A separate runtime activation gate read the clipped
user-space boot arguments and therefore missed the level-5 argument.

The bt-path3 candidate uses the existing shared FAT parser with an explicit
bounded-prefix mode, reports firmware errors accurately, and activates the
runtime from the kernel's level-5-only sysctl name. Host tests cover physical
bounds, corrupt chains, allocation/read failures, strict FAT16/FAT32 mounts and
runtime activation/failure ordering. Controller setup, powered discovery and
the Bluetooth UI still require tablet verification.
