# Audio duplex and input wake analysis

2026-11-27; source baseline `lead/v131y12t`, `3fcf74f45`.
Host-side analysis only. No driver modification, image, tablet access or push.
This analysis covers daily-use Goals 6 and 7. Source references below refer to
the baseline, unless explicitly qualified as a reference revision.

## Evidence and scope

- The owner's installed macOS 27.3 report establishes an audible duplex problem;
  it does not identify the installed KC, runtime switches or a failure layer.
- The saved `aud-mic4` score and original console establish two distinct input
  runs: 469 late periods / 8293 us maximum, then 25 / 7374 us. Both have zero
  post-startup missing PCM. The often quoted 25 / 7.4 ms describes the restart,
  not the long capture. This was a RAM diagnostic run, not installed acceptance.
- The saved November 24 audio analysis reports an earlier duplex defect fixed
  since v13.1p, with signed sound age enabled on its observed installed y12c.
  Later silent installs skipped audible duplex acceptance. Historical acceptance
  cannot establish the behavior of the owner's current installed boot.
- Linux/Ubuntu v1.4.0 is available locally: mainline project
  `9f4b92899a7114c6af70a9ed64cd15a3f9b788f5`, whose `kernel/source.json` pins
  Linux `83f0f89bed14a0cb7d582a26e72bced0387870f4` (also kernel tag v1.4.0).
  These exact revisions, rather than the newer checkout HEADs, are the reference.
  GPL sources are read as behavioral specifications; no Linux implementation is
  copied. `unixnu.lock` does not pin this Ubuntu release; the older references in
  `docs/drivers/audio.md` and `package/PianoAudio/UPSTREAM` are distinguished.

## Findings established so far

1. The two macOS devices use IOAudio2's mapped rings and independent kernel
   engine threads, not an IOAudioEngine workloop or AudioDriverKit callback
   (`package/PianoAudio/PianoAudioDevice.cpp:3`, `:131`, `:511`). Their period
   locks are separate. `gTransportLock` serializes start/stop notifications,
   not every playback/capture period (`:136`, `:542`, `:553`).
2. A real, historical duplex trigger is statically reproducible: the worker
   samples `now` before draining capture completions, while the output engine
   can publish a newer `sound_at`. Unsigned subtraction falsely declares
   silence and starts a fade. The signed-age remedy is **already present**
   (`package/PianoAudio/AudioDsp.cpp:613`, `:633`;
   `package/PianoAudio/AudioLive.c:200`, `:727`, `:1081`, `:1716`).
   `tests/live_host.c:2284` already reproduces old-versus-signed behavior.
3. Input `late-periods` measures the time after the deadline wait **and after
   reacquiring the device lock**, but before capture. It increments only above
   one entire 5 ms batch (`PianoAudioDevice.cpp:532`). No thread scheduling
   policy is set at engine creation (`:485`). This exposes a scheduling
   hypothesis, not a measured lock hold or a proven timer defect.
4. DSP control-plan serialization does not serialize all PCM completions behind
   a command's answer. Speaker and capture pumps send asynchronously
   (`AudioLive.c:393`, `:1126`, `:1553`). A synchronous bus operation on the shared
   worker can still delay both pumps; its duration must be distinguished from
   device-lock contention.

Further source/resource accounting, latency arithmetic, reference comparisons
and exact observation criteria follow in the completed analysis.

## Fix-image gate

Path: IOAudio2 Speaker/Mic mapped rings -> independent host engine threads ->
AudioLive FIFOs -> shared worker/inbox -> GPR/GLINK -> AudioReach playback and
capture graphs -> TDM/FS19xx and VA/DMIC; protection adds the V/I graph and monitor.

Checks: Separate device locks and asynchronous PCM pumps are ours-OK by source;
historical unsigned age is already corrected by default. Installed provenance,
current dropout classification and the input wake delay source remain unknown.

Root cause: Historical false-silence fade is established, but already fixed in
this baseline. Current installed stutter and input wake latency are not closed
by the available measurements. Neither justifies another implementation image.

ONE change: HOLD. Complete this static audit and obtain the named read-only
observations from the installed failing boot before selecting a driver change.
