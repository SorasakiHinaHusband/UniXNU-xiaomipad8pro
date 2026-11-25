# RAM16G R1 region1 GPU memory

R1 gives eligible Adreno 830 buffer objects (BOs) access to reserved RAM below
XNU's single contiguous RAM window. The captured memory layout has three
candidate ranges totalling 870 MiB after rounding inward to 2 MiB boundaries.
DarwinBoot's live UEFI map determines the reservations. Only GPU-qualified runs
become allocatable.

## Reference path

Ubuntu v1.4.0 pins Linux `83f0f89bed14` in `kernel/source.json`. The Linux and
sheng paths below establish the GPU allocation and mapping model. The 27.3
Metal/IOKit boundary follows the `AGXDeviceUserClient` ABI in
[gpu-userspace.md](gpu-userspace.md). R1 requires no change to Metal, the plugin
or Turnip. The Android capture has no stock allocator source for this path.

| Step | Ubuntu / sheng reference | Piano R1 |
|---|---|---|
| GPU and SMMU | Linux `arch/arm64/boot/dts/qcom/sm8750.dtsi:2318–2328` identifies Adreno 830 and the GPU SIDs | Same GPU and SIDs. `NativeAdrenoGpuContext` uses the existing CB0 page tables and TLB protocol. |
| BO backing | Linux `drivers/gpu/drm/msm/msm_gem.c:204–245` gets normal pages and builds a scatterlist. sheng's Adreno740 `7487f72` `NativeAdrenoUserClient.cpp:236` starts allocation in its user-client core. | Different source for eligible BOs. `NativeAdrenoMemory::allocateTier` reserves whole slabs, clips the last exposed span to the requested length and falls back through the existing allocation chain on refusal. |
| GPU mapping | Linux `msm_gem.c:505–532` maps the scatterlist; cached-coherent backing explicitly opts in to cached IOMMU access. | Same scatter/gather principle. `NativeAdrenoVm::attributes` assigns tier BOs non-cacheable leaves. Paged VM pressure and the client cap still count their mapped bytes. |
| CPU view | Linux `msm_gem.c:357–361,1192–1194` sets write combining for WC objects. sheng's Adreno740 `NativeAdrenoPublish.cpp:532` creates a Physical64 descriptor with `withAddressRanges`. | Same write-combined CPU access and Physical64 ranges. Tier views use the factory's designated initializer in a descriptor that enforces WC because IOKit accepts caller-supplied cache bits. |
| Admission | Linux GEM obtains already-managed pages. | Different ownership. DarwinBoot reserves the runs before ExitBootServices. PianoA830 validates the handoff and probes both endpoint pages with the GPU before exposing a run. |

## Fix-image gate

Path: ABL memory description and carve-outs → UEFI MemoryMapLib and
RamManagerDxe → DarwinBoot `AllocateWindow`, then address-specific
`EfiReservedMemoryType` reservations → copied ADT
`/chosen/piano-extra-ram` → unchanged XNU window → PianoA830 S8 endpoint
`CP_MEM_WRITE`, fence and physical readback → S9 Metal publication →
WindowServer/Metal / NativeAdrenoMetal / Turnip → `BoAlloc`, Physical64 CPU
view and SMMU map → fence retirement, CPU lease teardown and `BoFree`.

Checks: ours-OK by source and host checks are the contiguous window, reserve
and ADT publication logic, placeholder compatibility, slab alignment and GPU
address-width checks, both endpoint checks before allocation, write-combined
cache policy, physical zeroing, atomic fallback, and separate tier counters.
Ours-missing on the original path were the boot-time reservation and a BO
backing that could use it. Unknown until the RAM boot are the actual UEFI
conventional runs, end-to-end GPU stage-2 access, CPU view behavior on the
target kernel, and frame times under load.

Root cause: DarwinBoot gives XNU a single range beginning above the region1
holes. XNU's physical aperture and free-page list are bounded by that
range. The paged GPU allocator draws its backing from that list, so region1
cannot contribute to GPU allocations through the existing path. See
[`dts/piano.dts`](../../dts/piano.dts) and
[the captured memory map](../hyp-evidence/memory-map-running-dt.txt).

ONE change: Add an opt-in DarwinBoot-to-PianoA830 reservation handoff for
region1. Use qualified slabs for eligible BOs and keep the normal pool/XNU
fallback. The physical window XNU receives stays the same.

## R1 RAM boot read list

Use an FD with DarwinBoot patches 0001 and 0002. Package the RAM payload with
`build-ram-images.py --extra-ram` and use the existing 27.3 desktop RAM profile
with `data-persist=0 piano-gpu=2 piano-gpu-mem=paged piano-gpu-user-vm=1
piano-gpu-tier=1`. Keep the boot guard and desktop GPU policy. An equivalent
control uses `piano-gpu-tier=0` and the ordinary ADT. Mode 2 additionally tries
sub-2 MiB BOs, each of which occupies at least one slab. Full cached coherence
(`piano-gpu-coherent=2`, the upstream `pagani-gpu-coherent=2`) closes the tier.

1. Capture early console lines matching
   `DARWINBOOT: (window|extra-ram)`, `NativeRamBlock: RAM window`,
   `PianoA830: gpu tier:` and `PianoA830: adreno830_start:`. Keep the
   reservation base, end and MiB for each run, the total, and every probe
   result. Expect the usual XNU window and one tier pass/fail result per
   published run. A successful R1 test needs at least one passing run.
2. At desktop idle, during the same textured 3D workload used for the
   control, after closing it, and at +10 min, save these readouts with
   timestamps. Keep the sysctl names in the output.

   ```sh
   date -u
   sysctl kern.bootargs hw.memsize hw.memsize_usable hw.physmem hw.pagesize
   sysctl machdep.piano_gpu_tier machdep.piano_gpu_paged machdep.piano_gpu_pt
   sysctl machdep.piano_gpu_state machdep.piano_gpu_recovery
   sysctl kern.memorystatus_level kern.memorystatus_vm_pressure_level vm.swapusage
   sysctl -a | grep '^vm\.compressor_'
   vm_stat
   memory_pressure
   ioreg -p IODeviceTree -r -n chosen -d 1 -w0 | grep '"piano-extra-ram"'
   ioreg -r -n NativeAdrenoGpu -d 1 -w0 | grep -E '"NativeAdreno(Client|Coherent)'
   ```

   Decode only `piano-extra-ram` from the chosen node. Expect version 1 and
   the successful reservations from the firmware lines. Keep the extended
   arguments record from the build if `kern.bootargs` is clipped. For R1,
   expect `mode=1 state=ready`, matching pass-mask and console outcomes, and
   `allocs` and `used-mib` to rise during allocation while `free-mib` falls.
   After exit, the workload's slabs should return. Desktop-owned slabs can
   remain in use. Track any `refusals` alongside the ordinary fallback
   counters and verify that the pool counters do not include tier BOs.
3. Save per-frame presentation intervals for the same workload and duration
   in the control and R1 runs. Report p50, p95, p99 and the count over
   16.67 ms at identical refresh settings. Retain the console window covering
   the workload, including `CB0`, `SMMU`, `fault`, `hang`,
   `recover` and `NativeGopDisplay:` lines, and record any pixel errors.
   Correlate frame intervals with the tier's measured usage, final GPU
   recovery counters and paged-VM audit result.
