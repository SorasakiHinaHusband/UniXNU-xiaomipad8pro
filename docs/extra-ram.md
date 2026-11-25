# RAM16G R3: driver pools in the region0 bank

The tablet has 16 GiB of DRAM. ABL hands the HLOS 15.34 GiB, and XNU manages one contiguous window of 13310 MiB at 0x8c0004000.
About 1522 MiB of HLOS memory sits in region0, the 2 GiB bank at 0x80000000, split into 11 pieces between about 50 carve-outs.
arm64 XNU cannot add that memory to its VM: it manages one range (one `physBase`/`memSize`, and `pa_valid` is one compare). Drivers
can still use it as private DMA pools. R3 is that option. R1 is the sibling option: the region1 runs below the window for the GPU.
The two share one ADT interface, and this page owns its region0 half.

## The interface

DarwinBoot publishes `/chosen/piano-extra-ram`, an array of 24-byte little-endian records:

| field | type | meaning |
|---|---|---|
| base | u64 | the run's first byte |
| size | u64 | its length |
| bank | u32 | 1 = region1 (R1), 0 = region0 (R3) |
| use | u32 | 0 = any driver pool, 1 = GPU, 2 = DSP, 3 = video, 4 = camera, 5 = display |

DarwinBoot takes the region0 runs from `firmware/patches/darwinboot/region0-allow.json`. That file also has the never-use list and
DarwinBoot's rule. A run is published only in two cases:
- Every UEFI descriptor overlapping it is conventional memory. DarwinBoot then reserves it with `AllocatePages(AllocateAddress)`
  and publishes it only if that succeeds.
- An EfiReserved descriptor covers it wholly. This is PIL_Reserved, where UEFI never allocates.

Any other descriptor type means the run is not published. Each decision is printed.

`piano-extra-ram-stage` (default 1) caps the stage. The kexts never trust the ADT alone. `package/include/PianoExtraRam.h` keeps its
own allow and never tables, which `tests/region0_allow_check.py` holds equal to the JSON. It refuses any run that is outside them,
touches a never span, overlaps XNU's window or an earlier run, or fails its self-test.

## Region0, run by run

Sources:
- ABL `/memory` as seen in Android's live DT and in the Ubuntu running DT (`docs/hyp-evidence/memory-map-running-dt.txt`).
- Android `/proc/iomem` System RAM and the dmesg `OF: reserved mem` lines (rooted stock capture, 2026-11-20).
- pianoPkg `MemoryMapLib.c`, plus Mu-Silicium `RamManagerDxe`. RamManagerDxe adds every gap of the static map inside an SMEM RAM
  partition as EfiConventionalMemory.

| HLOS piece (ABL /memory) | MiB | Android | mainline DT | UEFI | verdict |
|---|---|---|---|---|---|
| 0x81960000.., 4 slivers | 2.66 | RAM | RAM | RSRV0/XBL_DT etc. | never (no whole 2 MiB slab) |
| 0x82800000..0x84a00000 | 34 | qdss CMA (coresight ETR assigns it) | qdss no-map | gap | never |
| **0x89d80000..0x9b000000** | 274.5 | **System RAM**, no mpss node | mpss@8ba00000 no-map, status disabled | **PIL_Reserved** (EfiReserved) from 0x89e00000 | **allow "modem" 0x89e00000 + 274 MiB, use 3 video, stage 1** |
| **0x9ee00000..0xb8000000** | 402 | System RAM, ramoops 0xa3500000 | RAM, ramoops | PIL_Reserved to 0xa3500000, then Display_Demura, FD, Kernel | **allow "pil-tail" 0x9ee00000 + 70 MiB, use 2 DSP, stage 1**; the rest never (Demura, UEFI FD/Kernel) |
| 0xbd980000..0xd4e23000 | 372.6 | System RAM (secure_cdsp, non_secure_display CMA) | RAM | DXE_Heap | never until a DarwinBoot map print shows it free of runtime data |
| 0xd5100000..0xd8000000 | 47 | RAM | RAM | DXE_Heap, Piano_Log 0xd7f00000 | never (Piano_Log) |
| **0xe3720000..0xf8800000** | 336.9 | System RAM: reusable CMA only (cnss_wlan, qmc_dma, va_md, adsp_heap, linux,cma, vm_comm, qseecom_ta, qseecom, sp_region), trust_ui_vm 0xf3800000 + 80 MiB (gh_secure_vm_loader assigns it) | RAM + trust-ui-vm no-map | not in the static map, so RamManagerDxe makes it conventional | **allow "cma-block" 0xe3800000 + 256 MiB, use 0 any, stage 2**; trust_ui_vm never |
| 0xfc400000..0xff800000 | 52 | splash (map), cdsp_eva CMA | splash no-map | Display_Reserved | never (the boot framebuffer is scanned out) |

