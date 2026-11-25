# UniXNU-piano

Native macOS 27 on the Xiaomi Pad 8 Pro (`piano`, Snapdragon 8 Elite SM8750), as a [UniXNU](https://github.com/UniXNU/UniXNU)
device repository: https://github.com/lenzhang/UniXNU-xiaomipad8pro (formerly lenzhang/UniXNU-piano; the local
checkout keeps the name UniXNU-piano). The 27.3 line's k273 XNU build (the k27 source tree with `patches/k27` 0053-0074 and
`patches/k273`) runs as the HLOS at EL1, with
Gunyah staying at EL2. Boot chain: ABL → BootShim → Mu-Silicium UEFI (the FD, piano-uefi's `pianoPkg` with this repository's
deltas) → the UniXNU payload and menu → DarwinBoot → XNU. UniXNU core, Mu-Silicium, piano-uefi, DarwinBoot and the SoC module
are pinned dependencies (`unixnu.lock`, `feeds.toml`), not forks.

Related public repositories: [UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) is a placeholder for the
SM8750 pieces to be shared later (the code still lives here, and the build pins another SM8750 module); [veil](https://github.com/lenzhang/veil)
is the frozen archive of the earlier Linux + Gunyah guest path. The map of repositories, local trees and rules:
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md).

## State (2026-11-25)

Native macOS **27.3 (26C5035r)** boots on the tablet from power-on as well as from a RAM image. The current daily
install is **v13.1y12k** (2026-11-25). The machine identifies as Mac17,11, shown as "Xiaomi Pad 8 Pro" with a
"Snapdragon 8 Elite" chip. Root is on `linux_rootfs`; the owner's Data store is on `linux_data`. High Power is the daily
Energy Mode. The Android-aligned CPU/GPU frequency curve and PD buck charging (9 V) are on by default; the charge pump
stays off.

What works on the installed system today: boot from disk with the owner's data, the display at 60 Hz, the native touch
screen, the keyboard cover and its trackpad, Wi-Fi (join at boot, off/on, rejoin after a link loss), speakers, battery
percentage, 5 V and 9 V PD charging with the 80 % hold, automatic brightness, Metal on the Adreno 830 with paged GPU
memory, Photo Booth and camera recording, hardware video decode in QuickTime and Safari (H.264 up to 1080p), and Safari
WebGL 2 and WebGPU. Not yet: Bluetooth pairing, lid and power-button sleep on the installed system, Chrome GPU
acceleration, charge-pump fast charging, 120 Hz in daily use, USB host devices and external displays.

**The full per-feature table, with the build and date each result was verified: [docs/STATUS.md](docs/STATUS.md).**
Today's tablet results: [docs/results-2026-11-25.md](docs/results-2026-11-25.md).

A USB-C debug console (`package/PianoDwc3`, `tooling/native/usb-console.py`) and ssh over the cable are available;
the monitor's `reset fastboot` returns the tablet to ABL without buttons. Everyday development uses `fastboot boot`
RAM images, which take priority over the installed copy.

Per-driver pages, boot arguments and test levels: [docs/drivers/README.md](docs/drivers/README.md) and
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md). Using, updating and recovering the installed system:
[docs/USING.md](docs/USING.md). Earlier public write-ups include
[docs/results-2026-11-19.md](docs/results-2026-11-19.md) and
[docs/results-2026-11-17.md](docs/results-2026-11-17.md).

## Layout

