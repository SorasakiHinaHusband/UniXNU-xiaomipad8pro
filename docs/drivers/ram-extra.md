# RAM16G R1 region1 GPU memory

## Status as of 2026-11-27

**The region1 GPU tier remains in qualification.** Installed v13.1y12q uses the 13 GiB XNU window (13310 MiB measured on
v13.1y12l on November 26). Installed v13.1y12l verified 4 KiB paged GPU memory with a 7168 MiB cap. The November 26 EFI audit
classified 319/322 two-MiB slabs as non-runtime, released 0 MiB and awaits owner verification. The November 26 RAM pass for
the 684 MiB mixed 4K Main10 HEVC arena exercised region0 and XNU backing for video. GPU tier and other driver pool qualification
remain open. [Support status](../STATUS.md) · [November 26 memory results](../results-2026-11-26.md#efi-memory-battery-and-video)

### Turnip 0015 private-tier RAM qualification (ram16g-tier3w, 2026-11-27)

On the tablet RAM boot, Turnip 0015 ran with kernel 0077, which loaded three Normal WC `region1` runs for
`PianoA830`'s `A830GpuTier`. All three runs passed both S8 endpoint probes (`self-test ok` × 3,
`pass-mask=0x7`, `fail=0`). The tier reported `mode=1 state=ready` with 870 MiB free.

`piano-tier-workload` (`TW`) stepped through **0 → 2 → 6 → 14 → 30 → 62 → 126 MiB** of private residency.
At peak, `free-mib` fell from 870 to 744, `used-mib=126`, `allocs=6` and `private-req=6`, with `refusals=0`,
`tier-import-denied=0` and `tier-cpu-map-denied=0`. All 126 MiB returned to the pool after exit.
Staged and control readbacks were byte-exact across **1,512 MiB (~1.5 GiB)** of reported copies.
Warm ran 18 iterations (378.0 MiB, GPU time p50 4.71 ms), and hold ran 54 (1134.0 MiB, GPU time p50 4.74 ms).
Both phases recorded `mismatches=0`, `control-mismatches=0` and `errors=0`.

WindowServer held PID 102 through desktop login, Safari (`BR4`), Metal Image (`M`, 30/30), the 3D bench (`RXL`)
and `TW`, with zero WindowServer crashes and zero GPU faults or recoveries. BR4 missed its accelerator-open
marker. The boot log and live kernel records confirmed Safari's GPU connection. The 3D bench issued zero private requests.

- **`TW` (`test/harness`).** Exit 4 reported `require-tier not met: fail-mask=0x2`. That bit marks the harness's
  `mode=2` requirement on this `mode=1` boot. Allocation, residency, release, readback and denial checks passed.
- **`RXV` (pre-fix frozen image).** AVC `video-v6` passed 12 phases, then timed out at `cancel-recreate`
  with eight accepted frames and four pending. The frozen image predates `a426ce70` on `main`, which defers
  the `dinv` reply in `PianoVideoVTHost.cpp` until terminal `Demt` dispositions are acknowledged.
  [Pending-cancel analysis](video-hevc-cancel.md)
- **`RXB` (`test/environment`, known `LT-B4`).** `install-chrome-pad.sh` completed and Chrome launched on RAM-backed
  volatile Data, then the tablet reset to ABL at +1719 s. The abort matches the known RAM-Data exhaustion reset.
  Chrome is installed-only, and the like-for-like `ram16g-tctl3` control was underway.

Daily stays **OFF** (`piano-extra-ram=0`, `piano-gpu-tier=0`). This qualification covers the RAM boot only.

## R1 design and admission checks (2026-11-26)

R1 gives eligible Adreno 830 buffer objects (BOs) access to reserved RAM below
XNU's single contiguous RAM window. The captured memory layout has three
candidate ranges totalling 870 MiB after rounding inward to 2 MiB boundaries.
DarwinBoot's live UEFI map determines the reservations. Only GPU-qualified runs
become allocatable.

### Reference path

Ubuntu v1.4.0 pins Linux `83f0f89bed14` in `kernel/source.json`. The Linux and
sheng paths below establish the GPU allocation and mapping model. The 27.3
Metal/IOKit boundary follows the `AGXDeviceUserClient` ABI in
[gpu-userspace.md](gpu-userspace.md). R1 requires no change to Metal, the plugin
or Turnip. The Android capture has no stock allocator source for this path.

| Step | Ubuntu / sheng reference | Piano R1 |
|---|---|---|
| GPU and SMMU | Linux `arch/arm64/boot/dts/qcom/sm8750.dtsi:2318–2328` identifies Adreno 830 and the GPU SIDs | Same GPU and SIDs. `NativeAdrenoGpuContext` uses the existing CB0 page tables and TLB protocol. |
| BO backing | Linux `drivers/gpu/drm/msm/msm_gem.c:204–245` gets normal pages and builds a scatterlist. sheng's Adreno740 `7487f72` `NativeAdrenoUserClient.cpp:236` starts allocation in its user-client core. | Different source for eligible BOs. `NativeAdrenoMemory::allocateTier` reserves whole slabs, clips the last exposed span to the requested length and falls back through the existing allocation chain on refusal. |
| GPU mapping | Linux `msm_gem.c:505–532` maps the scatterlist. Cached-coherent backing explicitly opts in to cached IOMMU access. | Same scatter/gather principle. `NativeAdrenoVm::attributes` assigns tier BOs non-cacheable leaves. Paged VM pressure and the client cap still count their mapped bytes. |
| CPU view | Linux `msm_gem.c:357–361,1192–1194` sets write combining for WC objects. sheng's Adreno740 `NativeAdrenoPublish.cpp:532` creates a Physical64 descriptor with `withAddressRanges`. | Same write-combined CPU access and Physical64 ranges. Tier views use the factory's designated initializer in a descriptor that enforces WC because IOKit accepts caller-supplied cache bits. |
| Admission | Linux GEM obtains already-managed pages. | Different ownership. DarwinBoot reserves the runs before ExitBootServices. PianoA830 validates the handoff and probes both endpoint pages with the GPU before exposing a run. |

### GPU tier controls

The combined analysis gate and RAM-boot reads are in [RAM16G extra RAM](../extra-ram.md).
PianoA830 validates the shared 24-byte records against its physical window and uses bank 1/use 1 only.
Mode 2 additionally tries sub-2 MiB BOs, each occupying at least one slab. Full cached coherence
(`piano-gpu-coherent=2`, upstream `pagani-gpu-coherent=2`) closes the tier. A probe failure closes the affected
run. If recovery or TLB cleanup cannot quiesce the probe, S8 stops before Metal publication.