The allow-list holds 600 MiB in 300 slabs: stage 1 has 344 MiB, stage 2 has 256 MiB. The never list also covers everything the brief
names: qdss, splash, Display_Demura, Piano_Log, cvp, cdsp and video. The host tests check each of these addresses.

Why the modem run is first:
- Android, Ubuntu and UEFI all agree that nothing else owns it. Android and Ubuntu use it as plain RAM. UEFI marks it EfiReserved.
- No firmware is loaded there, because the tablet has no modem.
- DarwinBoot has nothing to allocate for it.
- It holds the AVC arena whole.

Inferred, not yet seen on the tablet: nothing touches the modem run between ABL and XNU. The RAM boot's self-test and the first
decoded frame settle this.

## Which drivers, and in which order

| driver | pool today (from XNU's window) | DMA path | CPU view | fit for region0 | risk |
|---|---|---|---|---|---|
| GPU (PianoA830) | 1 GiB BO pool + paged BOs | own SMMU bank, translated (client IOVA 0x20000000..0x3ffffe000) | WC/cached | R1 serves it from region1 | R1's |
| **video (PianoVideo level 6)** | **256 MiB AVC arena, boot-lifetime reservation ("V9h"), 4 KiB pages; HEVC sessions up to 768 MiB** | own context bank, translated at 4 KiB (VideoSmmu.h/VideoArena.h); PA below the bank's output size | WC only (cached zero + clean, then never cached) | **yes: no contiguity, whole slabs, WC is the attribute already proven for out-of-window memory (cdsp slice)** | moderate: an active lane; the fallback is the XNU path unchanged |
| camera (PianoCamera level 5) | 16 MiB + 16 KiB contiguous, 2 MiB aligned, reserved early against fragmentation | translated window, 2 MiB blocks of one contiguous range (CameraSmmuWindow.h), PA < 1 TiB | cacheable + `dc civac` | yes (8 contiguous slabs; fragmentation goes away) | low, but the gain is small; needs a cacheable mapping of non-managed memory (unproven) |
| audio (PianoAudio) / sensors FastRPC (PianoSensors) | 16 + 8 MiB in the cdsp carve-out | bypassed streams: IOVA = PA, must be below 4 GiB | WC | region0 is the only place that works; pil-tail (use 2) is reserved for them | low; no need today (the slices are not full) |
| Wi-Fi (PianoWifi) | 24-64 MiB contiguous below 2^36 (`piano-wifi-dma`) | bypass at L3/L4, a 32-bit translated window at L5 | WC | possible later (below 4 GiB would make a bypass stream 32-bit clean) | high: the top-priority lane |
| DPU (PianoDpu) | scans IOSurfaces (XNU pages) through its own L3 windows | its bank identity-maps 0x80000000..0xffffffff read-write | n/a | not a pool user | note: the DPU stream can reach every region0 run, as it already reaches cdsp/cvp |
| UFS (NativeUfs), ADSP metadata, Bluetooth | KiB-sized tables, bounces, PAS metadata | various | various | no | not worth it |

Order:
1. Video, the first integration. It is the largest pool that is already out of a hard path: a boot-lifetime 256 MiB that frees
   256 MiB of window RAM for the whole boot. Its CPU attribute and SMMU translation need nothing new.
2. Camera, the 16 MiB early reserve, once a cacheable mapping of a region0 slab is proven, or with its view switched to WC.
3. DSP (pil-tail), when audio or sensors outgrow the 25 MiB cdsp slice.
4. Stage 2 (cma-block, use 0). This comes after the modem run passes on the tablet and DarwinBoot's map print shows the block as
   conventional and reserved. Its likely owner is video, for HEVC: `piano-extra-ram-any=3`, but 274 + 256 MiB is still under the
   768 MiB HEVC maximum, so HEVC also needs a mixed backing. That is an open item.

## The helper

The helper has three parts:
- `package/include/PianoExtraRam.h` is portable. It parses the records, holds the allow, never and window checks, and decides
  ownership by use tag. Use-0 runs go only to the one use named by `piano-extra-ram-any`, so two kexts never share a slab and no
  kernel-wide word is needed. It also hands out 2 MiB slabs (contiguous or not, all or nothing), runs the self-test and builds the
  report.
- `package/include/PianoExtraRamKernel.h` reads the ADT and the boot arguments and runs the self-test through IODeviceMemory WC maps.
  It hands out slabs under a lock and describes taken slabs as one `kIOMemoryTypePhysical64` descriptor.
- `package/include/PianoExtraRamSysctl.h` provides `machdep.piano_extra_ram_<use>`, read-only and root-only. It sits in its own source
  file, because `<sys/sysctl.h>` declares `struct session`, which collides with `pianovideo::session`.

Self-test, per run, in two passes over every slab:
- Pass 1 writes a per-address pattern into three words of each 4 KiB page.
- Pass 2 reads every pattern back, then zeroes the slab and checks the zeros.

Because the pattern depends on the address, an alias inside the run fails. A failed run is refused, and none of its slabs are handed
out. A device-side hook, `device(pa, bytes)`, is called per run. Video has no hook of its own: the first decoded AVC frame landing in a
region0 arena, matching the fixture's golden SHA-256, is its device-side proof.

Switches, all off by default:
- `piano-extra-ram=1` is the master switch.
- `piano-video-extra-ram=1` turns on video's use.
- `piano-extra-ram-any=<use>` names the one use served the use-0 runs.
- `piano-extra-ram-stage=N` caps the stage served (default 1).
- `piano-extra-ram-test=0` skips the self-test. A skipped test means nothing is handed out.

Tests:
- `package/include/tests/extra_ram_host.cpp` and `region0_allow_check.py` run in `package/include/tests/run.sh`.
- `PianoVideo/tests/geometry_host.cpp` builds the arena on region0 slabs, in reverse slab order and mixed with XNU pages. It also checks
  that pages just outside the run, and the video and cdsp carve-outs, are refused.

## Fix-image gate

Path: ABL `/memory` (region0 HLOS pieces) -> UEFI map (pianoPkg MemoryMapLib: PIL_Reserved EfiReserved; RamManagerDxe: the gaps become
conventional) -> DarwinBoot (R1's change: per-run publication by the region0-allow.json rule, map print) -> `/chosen/piano-extra-ram` ->
PianoVideo start at level 6 -> `pianoxram::KernelPool::start` (boot args, ADT, PianoExtraRam.h allow/never/window checks, use-3
ownership, per-run CPU self-test through IODeviceMemory WC) -> `dmaAlloc` session arena: `takeExtra` takes 128 slabs ->
`kIOMemoryTypePhysical64` descriptor -> WC kernel view, zeroed -> 65536 arena page PAs -> `rememberPage` -> `buildArena` (VideoHw.h
`arenaPagePa`) -> the video bank's L3 leaves -> Iris DMA -> RAW loans (`IOSubMemoryDescriptor` of `arenaDesc`) -> the VT frontend's
IOSurface copy from `frame->cpu` (the WC view).

Checks:
- ours-OK, verified in source or by host tests:
  - The modem and PIL-tail runs are HLOS RAM in ABL, Android and mainline, EfiReserved in UEFI, and outside every carve-out.
  - The allow and never tables equal the JSON.
  - Ownership by use tag; slab hand-out and give-back.
  - Self-test detection of an alias, a bit error and a failed map; refusal on a failed device check.
  - `buildArena` accepts region0 pages and refuses pages next to the run.
  - The kext builds and its imports check passes, without a static constructor (constinit).
  - Loans use `arenaDesc`.
  - XNU fallback on every shortfall.
- ours-missing:
  - DarwinBoot's publication and map print (R1's job, shared interface).
  - A device hook for the camera and DSP users (not integrated yet).
  - HEVC mixed backing.
- unknown, the RAM boot answers:
  - The modem run is untouched at XNU entry (the self-test).
  - Iris DMA reaches region0 through Gunyah's SMMU stage 2 (the AVC golden frame; the Iris firmware carve-out at 0x9c300000 is in
    region0 too, which suggests stage 2 covers the bank).
  - The cost of WC zeroing (start time).
  - `IODeviceMemory` WC maps of non-managed memory are accepted for 2 MiB at a time (the cdsp slice pattern: verified for 16 MiB).

