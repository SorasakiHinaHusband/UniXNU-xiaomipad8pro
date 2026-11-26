# ram16g-hevc1: pending AVC cancellation

Status: **HOLD**, analysis only. The 2026-11-26 failure is an unfinished
pending-cancel operation above a **completed native teardown**, followed by a
host SSH timeout. It is **not HEVC phases exhausting the AVC time budget**.
The exact public VT wait and its causal defect remain unproved. No code,
image, harness budget, or tablet state was changed for this analysis.

## Established evidence

Evidence names below refer to the archived `ram16g-hevc1` run, without its
private storage location. Source for the image is commit `1da1485af7`
(`run.log:2`); the analysis checkout starts at `3fcf74f4`. The qualification
program is `tooling/native/video-v6.mm`, not
`package/PianoVideo/tests/video-v6.mm`. The image revision has the mixed HEVC
implementation; this checkout does not have all of those changes. Read the
image revision with `git show 1da1485af7:<repo-relative-path>` when comparing
its mixed backing.

- `RESULT.md:130-140`: the AVC invocation lacks its exit marker after twelve
  phase passes; the separate HEVC invocation passes with 24 callbacks.
- `ram16g-hevc1.spec:2018`: `SSH_T=360` wraps
  `video-v6 --qualify <fixtures>; echo V6_TOOL_EXIT=$?`.
- `ram16g-hevc1.spec:2031`: a subsequent, separate `SSH_T=420` invocation runs
  `video-v6 --qualify-hevc <hevc-fixtures>; echo HEVC_TOOL_EXIT=$?`.
- `rx-v6.txt:1`: tool start is monotonic `583841549` microseconds.
- `rx-v6.txt:1949-1988`: `cancel-recreate` creates epoch 20, accepts eight
  samples, has four pending public callbacks, then stops printing during its
  close helper. The last line is the host wrapper's `Alarm clock: 14`.
- `rx-v6-console.txt:12933-12995`: `dinv` request 115 reaches the frontend;
  both HFI STOPs and CLOSE succeed; session `56360012` closes with result 0,
  seven ETBs/EBDs/decoded pictures, zero loans/watchdog/fault bits; the worker
  reports four cancellations and zero maps; `invalidate safe worker-joined 1`
  is printed and a successful `dinv` reply is queued. A subsequent `dfin`
  request 116 receives `-12903` and its reply is queued.

Source line numbers below are at `1da1485af7` unless explicitly identified as
the analysis checkout. The cancel worker/teardown and `dfin`/`dinv` handlers
are unchanged between those revisions; the HEVC capability and copy/backing
changes must be read at the image revision. No mixed implementation was
imported into this branch.

## Time budget and phase accounting

The host wrapper is `tmo.sh:3`:

```sh
s=$1; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"
```

The ordinary `rssh` path in `tabletrun.sh:186-187` invokes that wrapper with
`SSH_T`, then suppresses its failure with `|| true`. Thus the 360-second
alarm applies to the **host SSH command**, not directly to the remote
`video-v6` process. `Alarm clock: 14` identifies host SIGALRM; it is not an
Iris watchdog or a video process exit status. The grading check at spec line
2020 only requires an exit marker with digits; its absence is the single
FAIL. It does not by itself establish where the remote process blocked.

There is also a **300-second process alarm** in `video-v6.mm:531`, with a
handler at line 35 that writes `FAIL process-timeout status=124` and exits
124. Neither that message nor `V6_TOOL_EXIT` was received. An unlabelled
`invalidate safe` line between kernel uptime 883.784 and 883.848 seconds
(`rx-v6-console.txt:19931-19943`) is 299.942451–300.006451 seconds after
tool start. This is consistent with process-alarm cleanup, but is not
PID-labelled proof of process death. The SSH connection can outlive the
remote process; the missing output/exit status remains an observation gap.

