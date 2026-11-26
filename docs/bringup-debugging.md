# Native bring-up on piano: how to boot, debug and iterate without watching the screen

## Current baseline (2026-11-27)

The reference tablet booted **v13.1y12q** from disk on 2026-11-26. It runs native macOS 27.3 (26C5035r) as Mac17,11 at
EL1, with Gunyah at EL2. The v5 BootShim and UEFI firmware are on `boot_a`. The ESP's `macos-27.3` entry supplies the
kernel collection, ADT, trust cache, Data seed and boot arguments. The 13 GiB XNU window measured 13310 MiB on v13.1y12l.
[27.3 line](k273-line.md#current-line-2026-11-27) · [Installed profile](USING.md#2-the-installed-profile)

The macOS root uses the OS and SystemOS windows on `linux_rootfs`. The 200 GiB direct Data store is on `linux_data`
at LBA 71859642. Data mounts in serial mode and switches to concurrent mode after mount. Write-back and queue modes
stay off. USB debug network TX flow control is on by default and passed 66/66 exact transfers in qualification.
[November 26 results](results-2026-11-26.md) · [Data concurrency and durability](drivers/data-store-m4-m5.md)

Daily display stays at 60 Hz with Night Shift routing, native panel sleep and refresh switching off. Fixed 120 Hz and
144 Hz have RAM scanout results. [Support status](STATUS.md) · [Hardware backlog](BACKLOG-hardware.md)

Routine tests use RAM boots. Partition changes follow the approved maintenance procedure with verified backups and
read-back. Firmware and calibration partitions and all `_b` partitions stay read-only. The in-RAM `piano-uefi` device
handles the debug `oem` commands. Avoid `systemctl isolate` on the tablet.
[Project rules](PROJECT-MAP.md#6-hard-rules) · [Install design](persistent-boot.md) · [Operator's guide](USING.md)

## 1. The boot chain, and what each stage needs

```text
Disk: ABL → BootShim + v5 FD on boot_a → UniXNU.efi → macos-27.3 on the ESP
 RAM: ABL → BootShim + FD A in the RAM image → UniXNU.efi → RAM payload
Both: DarwinBoot.efi → native k27 XNU at EL1 (Gunyah at EL2)
```

| Stage | What to check | Failure signal |
|---|---|---|
| ABL → BootShim on a RAM boot | The ARM64 header has `text_offset 0`, `flags 0xa` and an `image_size` covering BootShim + FD. [RAM image builder](../tooling/bootimg/build-ram-images.py) | In the early bring-up, ABL accepted the download but returned to fastboot about 80 s later. Subsequent boots in that session reported `Load Error`. |
| UEFI drivers | Use FD A, built with the bundled piano-uefi DXEs. [FD builder](../tooling/firmware/build-fd.sh) | The early FD B built with the unit's OS3.0.303 DXEs reset before XNU. |
| UEFI → UniXNU RAM payload | SEC preserves the ABL device tree argument in the entry-argument HOB. The RAM payload takes precedence over the ESP. [SEC patch](../firmware/patches/mu-silicium/0001-sec-entry-arg-hob.patch) · [RAM precedence patch](../firmware/patches/unixnu/0001-UniXNULib-config-prefer-the-RAM-payload.patch) | `UNIXNURAMDISK: no entry argument HOB` if entry argument x0 was lost. An FD without the precedence patch can select the ESP configuration. |
| DarwinBoot → XNU | The linear BGRA GOP comes from SimpleFbDxe. [Linear GOP patch](../firmware/patches/0002-pianoPkg-linear-gop-for-darwinboot.patch) | With the old Blt-only GOP, the menu redraws after `DARWINBOOT: boot refused: Unsupported`. |
| DarwinBoot RAM window | The 27.3 ADT requests `0x33fe00000` bytes. The FD builder stamps the FD that supports it. Check the console's `NativeRamBlock: RAM window` line for 13310 MiB. [27.3 ADT](../configs/k273-adt.toml) · [DarwinBoot window patch](../firmware/patches/darwinboot/0001-DarwinBoot-window-up-to-13-GiB-shrink-to-the-largest-run.patch) | Older FD builds refuse requests above 8 GiB. A short allocation produces `taken (largest run)`, and a missing run produces `NO WINDOW`. |
| XNU CPUs | Boot with all eight ADT CPUs using `sheng-smp=1`. [RAM image builder](../tooling/bootimg/build-ram-images.py) | The early `cpus=1` test aborted in `recount_switch_thread+0x84`. |
| XNU root and Data | NativeUfs publishes both SHNGROOT windows. Check `native-ufs-root-part`, `data-store-part` and the store state against the intended profile. [UFS driver](drivers/ufs.md) · [Data store](data-store.md) | Repeated `Waiting on <dict> IOMedia` points to root matching. The store-state line distinguishes a resumed store from volatile Data. |
| UFS controller | The driver redirects SID 0x60 to the bypass bank and exits Hibern8 for UFS link bring-up. [UFS driver](drivers/ufs.md) | The early failures were `conflicting UFS stream mapping` and `FAILED at NOP OUT`. |

In the RAM image menu, volume keys move and power selects. The RAM builder defaults to `PIANO_MENU_TIMEOUT=0` and
`PIANO_MENU_DEFAULT=macos-8cpu`. A timeout of `-1` waits for a key. [RAM image builder](../tooling/bootimg/build-ram-images.py)

## 2. Seeing what happened without a camera

| Stage or symptom | Evidence and tool |
|---|---|
| ABL download or firmware handoff | During a RAM boot the USB sequence is ABL fastboot → disconnect → `piano-uefi` fastboot → disconnect as DarwinBoot hands off to XNU. Before handoff, the in-RAM UEFI exposes the console (`oem log`), panel (`oem screenshot`) and the `Piano_Log` DEBUG buffer (`oem dump`, address `D7F00000`, length `100000` in hex). The DEBUG buffer includes driver loads and the `UNIXNU:` and `UNIXNURAMDISK:` lines. |
| Live XNU console and monitor | PianoDwc3 device mode (`piano-usb=2`) enumerates as `1209:0001` with CDC-ACM console and shell ports. The host console tool replays the retained log first. The monitor's `args` command shows the merged boot arguments. [Host USB console](../tooling/native/usb-console.py) · [USB debug driver](drivers/usb-device-debug.md) |
| Console after a reset | Patches 0055 and 0058 mirror the console and replicate the ring and its header 11 times for majority decoding. With 0071 the last 64 KiB of the 7 MiB CVP region is reserved for the separate reset reason, and the console uses the first `0x6f0000` bytes. The decoder's `decode` mode reads an existing dump file on the host. [Mailbox decoder](../tooling/native/xnu-log.py) · [27.3 ADT](../configs/k273-adt.toml) |
| Reset intent or boot hang | Patch 0063 guards the installed boot, with the launchd exec check at 180 s and continued Gunyah watchdog and CPU liveness checks. Wake or halt tracing can be read from captured logs. Patch 0071 can retain reset intent when enabled, independently of the console ring. An absent record leaves the cause unknown. [Boot guard](boot-guard.md) · [Halt diagnosis](halt.md) · [Halt trace grader](../tooling/native/halt-trace-verdict.py) |
| Desktop is up but the panel is suspect | Use the native framebuffer capture for the pixels presented to scanout. [Framebuffer capture source](../tooling/native/gop-shot/gop-shot.c) · [Camera app checks](tools/cam-apps-check.md) |
| Panic addresses or a missing backtrace | The native kernel runs unslid. Resolve PC and LR against the matching unstripped kernel, and use the collection's `natives.json` for kext addresses. Patch 0073 adds opt-in panic text on the retained linear scanout during the panic hold. [Historical panic example](#6-historical-panic-investigations-2026-11-12-to-2026-11-13) · [Panic text diagnostics](../tooling/k27/piano-panic-text/README.md) |

For host controls and console capture, see the [operator's guide](USING.md#3-from-the-mac). Use the
[boot timeline tool](../tooling/native/boot-timeline.py) to derive timing from a recorded console and the
[boot-time guide](boot-time.md) for the historical timing probes.

### Historical console recovery (2026-11-10 to 2026-11-12)

The first native RAM boots recovered the log through Ubuntu's `/dev/mem` using aligned 8-byte accesses.
Ubuntu's `ramoops@a3500000` lost its log on reboot because the firmware uses that range for display demura.
The CVP mailbox retained enough data to decode after a warm reset, with about 4% bit loss measured in the first tests.
A hard power-off lost the mailbox. The November 12 XNU USB console added a live capture path.
[November 11 forensics](forensics-2026-11-11.md) · [November 12 results](results-2026-11-12.md)

## 3. The unattended loop

[The RAM test loop](../tooling/native/native-loop.sh) boots a test image and checks that ABL answers after it returns.
The RAM builder defaults to `PIANO_RESET_AFTER=45`. Patch 0056 schedules the PSCI reset with the Gunyah watchdog as
a backstop, and 0057 directs the reset to ABL fastboot. A test with an active `piano-reset-after` owns the watchdog
in place of the installed boot guard.

The loop defaults to keeping the live console capture and leaving the tablet in ABL after the test. A second boot for
a mailbox dump requires a matching `DUMP_GUARD_RECORD` with a qualified independent reset path that covers the ABL
handoff before USB appears. The loop also checks battery voltage before permitting a dump boot. Consult the
[RAM test procedure](USING.md#4-test-a-change-ram-boot-nothing-written) for the reference layout and seed handling.

### Historical two-boot capture (2026-11-11)

The original unattended loop ran the test, rebooted to ABL, then booted a separate dump image to pull the console
mailbox from its in-RAM UEFI. The dump image reset after one second in XNU. The earlier Ubuntu-based loop retired
when the macOS root moved to `linux_rootfs`. The maintenance RAM image remains part of the approved install procedure.
[November 11 results](results-2026-11-11.md) · [Install design](persistent-boot.md)

## 4. Rebuild after a change

```sh
tooling/k273/prepare-k27-tree.sh                             # current kernel: patches/k27 0053-0066 + patches/k273
tooling/unixnu.sh build firmware --skip-preflight          # FD (A: Binaries/piano -> the bundled set)
K273_KERNEL=<path printed by prepare-k27-tree.sh> tooling/k273/build-k273-image.sh
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

`tooling/build-slice1.sh` still calls the retired `tooling/k27/build-native-image.sh`; **do not use it for the current line**.
The native kext set is `configs/k27-native-kexts.txt` (§5). Only 27.3 images are built; see the `K273_ROOT` layout caveat in
[k273-line.md](k273-line.md) §4.

**The k27 tree** is the local XNU source tree: xnu-12377 + the baseline series 0001-0052 (pin `k27` of `unixnu.lock`),
with this repository's `patches/k27` (0053-0066) and, for the current 27.3 build, `patches/k273` applied by
`tooling/k273/prepare-k27-tree.sh`. The build only reads it, and no committed file names its place: set
`PIANO_K27_TREE` in the environment or as a line `PIANO_K27_TREE=/path` in `~/.config/piano/env`. Every script that needs it
goes through `tooling/k27/k27-tree.sh` (shell) or `tooling/k27/k27_tree.py` (Python) and stops with what to set when it is
missing. What the native build reads from it:
- the relinked 27.3 kernel (`out/k27/kernel-k273-...-native.macho`, selected by `K273_KERNEL`) and its objects' compile recipes
  (`out/x12377/build/xnu/k27-native-obj`), which every kext is compiled with (`tooling/k27/build-native-kext.py`);
- the generated headers under `out/x12377` (storage ABI, xnu's osfmk, IOKit hid/net/gfx) the kexts include;
- the current KC base, Apple's macOS 27.3 mac17j kernelcache (an Apple payload: never published); the 27.0.1 vma2 `out/kc/kc27.bin`
  base belongs to the retired line;
- the KC builder (`k27/kc-build27-piano.py`, `k27/kc27-layout.py`, `k27/segaddrs.sh`, `configs/kc-keep.txt`, `scripts/kc-build.py`,
  `kc_native.py`, `fileset_kc.py`, `ptrauth_disc.py`; its placement patches, applied there by hand: `patches/k27-kc`,
  [kc-space.md](kc-space.md)) and `scripts/gen-iomfb-abi.py` (its output is cached in `out/k27/iomfb-abi/`);
- `sources/UniXNU/drivers/DarwinBoot/Test/packed_ramdisk.py` (the Data seed's format, `tooling/k27/stage-soft-metal.py`);
- for the A830 GPU's userspace (`tooling/metal/build-turnip-a830.sh`): the Metal plugin's sources, its link stub and the prebuilt
  bridge (`overlay/metal`, `scripts/piano-frontend-patch.py`, `out/metal`), the virtgpu ABI header, and the UniXNU-turnip checkout
  next to the tree (`TURNIP_TREE`). Provenance: `tooling/metal/UPSTREAM`.

## 5. The native kext rule (2026-11-11)

A native boot loads only this repository's kexts: the native kernel collection links the set of `configs/k27-native-kexts.txt`
(descriptors in `tooling/k27/kext/`, sources in `package/`, objects in `out/native-kexts/`). `tooling/kc/build-kc-k27.sh`
refuses any other descriptor and checks the linked collection again (`tooling/k27/check-native-kexts.py`), so `PIANO_EXTRA_KEXTS`
/ `PIANO_KEXT_LIST` cannot bring a foreign kext in either; `tooling/persist/build-esp.py` refuses an ESP whose collection has
one. The check carries the VM-era paravirtual kexts as a denylist (their bundle identifiers, their legacy name prefix and the
virtio transport) and proves, before every collection, on synthetic descriptors that it still refuses them
(`check-native-kexts.py --self-test`; a build log says `native kext rule: N native kexts linked, none from outside this
repository`). Code native piano needs from the VM era is this repository's package with an `UPSTREAM` file, never the VM-era kext.

### 5.1 The VM-era kexts in native boots

The retired macOS 27.0.1 install was first brought up under the hypervisor, with paravirtual kexts for its virtual
devices. The tablet now runs macOS 27.3. What those kexts did in native boots (runs in `logs/native-runs/`, 2026-11-11), and what replaced them:

| Kext(s) natively | Effect | Natively now |
|---|---|---|
| the virtio display kext (as the display, or as NativeGopDisplay's nub), with the other virtio kexts | IOBSD matching did not finish in 30 s (`!BSD`), both UFS windows stayed busy, **the root never mounted**. The mechanism (k27 0059's busy dump): a window published before IOBSD had matched let AppleAPFSContainer::start take APFS's module lock while com_apple_filesystems_apfs::start waited for it; the virtio kexts' start timing triggered it | NativeUfs publishes only after IOBSD has matched; `package/NativeGopDisplay` copies into the GOP framebuffer and carries the `AppleParavirtGPU` nub itself |
| the stub GPU accelerator | WindowServer read its MetalPluginName, its type-0x4147 opens failed (`kIOReturnBusy`, slots reserved for iOS's backboardd) and WindowServer crash-looped every 30–40 s | `package/PianoA830` (the A830 GPU, its `AGXAccelerator`), or `package/NativeSoftGpu` for software Metal |
| the virtio Ethernet, audio, video, input and block kexts | start with no virtio device behind them; without them SystemOS matches in 0.5 s instead of 4 s | the root and Data from `package/NativeUfs` and `package/NativeRamBlock` (its virtio data store gone, NativeUfs's store window serves it, `data-persist=2`); the drivers of `docs/drivers/` |
| the virtio Ethernet kext's built-in `en0` without a device | turned out to be needed: TimeSync takes its clock ID from a built-in interface's MAC, and without one clocksyncd never answers, so WindowServer and the login session never start | `package/PianoNullEthernet`, that interface without virtio (measured: TimeSync's clock created, logind, loginwindow and WindowServer start, the display is turned on) |
| NativeAES | thought unused (no AppleKeyStore, Data unencrypted; the same boot without it), but AppleImage4's start waits for an IOAESAccelerator without a timeout, so without it the IOKit registry never goes quiet ([identity.md](identity.md) §8) | removed 2026-11-11; back 2026-11-13 as `package/SoftwareAES` (UniXNU's component, with the device key AppleImage4 entangles nonces with) |

Also this repository's packages: `ARMPlatformStandIn` (the vma2 collection's IOMobileGraphicsFamily and its kexts link against
AppleARMPlatform's classes) and PianoInput's HID class. Every removal was checked with a native run that matched the baseline on
five points: root mounted from UFS, Data resumed from its store, the same launchd boot tasks, no panic, no I/O errors.

### 5.2 The boot arguments

The arguments every image carries, who reads each, and the four legacy `droidvm-*` names the k27 kernel and the OS image still
read: [k27-userland-args.md](k27-userland-args.md).

**Beyond DarwinBoot's 1023 bytes (k27 0066, 2026-11-13).** DarwinBoot copies at most 1023 bytes of command line into
`boot_args.CommandLine` (the bootloader ABI, `BOOT_LINE_LENGTH` 1024), and the tools keep a line under 1000. With patch 0066 in the
kernel the rest lives in the ADT: the line's `piano-boot-args=NAME` makes the kernel append `/chosen/piano-boot-args-NAME` (a string)
to a copy of the line, right after the device tree is set up and before any tunable or kext reads an argument
(`piano-boot-args=0`: none; no selector: `/chosen/piano-boot-args` if the ADT has it). `PE_boot_args()` returns the copy (4096
bytes), so every `PE_parse_boot_argn()` / `bootArg()` sees both with no change in the kexts; without the property and the selector
nothing changes. One console line says what happened: `piano-boot-args: /chosen/piano-boot-args-ram (P bytes): T bytes appended,
boot-args M bytes (line L)` (or why nothing was appended: no such set, a byte outside printable ASCII, too long for 4095).
`kern.bootargs` keeps its 1024-byte answer, cut after the last whole argument (Apple's and third-party fixed-size readers depend
on it); the USB console's `args` prints the whole line. static-if keys (`validation_disables`, `serverperfmode`, `lcks`, `wdt`)
must stay on the line (read before the device tree).

**kern.piano_bootargs (k27 0071, 2026-11-15): the whole merged line for our own readers.** The clip above hid every argument past
byte 1023 from userland: `piano-usb-shell` in the ADT tail silently left the root shell down, and the rotation and Metal debug keys
fell to their defaults. The read-only `kern.piano_bootargs` answers exactly `PE_boot_args()` (the line plus the tail, at most 4095
text bytes; `sysctl_io_string` with trunc 0, so a too-small buffer gets `ENOMEM`, never a prefix). The repository's readers go
through `package/include/PianoBootArgs.h`: a 4096-byte buffer, falling back to `kern.bootargs` only on `ENOENT` (an older kernel --
its answer is still the clipped line); any other failure reads nothing and the caller's defaults apply, as before. The eight
readers moved in the same change: `pianousb-shell`, `piano-usbnet-address`, `piano-camera-grab --boot`, `piano-rotated`,
`NativeSoftMetal`, Turnip's abort reporter and the frontend/Mesa `piano-metal-barriers` / `piano-tu-debug` hunks.
`tooling/native/sysctl-test-shim.c` fakes both sysctls for their host tests (the shim emulates the kernel's string semantics,
including the `ENOMEM`).

The tools split only a line that would reach the budget (`PIANO_BOOT_ARGS_SPLIT=1` forces it): every `piano-*` argument moves to the
ADT except `piano-reset-after`, `piano-reset-to` and `piano-boot-guard*` (the way back to fastboot even under a kernel without 0066);
the rest (k27 arguments, `native-ufs-*`, `data-*`, `pstore-*`, `droidvm-*`, `cpus`/`sheng-smp`) stays, order kept within each part, so
for one name the last value still wins. `tooling/adt/boot_args.py` (`--show ADT --line LINE` prints the sets and the boot-args the
kernel makes; `--self-test`), `build-ram-images.py` (set `ram` in the payload's ADT; refuses a collection whose kernel lacks 0066).
Host test: `tooling/k27/test-piano-bootargs.sh`. `build-esp.py` and `tooling/k273/build-dual-esp.py` do not split yet (the next ESP
is the 27.3 line's only; its entry would take its own set, `piano-boot-args=<entry id>`, in its kernel's ADT).

### 5.3 The ADT identity (`configs/k27-adt.toml`, `configs/k27-gop-display.toml`)

| Item | Value | Why |
|---|---|---|
| `model`, `target-type`, `product-name`, `has-virtualization` | VirtualMac2,1, VMA2MACOS, "Apple Virtual Machine 1", set | the VM era's identity, kept as the default. No kext of the native collection matches on it (the VM-era collection's platform kexts did); what reads it is userland, and another identity is a build switch for test images: [identity.md](identity.md) |
| `vmm-present` | 1 (`PIANO_VMM_PRESENT` overrides it per build) | with 0 watchdogd runs as on real hardware, exits twice (no watchdog device) and launchd panics about 150 s into the run (`watchdogd[297] exited`); with 1 a 240 s run has no panic. Revisit with a watchdog driver |
| `serial-number` | DVMPIANO001 | made up, not a unit serial; the Data seed was made with it |
| `/arm-io/gop-display` | compatible `paravirtualizedgraphics,gpu` | IOMobileFramebuffer.framework only looks for displays on a node with that string (`docs/drivers/native-gop-display.md`) |

### 5.4 The macOS install

The installed OS is macOS 27.3 only since 2026-11-14; the VM-era 27.0.1 volume was cleared. Replacing the root means installing
the 27.3 root onto `linux_rootfs` with the §13.8 maintenance recipe in [persistent-boot.md](persistent-boot.md), not restoring
27.0.1. The installed Data seed identity is `9bd6f902`; its 200 GiB store is on `linux_data`. The old 27.0.1 OS/SystemOS windows
and Data seed on `linux_rootfs` were cleared before the 27.3 root moved there ([persistent-boot.md](persistent-boot.md) §13).

## 6. A kernel panic in the mailbox log (2026-11-12: the 13 GiB boot's `vm_page_unwire`)

The native kernel runs unslid: `atos -o "$PIANO_K27_TREE/out/k27/kernel-k27-00NN-native.macho" -arch arm64e <pc> <lr>` names the
addresses of the panic line directly (the kexts' addresses: the KC's descriptor). An `Assertion failed` stops in a `<function>.cold.N`
stub (`brk #0xbffc`, esr `0xf200bffc`), so pc names the function, and lr is in the same function; the console prints no backtrace,
so the caller comes from the registers: x19-x28 are the caller's wherever the asserting function had not yet changed them (its
prologue and the path to the stub, `llvm-objdump -d`), and a constant the caller keeps in one of them (here `mov w26, #0x14` and
`adrp x28, vm_pageout_debug` in `vm_page_do_delayed_work`) gives it away. The mailbox copy may still have single bit errors after the
majority (here x24 = x0 with bit 45 cleared); the USB capture (`logs/usb-console/`) is exact up to where it stopped.

The 13 GiB boot of 19:17 (`logs/native-runs/20261112-191724-loop`): `vm_resident.c:6226 Assertion failed: !mem->vmp_gobbled`, pc
`vm_page_unwire.cold.3`, lr `vm_page_unwire+860`, the caller `vm_page_do_delayed_work`'s `DW_vm_page_unwire` (an IOPL's
`upl_commit_range`/`upl_abort_range` slow path), the page's object a zone `vm_object`. Evidence vs inference:

| Item | State | Evidence |
|---|---|---|
| In k27 only `vm_page_find_contiguous(wire=FALSE)` sets `vmp_gobbled` (`kmem_alloc_contig_guard()` → `cpm_allocate()`); `kernel_memory_populate_object_and_unlock()` and `vm_page_wire()` clear it; `vm_object_iopl_wire_empty()` wires a fresh page without either | verified (source) | `osfmk/vm/vm_resident.c`, `vm_kern.c`, `vm_pageout.c` |
| `kmem_alloc_guard_internal()` takes the contiguous pages before `kmem_find_space()`; on its failure `vm_page_free_list(..., FALSE)` puts them on the free list still gobbled and still in `vm_page_wire_count`/`vm_page_gobble_count`; `vm_page_grab()` hands them out as they are | verified (source) | `vm_kern.c` `out_error` |
| The kernel map refuses one mapping above `vm_map_kernel_max_simple_mappable_size()` (2 GiB less a page, mode REJECT) in `vm_map_locate_space_anywhere()`, after the pages were taken | verified (source) | `vm_map.c` |
| PianoA830's S3 asks for a 2048 MiB pool first on every boot since 2026-11-12 (a quarter of the RAM, at most 2048), never granted (`row2 MEM pool 1024`, 512 or 64) | verified (logs, source) | `logs/usb-console/*`; `NativeAdrenoMemory::reservePoolBestEffort` |
| When that search finds 2 GiB of contiguous pages, 2 GiB of gobbled pages leak: the 17:27 session (8 GiB) shows it as phantom wired pages, `vm_stat` free + active + inactive + speculative + wired = 2,517,918 pages for the 2,097,152 of the RAM (1.6 GiB counted twice) | verified (log arithmetic) / inferred (that the 2048 MiB search was what leaked, and that the phantom wired count is what made XNU refuse the GPU's BoAllocs of 17:40) | `logs/usb-console/20261112-172703-port0.txt` `DD7:` lines |
| The 13 GiB boot's panic is such a page, wired by an IOPL and unwired through `vm_page_unwire()` | inferred (no other source of a gobbled page in k27 short of memory corruption; if `vm_pages[0]` is the window's first page, the page is about 9.2 GiB into the window, in RAM only the 13 GiB window has) | the registers: x0 - x13 = 2365929 x 48 (x13 taken for the `vm_pages` base, x11 = 0x30 the 48-byte `struct vm_page`) |
| With the 13 GiB window the kmem ranges are 2611 MiB each instead of about 4.5 GiB (the physmap takes the rest), so a full range is the other way a contiguous allocation fails after its search | verified (console) / inferred (that it happens) | the boot's VM range lines |
| The A/B image (same KC, 8 GiB ADT, 19:40) reserved a 64 MiB pool: the contiguous searches failed (the boot reservation of the volatile Data disk spreads wired pages), so its 2048 MiB request most likely leaked nothing | verified (console) / inferred | `logs/usb-console/20261112-194044-port0.txt` |

The fix: PianoA830 never asks XNU for a contiguous block above 2 GiB less a page (`NativeAdrenoContiguousMaxBytes`: the pool from
2048 MiB starts at 1024, the size every boot got; a contiguous BO above it is refused unasked; host-tested in `test_memory`), and k27
patch 0065 (`patches/k27/0065-*`) gives a failed contiguous allocation's pages back ungobbled with both counts fixed and refuses a size
the map would refuse before the search. Until 0065 is in the kernel, a contiguous allocation that fails on a full kmem range (13 GiB:
the shared data range holds the GPU's buffers and the Data disk's fallback chunks) can still leak.

**A kernel stack overflow** (2026-11-13, c3rp: `logs/native-runs/20261113-000631-loop`): `Invalid kernel stack pointer (probable
overflow). at pc 0xfffffff009fda0f0, lr ...` and `MACH Reboot`, no other panic text. pc in `el1_sp0_synchronous_vector_long` (+0xf0:
the vector's `stp x0, x1, [sp, #0x8]` after `sub sp, sp, #0x350`) means the exception frame itself faulted: SP_EL0 at the first
exception was sp + 0x350, and far = sp + 8 lies in the unmapped page below the thread's stack. A kernel thread has 16 KiB
(`KERNEL_STACK_SIZE` on this DEVELOPMENT kernel; the vector's check is SP above kstackptr - 0x4000, a little below the real bottom,
so a store just past the bottom faults first and the frame's store is the one reported). lr and x19-x28 are the overflowing
thread's (here `vm_map_store_lookup_entry`, a 64 KiB range: PianoCamera mapping DRV-2). A kext's own frames from its thread's entry:
`tooling/k27/check-kext-stack.py OBJECT.o ENTRY --indirect NAMES` (a static bound; XNU's IOKit mapping path below needed about 3-4
KiB); at run time `ml_stack_remaining()` (PianoCamera logs its least at the end of a run).
