English | [简体中文](README.zh-CN.md)

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

**Showcase:** screenshots and short clips of each feature, taken on the tablet itself, are collected in
[docs/STATUS.md#showcase](docs/STATUS.md#showcase) (the first native captures are scheduled for the next test boot).

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
| `skills/`, `tooling/repro/` | the agent guide for another unit and its kit: pins of Apple's inputs, self-fetch, root rebuild checks, host check |
| `logs/`, `artifacts/ARTIFACTS.md` | committed verification logs; SHA-256 of built artifacts and inputs |
| `private/`, `out/` | ignored: keys, per-unit calibration; build outputs |

## Build

The whole path for a new unit (requirements, inputs, build, RAM-boot validation, guarded install, rollback, checks) is
[skills/install-piano-macos/SKILL.md](skills/install-piano-macos/SKILL.md), written for a coding agent. This section is the
summary; the skill is the reference.

### Host requirements

- **A Mac with Apple silicon (M1 or later). Intel Macs are not supported.** Xcode 27's compiler and linker and macOS 27's own
  tools are arm64-only, and two build steps run arm64e programs they compile. The evidence is in the skill (§1.1).
- Tested: macOS 27.2.1 with **Xcode 27.2** (the app, not only the Command Line Tools: the kernel needs the iphoneos SDK and
  `Kernel.framework`; the kernel links with Xcode's `ld-classic`). Homebrew at `/opt/homebrew` with the packages the skill lists
  (§1.2), `fastboot`, Python 3.11 or later, about 90 GiB free disk, `sudo` in an attached Terminal for the root image steps.
- `bash tooling/repro/check-host.sh` reads the host and prints `ok` / `WARN` / `FAIL` / `GAP` per requirement; continue only with
  `0 FAIL`. Run from a native arm64 shell, not under Rosetta.

### Sources and pins

- This repository plus the pinned dependencies of `unixnu.lock` / `feeds.toml` (UniXNU core, Mu-Silicium, piano-uefi,
  DarwinBoot, the SoC module) and the k27 kernel tree (`PIANO_K27_TREE`, in the environment or in `~/.config/piano/env`; pinned
  in `unixnu.lock`).
- Apple's files are never distributed. `tooling/repro/fetch-ipsw.py` fetches the pinned 27.3 IPSW's members from Apple's CDN and
  the AEA keys from Apple, and checks every file against `tooling/repro/pins.json`.
- Some inputs are still local to the original setup (the k27 kernel tree, the base trust bundle, the Data seed, the Turnip tree,
  the write tooling's single-unit pins): the skill's "Known gaps" lists each with its options.

### Build steps

```sh
bash tooling/repro/fetch-and-build.sh --dry-run   # every command it would run; nothing fetched, built or written
bash tooling/repro/fetch-and-build.sh             # host, inputs, systemos, root, kernel, fd, image (or one step by name)
bash tooling/repro/fetch-and-build.sh --check     # each step's outputs against the pins (no network)
```

The steps it runs, also usable one by one:

```sh
bash tooling/k273/prepare-k27-tree.sh          # the 27.3 kernel tree and its relinked kernel; prints K273_KERNEL's path
bash tooling/firmware/build-fd.sh               # the UEFI FD from the unixnu.lock checkouts
K273_KERNEL=<path printed above> bash tooling/k273/build-k273-image.sh   # kexts -> kernel collection -> ADT -> RAM images
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

- **Kexts and the kernel collection:** the KC links only this repository's kexts (`configs/k27-native-kexts.txt`);
  `tooling/k27/check-native-kexts.py` enforces it before and after the link. Pass line in the KC log:
  `native kext rule: ... all this repository's`.
- **The root image is generated locally, never shipped.** The 20.9 GB root is the IPSW's OS volume (byte for byte, pinned) plus a
  SystemOS window rebuilt by `tooling/k273/make-systemos.sh` and `make-root-image.sh`, checked by `tooling/repro/verify-root.py`
  against the pinned file-level manifest (skill §3.1).
- Only the 27.3 line is built; the retired 27.0.1 path (`tooling/k27/build-native-image.sh`) is not used. Root placement: a new
  unit starts with the root on `linux_data` (`K273_ROOT_PART=linux_data`, skill §3.2); the reference unit's installed ESP uses
  `build-k273-esp.py --root-part linux_rootfs` ([docs/persistent-boot.md](docs/persistent-boot.md) §13.8).

### Size limit of a RAM image

A RAM boot image must be **at most 250,000,000 bytes**. ABL loads the whole image into the UEFI's 256 MiB kernel region; a
294,952,960-byte image stayed in fastboot after the download, while 247,767,040-byte images boot
([docs/k273-line.md](docs/k273-line.md) §4). The cap is `IMAGE_MAX_BYTES` in `tooling/bootimg/build-ram-images.py` (which
shrinks the payload's free space when an image would pass it) and is checked again by `tooling/bootimg/verify-ram-images.py`
(both read `PIANO_IMAGE_MAX_BYTES`; do not raise it).

### RAM boot before any install

Every image is first booted with `fastboot boot` (a RAM boot writes no partition) and must pass there before anything is written
to the disk: the console reaches XNU, the USB console answers, the boot returns to ABL by its reset guard, and for a desktop image
the login window, touch, keyboard cover, ssh, Wi-Fi and battery read correctly (skill §4). Installs then go only through
`tooling/persist/maint-session.py`, with backups, read-back checks and a rollback for each step
([docs/persistent-boot.md](docs/persistent-boot.md), [docs/install-esp-v3.md](docs/install-esp-v3.md)).

More: boot arguments and debugging without a screen in [docs/bringup-debugging.md](docs/bringup-debugging.md) (§4 for what is read
from the k27 tree); what works today in [docs/STATUS.md](docs/STATUS.md).

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
- [skills/install-piano-macos/SKILL.md](skills/install-piano-macos/SKILL.md): building and installing on another unit (agent guide).
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