The host `run.log` records payload verification at **23:23:07** and the
post-AVC verdict at **23:29:10**, a **363-second command block** including
mark/health/SSH setup. The configured wrapper lifetime is **360 seconds**.
There is no timestamp on wrapper launch, `cancel-begin`, or the alarm line,
so an exact process runtime or exact cancel-to-alarm duration cannot be
recovered. Calling 363 seconds the decoder runtime would be incorrect.

These are the twelve passed phases, in order (`rx-v6.txt`):

| Phase | Lines | Recorded duration / limit |
| --- | --- | --- |
| `rejection-contract` | 2–7 | No individual clock stamp |
| `manifest` | 8–10 | No individual clock stamp |
| `demux-ip` | 11–19 | No individual clock stamp |
| `demux-ibp` | 20–28 | No individual clock stamp |
| `audit-ip` | 29–36 | No individual clock stamp; private serializer unavailable |
| `audit-ibp` | 37–44 | No individual clock stamp; private serializer unavailable |
| `decoder-spec` | 46–48 | No individual clock stamp |
| `single-regression` | 49–97 | Create **154.002 ms**; valid golden callback **31.266 ms** latency; no whole-phase timer |
| `ibp-async` | 98–561 | Create **93.058 ms**; summary **700.245 ms**, including create |
| `ip-temporal` | 562–1023 | Create **81.942 ms**; summary **681.812 ms**, including create |
| `ibp-temporal` | 1024–1485 | Create **76.806 ms**; summary **672.771 ms**, including create |
| `ibp-no-output` | 1486–1948 | Create **86.219 ms**; summary **674.951 ms**, including create |

The first seven phases together finish before the first native open at
583.974527 seconds, **less than 132.978 ms** after the tool's start stamp.
That bound also includes part of single-regression creation; it is not a
measurement of each preflight phase. The four stream summaries total exactly
**2.729779 seconds**. Their timers start before create and end after the
finish/wait barriers, before `close()` and its 20 ms observation sleep
(`video-v6.mm:384-410`). Adding create times again would double-count them.

Native opens give a separate check on whole-cycle progress. These are exact
**open-to-next-open** differences, not phase begin/end durations:

| Session / phase | Native open, seconds uptime | Until next native open |
| --- | ---: | ---: |
| `5636000d` / single | 583.974527 | 213.644 ms |
| `5636000e` / ibp-async | 584.188171 | 745.262 ms |
| `5636000f` / ip-temporal | 584.933433 | 729.965 ms |
| `56360010` / ibp-temporal | 585.663398 | 720.833 ms |
| `56360011` / ibp-no-output | 586.384231 | 721.279 ms |
| `56360012` / cancel-pending | 587.105510 | Subsequent public recreate never appears |

The `V9g native open` stamps are in `rx-v6-console.txt` at lines 98, 496,
3529, 6538, 9548, and 12537. They confirm seconds, not minutes, of preceding
work.

The thirteenth phase, `cancel-recreate`, creates epoch 20 in **85.647 ms**
and accepts eight samples with flags 9 (asynchronous plus temporal
processing), with four public callbacks and four still pending. Its last
public line is `cancel-had-pending`; `cancel-joined`, `recreate-at-idr`, and
the qualification verdict never appear.

| Event | Uptime / elapsed from start | Evidence and precision |
| --- | --- | --- |
| Tool starts | 583.841549 s / 0 | Exact `stamp-us`, `rx-v6.txt:1` |
| Cancel first native input | 587.201774 s / 3.360225 s | Exact V9j3 stamp, console slice line 12731 |
| `dinv` received, both STOPs and HFI CLOSE acknowledged | After that input, before 587.256086 s / before 3.414537 s | Ordered records, slice lines 12933–12963; CLOSE precedes the GPU clock marker |
| Native close, worker join, successful `dinv`, then `dfin=-12903` | After 587.257738 and before 588.145615 s / 3.416189–4.304066 s | Neighbouring clock markers; slice lines 12973–12996 |
| Possible process-alarm cleanup | 883.784–883.848 s / about 300 s | Unlabelled teardown; inference only |
| Host alarm | 360 s after host wrapper launch | Alarm line has no clock stamp |

