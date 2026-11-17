# PianoVideo

piano's video core for native macOS 27: the SM8750 Iris (VPU 3.5) that decodes H.264, HEVC and VP9 and encodes H.264 and HEVC in
hardware. **V2 power/PAS is tablet-confirmed** for three cycles in `vid-v2c5`; **V3 INIT/version, ten collapse/resume cycles and legal teardown are tablet-confirmed** in `vid-v3b`.
V4 is tablet-confirmed in `vid-v4a`: one real 320×240 AVC AU produces the exact active NV12 golden hash and safe teardown.
V5c is tablet-confirmed: one actual caller AU traverses VT and Iris into an IOSurface-backed golden callback.
V6 adds bounded persistent AVC streaming; continuous hardware results remain pending.
`piano-video=6 piano-video-vt=1` publishes the restricted AVC qualification endpoint. Other levels do not publish a decoder inventory. 27.3's guest gate is a registry match for `AppleVideoToolboxParavirtualizationDriver` followed by `IOServiceOpen(type=0)` on `AppleVideoToolboxParavirtualizationUserClient`; it does not require `kern.hv_vmm_present`.

- `piano-video=1`: a read-only survey: GCC's four video branches first; VIDEO_CC only with GCC's video AHB on (PLL0, the core
  clock's RCG and dividers with the rate and the OPP it matches, both GDSCs, every branch); the core's controller registers
  (version, CTRL_INIT/CTRL_STATUS, queue table, UC region, interrupt, LPI and power-status words) only with the controller GDSC
  powered and clocked, the codec's idle words only with the codec domain on too; the apps SMMU's entries for streams
  0x1940..0x194f, the free stream-match groups and the banks in use. Every MMIO access is a load; the write-to-clear and doorbell
  registers are never loaded. `piano-video-carveout=1` also loads the first words of the firmware carveout.
- `piano-video=2` (V1): the survey, then power cycles at the lowest operating point (240 MHz): MXC svs, MMCX at the boot display's
  floor and BCM MM1 by RPMh under PianoA830's DRV-2 lock, GCC's video branches, VIDEO_CC's PLL0 at 720 MHz, the controller then
  the codec GDSC and clocks, the readout with the gate open, off in Linux's order (`piano-video-cycles`, `-hold-ms`).
- `piano-video=3` (V2): + the owner's `vpu35_4v.mbn` (pinned size and SHA-256) from the RAM image's addon or the ESP, PAS 9 and MP_VIDEO_VAR under one
  hold of the kernel-wide secure-call lock, PAS shutdown before the power-off.
- `piano-video=4` (V3): + the streams' own SMMU bank and the UC region, the boot words and the HFI handshake (SYS_INIT,
  IMAGE_VERSION, the version string), collapse 1.5 s after the last activity with the firmware kept, resume/collapse cycles
  (`piano-video-resumes`, default 10), PAS shutdown and the bank released (`piano-video-unload`, default 1).
  V3 prepares queues/tables before c5 power-on, then applies the S1 domain with its own ASID and publishes routes last.
  Required SCTLR bits exclude optional ASIDPNE; TCR2 verification tolerates the observed TBI bits. Release keeps buses on until
  PAS stops and routes/TLBs detach, never restores unsupported S2CR/CBAR words, and retains DMA on an unconfirmed cleanup.
  V3 records the bank's readbacks, all queue layouts, CTRL_STATUS samples and elapsed time, each TX/RX and doorbell, separate
  INIT/version results, PC/resume cycles, faults/watchdog/SFR, and teardown results. `piano-video-resumes=0 piano-video-unload=0`
  explicitly selects the earlier collapsed steady state. V2's defaults and power/PAS sequence are unchanged.

- `piano-video=5` (V4): one own 320×240 Baseline H.264 IDR, under V3's power/PAS/SMMU owner. Separate wired NC data
  mappings, first PSC and real DPB/COMV counts, all five input internals including PARTIAL, RAW NV12 active-area SHA-256,
  DRAIN/LAST, both STOPs and CLOSE. Five-second transaction, bounded cleanup; unsafe quiesce/detach retains every DMA owner.
  It always tears down, uses no VT consumer and does not repeat the ten V3 resume cycles. No playback support is claimed.

- `piano-video=6 piano-video-vt=1` (V6): one exclusive demand session for bounded 320×240 progressive 8-bit AVC
  Baseline/Main IP/IBP. Parse actual avcC/SPS/PPS and complete one-picture samples, preserve opaque tokens, use two owned DMA
  input slots with eight frame credits, and continuously pump HFI while guest replies are pending. RAW slots recycle after
  checked active-row copies into scoped IOSurface clients; split DPB stays firmware-owned. Finish is a nonterminal
  ACK+LAST barrier; later input resumes both ports. Invalidation cancels the old epoch and joins before bounded
  STOP/CLOSE/PAS/routes/TLB teardown. Unsafe quiesce retains DMA and poisons future opens.
  Synchronous flags0, in-place format changes, HEVC, tiles and default app playback remain unsupported.

Levels 2-6 write registers only through `VideoBringup.h`: every (address, value) pair is logged the first time it is written,
every wait is bounded, V3 cleanup reports unsafe releases and keeps their DMA, and nothing is written below `piano-video=2`. Design, decisions, the
batched tablet test list with its expected lines and the risks: `docs/drivers/video.md` (section 12 for levels 2-4).