| Path | What |
| --- | --- |
| `patches/k27/` | the k27 kernel patches 0053-0074 (no 0072) on top of the k27 source tree (xnu-12377 + the baseline series 0001-0052) |
| `patches/k273/` | the 27.3 line's kernel series on top of `patches/k27` |
| `patches/k27-kc/` | the KC builder's native placement (best fit, relocation-free constants in `__TEXT`), applied by hand to the k27 tree ([docs/kc-space.md](docs/kc-space.md)) |
| `patches/xnu/` | the device series of the UniXNU macOS 27.3 line (empty: none yet) |
| `package/` | the native kexts; each is inert without its boot argument |
| `configs/` | the native kext list (`k27-native-kexts.txt`), the ADT identity and display node (`k27-adt.toml`, `k27-gop-display.toml`), the 27.3 line's kernel config |
| `dts/piano.dts` | piano's hardware for the ADT |
| `tooling/` | `k273/build-k273-image.sh` (the 27.3 kexts → kernel collection → ADT → RAM images, `K273_KERNEL`), `k273/build-k273-esp.py` (the installed 27.3 ESP), `kc/` (the KC build), `adt/`, `bootimg/` (RAM images and their verification), `native/` (the test loop, the USB console, input), `persist/` (the installed copy), `firmware/` (the FD) |
| `firmware/` | deltas to piano-uefi's `pianoPkg`, Mu-Silicium and UniXNU |
| `docs/` | bring-up notes, driver pages (`drivers/`), plans and evidence |
| `logs/`, `artifacts/ARTIFACTS.md` | committed verification logs; SHA-256 of built artifacts and inputs |
| `private/`, `out/` | ignored: keys, per-unit calibration; build outputs |

## Build

Inputs: the k27 tree, a local kernel tree the build only reads (its place is `PIANO_K27_TREE`, in the environment or in
`~/.config/piano/env`), the pinned UEFI checkouts of `unixnu.lock`, and the OS image inputs of `out/inputs/` (recorded in
`artifacts/ARTIFACTS.md`, never committed).

```sh
tooling/k273/prepare-k27-tree.sh                             # the 27.3 tree and its relinked kernel; prints K273_KERNEL's path
tooling/unixnu.sh build firmware --skip-preflight           # the FD
K273_KERNEL=<path printed by prepare-k27-tree.sh> tooling/k273/build-k273-image.sh
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

Only the 27.3 line is built. The retired `tooling/k27/build-native-image.sh` 27.0.1 path is not used. **Layout note
(2026-11-14):** the root is now on `linux_rootfs` and Data on `linux_data`; `build-k273-image.sh`'s `K273_ROOT=k273` default
still emits the superseded `native-ufs-root-part=linux_data`, so do not use that default for the current layout. The installed
ESP recipe uses `build-k273-esp.py --root-part linux_rootfs` ([docs/persistent-boot.md](docs/persistent-boot.md) §13.8).

Details, the boot arguments and what is read from the k27 tree: [docs/bringup-debugging.md](docs/bringup-debugging.md) §4.

## Boot and test

Everyday development uses RAM boots: `fastboot boot` of a RAM image, which takes priority over the installed copy, with an
unattended loop that boots a test, resets and reads the test's log back (`tooling/native/native-loop.sh`,
[docs/bringup-debugging.md](docs/bringup-debugging.md) §3). The installed copy ([docs/persistent-boot.md](docs/persistent-boot.md))
writes only the regions that document lists, after verified backups of each, with a read-back check after every write and a
rollback for each step; it never writes firmware partitions. Do not flash boot or firmware partitions by hand.

## The native kext rule

The native kernel collection links only this repository's kexts (`configs/k27-native-kexts.txt`). `tooling/k27/check-native-kexts.py`
enforces it on every build, before and after the link: no descriptor from elsewhere, none of the VM-era paravirtual kexts.

## Where to read next

- [docs/STATUS.md](docs/STATUS.md): what works, per feature, with the build and date it was verified.
- [docs/USING.md](docs/USING.md): using, updating and recovering the installed system.
- [docs/drivers/README.md](docs/drivers/README.md): every driver, its levels and its state.
- [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md): repositories, local layout, hard rules.
- [docs/bringup-debugging.md](docs/bringup-debugging.md): the boot chain, debugging without a screen, the test loop, rebuilding.
- [docs/BACKLOG-hardware.md](docs/BACKLOG-hardware.md): the hardware still to bring up.
- [docs/kc-space.md](docs/kc-space.md): the kernel collection's room for native kexts, the run-time firmware loader.

## Licence and contents

Code here is BSD-2-Clause-Patent, Copyright (c) 2026 Leo Zhang, unless a package's `LICENSE`, a file's SPDX header or an
`UPSTREAM` file says otherwise (third-party parts keep their own holders and licences).
Vendor firmware, partition images and Apple payloads (kernel collections, trust caches, IPSW-derived files) are never in this
repository; neither are secrets, keys, per-unit calibration, local network or storage details, or machine names.