On the approximation that remote start and host wrapper launch coincide,
cancel-to-host-alarm is **356.585–356.640 seconds**, and cancel-to-the-inner
300-second alarm is **296.585–296.640 seconds**. These are estimates: there
is unmeasured SSH/startup time, and `cancel-begin` itself is unstamped. The
hard result is that native teardown completed with **over 350 seconds of
the nominal outer budget still available**. The transcript's adjacent
`cancel-begin` and alarm lines conceal several minutes without public output.

### Where the HEVC work actually ran

No HEVC phase ran inside the failing `--qualify` process (PID 3876).
`video-v6.mm:508-525` selects the AVC phases above; HEVC has its own mode
dispatch and phase sequence at lines 482–505 and 532–541.

There are four one-frame HEVC sessions in the AVC **console window**,
`56360013`–`56360016`, selected for **PID 2783**, starting at uptime
819.874640 seconds, long after the pending-cancel teardown. They close
normally by approximately 821.6 seconds. They are another process's work,
not extra qualification phases before cancel. They also show that the
native session lease and HFI remained usable while the public test stalled.
The window's 252 submits are consistent with 1 single + 240 streamed AVC +
7 cancel AVC + these 4 HEVC submits.

The separate RXH command uses PID 3932 and starts after the outer timeout.
`rx-hevc.txt` records `hevc-fixture` (lines 2–49), `hevc-first` (51–149),
and `hevc-reopen` (150–245). The two creates take **160.286 ms** and
**105.447 ms**. No whole-phase durations are printed. Native opens are
947.425155 and 948.220887 seconds, **795.732 ms** apart; final close lies
after 948.850 and before 949.371 seconds (`console.out:45857-45884`).
Both sessions return all twelve P010 hashes and close cleanly;
`HEVC_TOOL_EXIT=0`. The entire post-AVC-to-HEVC-verdict block is about six
seconds in `run.log`, including grading/capture. Its 420-second budget is
independent of the failing AVC command's 360 seconds.

## End-to-end path and checks

`ours-OK` means the indicated source check exists and/or the stated run
witness is present. It does not imply that Apple's private decoder contract
is validated. `ours-missing` identifies an absent check, disposition, or
measurement; `unknown` identifies behavior not established by this capture.

The public caller uses Apple's built-in paravirtual decoder, advertised as
`com.unixnu.pianovideo.decoder.avc`. Our endpoint is the kext service
`AppleVideoToolboxParavirtualizationDriver` and its
`AppleVideoToolboxParavirtualizationUserClient`. There is no separate
`PianoVideoUserClient` class or repository-owned userspace VT decoder plug-in
on this path (see [video.md](video.md), sections on V0b/V5a).