| File | What |
|---|---|
| `PianoVideo.cpp` | the IOService on IOPlatformExpertDevice; the survey thread and its log lines; the kernel's Hw for levels 2-4 (MMIO pages on demand, cmd-db, the DRV-2 word, the firmware through the shared loader (`piano_fw_load`), SHA-256, the metadata buffer, the carveout and UC-region mappings, the secure calls); the `piano-video-survey` and `piano-video-bringup` properties |
| `VideoSurvey.h` | portable: what level 1 reads (no store in the file), the gate, the clock and status decoders, the SMMU scan, the verdicts |
| `VideoOps.h` | portable: the one access path of levels 2-4 (the write log, regmap-style read-modify-writes, bounded polls), steps and errors |
| `VideoRpmh.h` | portable: cmd-db lookups, corners, one ACTIVE-TCS transaction on apps DRV-2, the video's MXC/MMCX/MM1 votes |
| `VideoPower.h` | portable: V1's power-on and power-off in Linux's VPU 3.5 order at 240 MHz; the readout's verdict |
| `VideoFirmware.h` | portable: V2's pin, the image's place in the carveout, PAS 9 + MP_VIDEO_VAR under the secure-call lock, PAS shutdown |
| `VideoSmmu.h` | portable: V3's SMMU plan, our table for the UC region, the bank and routes, the teardown |
| `VideoHfi.h` | portable: V3's queue table and queues, HFI gen2 packets and answers, the boot words |
| `VideoSession.h`, `VideoBuffers.h`, `VideoArena.h` | portable V4 session, fixed fixture geometry, guarded 64 MiB arena and expanded same-bank tables |
| `VideoFixture.h` | own generated SPS/PPS/IDR; reproduction, SPS inspection and software golden verification in `tests/fixture_host.py` |
| `tests/session_host.cpp` | asynchronous scripted peer, counts 1/4/7/64, reordered replies, checksum and failure injection (host evidence only) |
| `VideoBringup.h` | portable: levels 2-6 as one sequence (V1, V2, V3, the idle collapse, resume, unload, failures) |
| `VideoHw.h` | the facts, each with its source |
| `FatVolume.c/h`, `QcomMdt.c/h`, `Sm8750Scm.c/h` | copies of PianoPower's (the build checks them byte for byte; Sm8750Scm is MIT); FatVolume serves the shared loader's ESP source |
| `tests/video_host.cpp` | level 1's host tests against register models (the gate, the allowlist of loads, decoders, verdicts, SMMU) |
| `tests/bringup_host.cpp` | levels 2-4 against one SoC model (VIDEO_CC, the core, DRV-2, cmd-db, the secure world, the SMMU, a firmware reached through our table), every wait made to fail, every secure call refused, the stop argument at every step |
| `vt/` | VideoToolbox paravirtual frontend and wire format from third-party UniXNU-droidvm commit 706338e; bounded native avc1 endpoint, descriptor/WC layout contract, IOSurfaceRoot lookup/copy and asynchronous guest completion |
| `tests/vt_host.cpp` | protocol6 wire atom round-trip; V5a parsing/layout/cancel contracts in `tests/vt_contract_host.cpp` |

Build: `tooling/k27/build-native-piano-video.sh` (the secure-call lock's tests, the shared firmware loader's tests, the copies'
check, the host protocol/lifetime/layout tests, the pin, the objects with the loader's `package/common` files, the import check). PianoVideo is in
`configs/k27-native-kexts.txt` after PianoA830 (its lock words) and NativeUfs (the loader's ESP source), and inert without `piano-video`; level 6 additionally requires `piano-video-vt=1` for the VT endpoint. The firmware (`vpu35_4v.mbn`, the owner's copy, never committed) is pinned as `PianoVideo/vpu35_4v.mbn`
(docs/kc-space.md §6): the build stages it into the ignored `out/firmware/PianoVideo/`, `build-ram-images.py` puts it into a RAM
image's addon and `build-esp.py` onto the ESP; level 3 loads it at run time. Sources and
licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent; the MIT notice for Sm8750Scm).

V5a tool: `bash tooling/native/build-video-v5a.sh` builds an ordinary signed arm64 executable. `video-v5a --self-test`
checks fixture and active-row hashing without querying VT; `--qualify` requires hardware and paravirtual decoding and
checks dcan, exactly one IOSurface-backed 420v golden callback, unsupported input, terminal finish and safe invalidate.
These host tests and implementation claims are not tablet acceptance or general AVC/HEVC playback support.

V6 qualification: `bash tooling/native/build-video-v6.sh` compiles/signs the tool and audits both owned IP/IBP clips.
Where private PV helpers are exported, it also runs actual Apple serialization of all 120 samples through the production
parser. On 27.3 those helpers are local: the offline audit reports SKIP, and actual target serialization is exercised by
public VT decode during qualification. `--self-test-public-only` tests the unavailable-export path without creating VT.
Mandatory demux, manifest, packet timing/hash and public CoreMedia format checks always run. Target use is
`video-v6 --qualify FIXTURE_DIR`, where the fixture directory is `tests/fixtures/v6` in this package. It requires hardware
and PV, compares each callback token/OutputPTS/active NV12 hash to independent software truth, checks mid-GOP drain/resume,
no-output reference decoding, cancel/recreate and retained surfaces. Seven producer credits leave one ACK-overlap credit.
Named phases stop after the first failed phase and its cleanup; no later hardware phase is attempted.
`tests/stream_host.cpp` exercises the real HFI parser, ownership ledger, guest deadlines and failure transitions with a
scripted peer. Host results establish contracts; only tablet results can qualify continuous decode or performance.
