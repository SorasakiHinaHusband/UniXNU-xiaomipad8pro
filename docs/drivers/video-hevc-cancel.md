# ram16g-hevc1: pending AVC cancellation

Status: **HOLD**. Host-only analysis of the 2026-11-26 run. No tablet
operations, driver changes, or timeout changes are authorized by this report.

## Established evidence

Evidence names below refer to the archived `ram16g-hevc1` run, without its
private storage location. Source for the image is commit `1da1485af7`
(`run.log:2`); the analysis checkout starts at `3fcf74f4`. The qualification
program is `tooling/native/video-v6.mm`, not
`package/PianoVideo/tests/video-v6.mm`. The image revision has the mixed HEVC
implementation; this checkout does not have all of those changes. Read the
image revision with `git show 1da1485af7:<repo-relative-path>` when comparing
its mixed backing.

- `RESULT.md:134-145`: the AVC invocation lacks its exit marker after twelve
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

The kernel records place the cancel close near uptime 587 seconds, only a
few seconds after tool start. The 360-second timeout was reached much later.
The added HEVC qualification phases did **not** consume this AVC invocation's
budget. HFI close and the frontend worker join demonstrably returned. The
unresolved wait is above that completed teardown; its precise public VT call
and cause require further evidence. In particular, a queued transport reply
does not prove that Apple's decoder consumed it or retired its pending work.

## Fix-image gate

Path: **HOLD**, analysis only; pending-cancel completion above the successful
native close. No fix image is justified by the current evidence.

Checks: Preserve the image and tool pins; compare the same pending-cancel
phase on `ram16g-hctl1`, with timestamps around invalidate and release and
evidence for outstanding guest tokens. Exact timing and full source-path
checks are being reconciled against the archived logs.

Root cause: The outer wrapper expired, but HEVC budget exhaustion and a stuck
HFI close are contradicted by the transcript. The remaining VT completion
wait is not yet attributed to a proven, host-reproducible code defect.

ONE change: None pending the `ram16g-hctl1` control and a deterministic
regression that reproduces the actual outstanding-completion contract.