| Hop / state | Check and observed result | Classification |
| --- | --- | --- |
| `video-v6.mm:329-344,429-436`: require hardware/PV, submit epoch 20 | Hardware required and reported; eight submissions return 0; pending=4 captured under the context mutex, then mutex released before close | **ours-OK** |
| `close()`, lines 375–378 | Calls `VTDecompressionSessionInvalidate`, then locks context/releases retained surfaces, calls `CFRelease`, sleeps 20 ms, checks callback stability | **ours-missing** timestamps/return markers between these operations; **unknown** which public call or release blocks |
| Apple's PV decoder → wire `dinv` request 115 | AVC selected for the right PID; `dinv` reaches our transport | **ours-OK** wire arrival; **unknown** internal VT cancellation/join and pending-token accounting |
| `PianoVideoVTParavirt.cpp:255-305,322-365`: selector 3 | Bounded snapshot, wire/atom shape, owned UUID and surface IDs checked; dispatch returns status 0 | **ours-OK**; no envelope/ownership rejection on this request |
| `PianoVideoVTHost.cpp:838` → `teardown():618-627` | Sets invalidating/stopping under lock, wakes worker, sleeps until workerRunning=false, closes residual native/surface owners | **ours-OK** joined in this run; **ours-missing** independent deadline on the worker join, which is not the observed long wait |
| Worker cancellation, lines 571–604 | Cancels guest request; synchronous originals and held admissions get error replies; already accepted asynchronous records get no error `Demt` during invalidation (`if (!invalidated)`); then `frames.cancel()` clears local pending | **ours-OK** local stop/no-late-success fence; **ours-missing** terminal notification for four accepted async tokens; **unknown** whether Apple cancels them itself |
| Surface/RAW/input cleanup, lines 605–612 | Cancel native pump, close surface client, release outstanding RAW loan, free staged input buffers, close native session | **ours-OK**: final lookup/invalidate/release counts 5/5/5, held=0, maps=0, open/close=1/1 |
| `PianoVideo.cpp:1120-1123`: `native_cancel` | Matching session marks tx.cleanup; it does not wait for drain or firmware EOS | **ours-OK** cancellation path; no HFI flush command required here |
| `native_frame_release():1111-1118` / native owner gate | Descriptor loan released before close; `closing(sid,loans)` refuses foreign sessions or outstanding loans | **ours-OK**: native close reports loans=0 |
| `native_close():1147-1190` | Checks owner/tx/core error, watchdog, real fault bits, quarantine; cancels tx ledger and calls `shutdown(true)` only on clean path | **ours-OK**: result=0, watchdog=0, fault-bits=0, no quarantine |
| `VideoSession.h:694-709,906-950`: STOP RAW, STOP input, CLOSE | Response-required commands; each wait defaults to 1000 ms, pumps HFI, checks first error, fails closed; one bounded late-message pump | **ours-OK**: STOP 1/1, CLOSE 1; **unknown** unexecuted error branches on this hardware |
| HFI buffer ledger, lines 951–970 | Successful fail-closed CLOSE clears queued/readonly/pending-release ownership; logged queued count is collected **before** clearing | **ours-OK**: nine entries explicitly `released-on-close 1`, readonly=0, pending=0; queued=9 is not a leak |
| Resident mixed arena / native lease | Boot backing and SMMU stay resident; NativeOwner refers to global gBringup; successful close deletes only owner and returns lease to Idle | **ours-OK**: arena-retained=1, dma-held=1 are intentional; next native session opens with previous-close=0/previous-teardown=0 |
| Worker exit → `dinv` response | Sets native=0, workerRunning=false and wakes join; handler queues status 0 after join | **ours-OK**: `invalidate safe worker-joined 1`, request 115 reply enqueued, no transport DROP |
| Post-invalidation `dfin` request 116 | `VideoStreamState.h:13-19` returns BadState while invalidating; frontend replies -12903, queue enqueue succeeds | **ours-OK** implemented state check; **unknown** guest-side consequences with four outstanding tokens |
| Shared reply queue → public VT completion → destroy/recreate | Queue notification/selector-4 acknowledgements exist; no `-dec` for this session or public `cancel-joined`/epoch 21 before stall | **ours-missing** token-specific guest retirement/receipt evidence; **unknown** public wait/deadlock and exact process lifetime |

### Mixed backing release is not the missing completion

At the image revision, `VideoMixedArena.h:25-37` allocates a 684 MiB arena
as **274 MiB from region0 plus 410 MiB from XNU**. `PianoVideo.cpp:591-640`
checks slabs, builds the two-owner descriptor, validates each physical page
and uniqueness, and creates one CPU alias; boot alias self-test passes.
`loanArena():643-656` validates a descriptor crossing the seam. Resident
session prepare at lines 993–1008 zeros the active layout and cleans the
mixed Normal-memory alias to PoC. These are **ours-OK** source checks, with
boot self-test and later 4K golden/reopen witnesses in this run.