Root cause: arm64 XNU manages one contiguous physical range, so the 1522 MiB of HLOS memory in region0 is never XNU RAM. The video
driver's boot-lifetime 256 MiB AVC arena takes window RAM, even though it needs neither XNU's VM nor contiguity, and its bank
translates any PA.

ONE change: PianoVideo backs its level-6 AVC arena with 128 slabs of the region0 modem run when `piano-extra-ram=1` and
`piano-video-extra-ram=1` are set (default 0), with the self-tested shared pool. It falls back to the XNU allocation on any shortfall.

## RAM-boot read list

This needs one RAM image carrying R1's DarwinBoot publication and this branch, with boot args `piano-video=6 piano-video-vt=1
piano-extra-ram=1 piano-video-extra-ram=1`. The first rows are read-only reads on the running boot.

| question | read | pass |
|---|---|---|
| DarwinBoot published the modem run | console: DarwinBoot's per-run map print and decision | modem run published, descriptor type EfiReserved; cma-block not published at stage 1 |
| the ADT holds it | `ioreg -p IODeviceTree -n chosen -l \| grep piano-extra-ram` | 24-byte records incl. `0x89e00000 + 0x11200000, bank 0, use 3` |
| the kext's verdict | `sudo sysctl machdep.piano_extra_ram_video` | `run.. base=0x89e00000 ... state=ours span=modem slabs=137 ... self-test=pass pages=70144 bad=0`, `used=128 free=9` |
| the arena came from it | console: `PianoVideo: RAM16G arena from extra RAM: 128 slabs, first 0x89e00000, ..., result 1` and `V9h boot reservation ready` | both present; no `RAM16G arena from XNU` |
| Iris DMA reaches region0 | the AVC qualification: decoded NV12 SHA-256 = the fixture golden `bd230ec4...` | match (this is the device-side check) |
| window RAM freed | `vm_stat` / `sysctl vm.page_free_count` vs the same image with `piano-video-extra-ram=0` (A/B by boot arg) | about 256 MiB more free or less wired |
| nothing else broke | the RAM qualification smoke (boot, ssh, login, Wi-Fi lease, audio, charging), panic log empty | as in the smoke |
| harness known-positive | `sysctl machdep.piano_extra_ram_video` on a boot with `piano-extra-ram=0` | `piano-extra-ram=0 ... adt=ok`, runs listed, no self-test, `used=0` |