Successful session close relinquishes firmware ownership, not the boot
reservation. It neither returns region0 slabs nor unmaps/reallocates the
mixed arena. Physical destruction is `KernelHw::dmaFree():846-864`: release
CPU/multi descriptors, complete/release XNU/extra owners, give slabs back,
then release UC/table/ledger owners. The full-core teardown gates that call
on PAS shutdown, legal SMMU detach/TLB completion and power-off
(`VideoBringup.h:435-503`); quarantine retains dependencies and DMA. Those
guards are **ours-OK** by inspection; actual physical mixed-arena free is
**unknown/not exercised** in the successful resident close. There is no
evidence of a wait in this allocator/destructor during epoch 20.

The native count of two cancelled records and frontend count of four refer
to different ownership ledgers. Eight accepted public tokens need not mean
eight ETBs: token 8 was admitted but cancellation prevented its hardware
submit. Seven ETBs/EBDs/decoded pictures and only four public callbacks are
consistent with the captured pipeline. Firmware completion alone does not
retire the remaining public tokens.

## Root-cause boundary and reference comparison

**Confirmed:** the host wrapper reached its timeout while pending-cancel
never completed publicly. HFI STOP/CLOSE, descriptor cleanup and frontend
worker join had already returned within the first 4.304066 seconds. No
PianoVideo HFI timeout, watchdog, fault, transport drop or quarantine appears
for this cancel session. The matching full-console witness is
`console.out:36079-36142`, including successful close at line 36127.

**Leading hypothesis:** accepted async tokens 5–8 remain outstanding in
Apple's PV/session machinery after our invalidation path clears its local
ledger without terminal `Demt` dispositions. All eight admission replies
were sent/published; only tokens 1–4 emitted successful `Demt` and received
public callbacks. Token 5's surface lookup completed, then cancellation
disposed it without a public completion. This is a concrete source/evidence
gap, but there is no captured Apple thread stack or deterministic host peer
that proves it causes the wait.

`dfin=-12903` alone is **not** a diagnosis: the same post-`dinv` reply is
followed by normal `-dec` destruction in earlier successful AVC sessions
and later HEVC sessions. Do not change it to success simply to hide the
state transition. Also do not drain or wait before invalidating: that
would remove the pending-work case the test is meant to exercise.

The reference was checked locally at Ubuntu **v1.4.0**, repository revision
`5b7b8a5cada36c43eed2cc153ab430bea69c6b00`, whose `kernel/source.json` pins
Linux `83f0f89bed14a0cb7d582a26e72bced0387870f4`. In that exact kernel:

- `drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c:866-891,1149-1175`
  sends response-required CLOSE/STOP; STOP waits for a bounded response.
- `iris_utils.c:70-86` releases the instance mutex around
  `wait_for_completion_timeout`, retakes it and reports timeout as error.
- `iris_common.c:201-234` stops the port and flushes deferred buffers even
  on the error path. `iris_vb2.c:231-256` then completes remaining client
  buffers with `VB2_BUF_STATE_ERROR`; `iris_utils.c:55-68` removes each
  queued buffer and calls `v4l2_m2m_buf_done`.

Our observed HFI close is consistent with those bounded teardown semantics.
The reference also separates hardware stop from client-buffer disposition;
our accepted-async invalidation path lacks an equivalent explicit terminal
notification. Linux's V4L2 completion contract does not establish Apple's
private PV invalidation contract, so it supports investigation, not a
blind port or a verified fix. No copied reference code is part of this change.

## Control and next deciding evidence

`ram16g-hctl1.spec` already pins the **same tool** (`b0072c969490…`) and
payload (`252cb0d465e6…`), uses the **same 360-second AVC / 420-second HEVC
budgets**, and sets `piano-extra-ram=0`, `piano-video-extra-ram=0`,
`piano-video-extra-ram-hevc=0`. Its separate HEVC command is
`--expect-hevc-decline`. No archived hctl1 RESULT/run/transcript was found
in the supplied dated run directory. Nothing was run on the tablet here.

The deciding control is the unchanged `video-v6 --qualify <fixtures>` on
**ram16g-hctl1**, retaining those pins and budgets. Require
`cancel-joined`, `recreate-at-idr` with correct hashes and epoch separation,
`phase-pass name=cancel-recreate`, `PASS qualification`, and
**`V6_TOOL_EXIT=0`** (the existing digits-only exit check is insufficient).
Compare accepted/pending token counts at invalidate, not only total phase
counts. A control that happens to have zero pending work does not exercise
this failure.

If hctl1 stalls with the same clean close and four missing dispositions,
mixed backing is excluded and the frontend/PV cancellation contract becomes
the target. If hctl1 passes, compare that exact pending case against the
mixed run; a pass alone does not prove mixed memory is defective. Neither
outcome retroactively makes the HEVC phases part of the AVC budget.

A future diagnostic capture needs monotonic markers immediately before and
after **invalidate, retained-surface release, and CFRelease separately**,
plus the blocked public thread stack, terminal disposition for every
accepted token, and independent process exit/status capture across the
300/360-second alarms. This would distinguish a VT join from a release
wait and from lost SSH output. These are proposed checks, not changes made
or tablet operations performed by this job.

Only after that evidence identifies a causal code change should a regression
exercise the real cancellation/completion boundary: eight accepted async
requests, four retired/four pending, cancellation during Dbuf, one terminal
disposition per token, no late success, joined close, and epoch-21 reuse.
A test that only asserts local `Ledger::cancel()` sets pending=0 would
repeat the implementation and cannot validate this hypothesis.

## Host validation

Existing `session_host`, `stream_host`, `vt_sync_host`, and `vt_surface_host`
were compiled with C++20, `-O2 -Wall -Wextra -Werror` and all passed in the
analysis checkout. They cover scripted HFI stop/close, ownership, sync
completion and surface cancellation. They do not run Apple's PV decoder or
prove public pending-async cancellation completes. An independent arithmetic
check confirmed twelve phase passes, no HEVC phases in `rx-v6.txt`, and the
2,729,779 microsecond stream-summary sum, eight published cancel admissions,
four public completions and seven cancel ETBs. No implementation or test
source was changed. `git diff --check`, full diff review, and checks for
private host paths, network addresses and secret assignments passed;
tracked content contains only sanitized evidence labels and source pins.

## Fix-image gate

Path: `video-v6 cancel-recreate` → public VT invalidate/release → Apple's PV
decoder → our paravirtual user client → frontend cancellation/worker join →
native loan release → HFI STOP RAW/STOP input/CLOSE → retained mixed arena →
successful `dinv` reply → unresolved public pending-token retirement and
destroy/recreate. **HOLD**, analysis only.

Checks: **ours-OK**: hardware/PV admission, bounded transport validation,
surface/RAW cleanup, clean HFI close, worker join, reply enqueue and resident
backing/lease reuse. **ours-missing**: terminal dispositions for accepted
async tokens on invalidate, per-call close timestamps, guest retirement
evidence and an independent worker-join deadline. **unknown**: Apple's
pending-cancel contract, exact blocked public call, remote exit/output
delivery and physical arena destruction in this resident run. Four existing
host tests pass; they do not reproduce this public wait. Keep the image/tool
pins and 360/420-second budgets for the `ram16g-hctl1` pending-work control.

Root cause: **Partially closed**: pending cancellation stops progressing
publicly after successful native teardown at elapsed 3.416–4.304 seconds;
the host SSH wrapper later reaches its 360-second alarm. Added HEVC phase
budget exhaustion is ruled out. Missing async terminal notification is the
leading hypothesis, not a confirmed host-reproducible defect or proof of an
infinite VT deadlock. HOLD until `ram16g-hctl1` and the deciding completion
evidence distinguish the remaining causes.

ONE change: **HOLD — no production change, timeout increase or fix image**.
Keep this commit documentation-only; the unchanged `ram16g-hctl1` control
and a regression of the actual pending-token contract must justify one
minimal default-safe cancellation fix before implementation.
