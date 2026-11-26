# Installed Metal 3D sustain test

Goal 12 calls for a real 3D workload on the installed system, with frame-level timing and a sustained run free of GPU hangs and resets. The November 23 RAM run used the historical `piano-gpubench` app on `gpu/bench3d-1` (`3f1217fa`), at 3200×2136 with 4,096 lit icospheres. Its 600-second pass reported 51.78 completed frames/s and a 25.31 ms completion p99. It spent 534 of 600 seconds at 734 MHz. Drawable callbacks supplied zero presentation timestamps, so those figures describe rendering throughput. See [status](../STATUS.md) and [presentation feedback](../drivers/presented-time.md).

| Existing source | Role in the installed test |
| --- | --- |
| `tooling/native/piano-frametime/piano-frametime.m`, `scene.h`, `Makefile` | AppKit and Metal setup, an embedded shader, drawable callbacks, and a single arm64 executable. |
| `tooling/native/gpu-qualification-load.m`, `gpu-qualification-pixels.m` | Existing composition load and separate pixel checks for the GPU frequency qualification. |
| `package/PianoA830/board/PianoA830.cpp:1647–1682`, `package/PianoA830/adreno830/Adreno830Start.cpp:2958–2999` | Read-only `machdep.piano_gpu_state` publishes the voted GMU level, MHz, ceiling, and binding limits. |
| `package/PianoA830/board/PianoA830.cpp:1356–1383` | Read-only `machdep.piano_gpu_recovery` exposes recovery, watchdog, and fault counters. |

The CLI is explicitly launched by the operator and consumes the installed driver's existing interfaces. There is no driver port or boot argument for this test.

## Build and launch

```sh
make -C tooling/native/piano-metal-sustain SDK=macosx
bench=out/piano-metal-sustain/piano-metal-sustain
results=$(mktemp -d /tmp/piano-metal-sustain.XXXXXXXX)
"$bench" --offscreen --seconds 12 > "$results/offscreen.csv" 2> "$results/offscreen.log"
"$bench" --onscreen --seconds 12 > "$results/onscreen.csv" 2> "$results/onscreen.log"
```

The single executable compiles the shader at runtime. Both modes render an orbiting view of a tiled arena with animated, textured crates, diffuse and specular lighting, and a depth attachment. The default 1920×1280 render has 256 separate indexed crate draws and one floor draw, for 442,370 submitted triangles per frame at one sample per pixel. Use `--width`, `--height`, and `--draws` to vary the load. The qualification keeps those defaults fixed. Offscreen mode submits continuously to a private render target. Onscreen mode uses a display-synchronized `CAMetalLayer` in an AppKit window on the main display, with up to three submitted commands in flight. Launch onscreen mode in the non-root console user's GUI session with the window visible and the display unlocked. Use `--minutes N` for a soak. The two-second warmup precedes the measurement in both modes.

## Frame and frequency evidence

- Stdout contains one CSV row per measured submission with CPU frame begin, encode begin, submit, Metal `GPUStartTime` and `GPUEndTime`, completion callback, `presentedTime`, presented callback, and command status. Times are host monotonic seconds. Empty cells mark unavailable timestamps. A presented callback with an empty `presented_s` supplied a zero or invalid timestamp. Offscreen mode has no drawable callbacks.
- Stderr gives the capture bounds and one `WINDOW` row per ten-second submission window, including a partial final window when needed. Each row gives nearest-rank p50, p95, p99, maximum, and samples over 50 ms for CPU submit cadence, CPU encode duration from drawable acquisition to commit, completion cadence, GPU execution duration and end cadence, and drawable presentation cadence. Cadence includes the interval crossing each window boundary. Windows are assigned by CPU frame begin and reported after up to two seconds for callbacks. `render_fps` counts distinct successful completion callbacks per window second. `present_fps` uses distinct positive drawable timestamps and is `na` when none are available. Retain the per-frame CSV to inspect individual stalls and distinguish GPU execution from host callback timing.
- The tool polls `machdep.piano_gpu_state` every 500 ms under load and records fresh driver snapshots as `GPU_STATE`, once per driver timestamp. `WINDOW` gives the sample count, min/max level, and max MHz, with `na` for unavailable state. The state reports the driver's voted level. It also carries the current cap, binding cause, and temperatures. Keep the original rows. Frequency sysctls are ordinarily unavailable on a Mac.
- `METAL_SUSTAIN_COMMAND_ERROR` is a command-buffer failure and makes the process exit nonzero. A queue or drawable timeout also fails the run. A watchdog exits if no command completes for 15 seconds. Onscreen rendering can complete on a system that supplies zero `presentedTime` values. In that case report Metal completion fps and frame times, and record presentation cadence as unavailable.

## Installed-system qualification

Use the installed boot's existing policy and the active USB console recorder. Allow at least 900 seconds for the desktop gate, smoke, capture and verified transfers after boot. Run after the normal desktop and lit-panel gate used by the main session. The block stages a hash-checked binary in a new remote temporary directory and checks the same windowed workload for three seconds before proceeding. It brackets the long process with unique `/dev/console` markers and brackets the run with boot UUID, GPU state, and GPU recovery counter reads. It retains the remote directory for re-pull and writes the host result to `$R/metal-sustain/`.

Add these lines to the installed-system runner's check phase. They use `rssh`, `rsudo`, `pull`, `lines`, `mark`, `waitfor`, and `verdict` from `tabletrun.sh`, as used in `v131y12s-qual1.spec`.

```sh
. tooling/native/piano-metal-sustain/installed.spec
metal_sustain_installed
```

The long run invokes the staged binary with `--onscreen --minutes 10 --width 1920 --height 1280 --draws 256` via `launchctl asuser` and `sudo -u` for the console UID. The exact privileged reads at both bounds are:

```sh
rsudo 'printf "BOOT="; /usr/sbin/sysctl -n kern.bootsessionuuid; /usr/sbin/sysctl -n machdep.piano_gpu_state; /usr/sbin/sysctl -n machdep.piano_gpu_recovery'
```

The offline grader requires all 60 windows and the process exit receipt, unchanged boot UUID and recovery/fault/watchdog counters, and both console markers with an in-window GPU heartbeat and no hang, recovery, reset, or loss line. Every window needs matching CSV and summary counts, positive Metal GPU timestamps, and GPU frequency state. A sampled level 3 or below at the pre-run read or during the run must precede a sampled level above 3 during the run. The provisional performance floor is **at least 30 completed frames/s and at most 40 ms completion p99 in every 10-second window**. The grader recomputes those metrics from per-frame completion timestamps, reports >50 ms stalls, and retains presentation even when the driver provides none. Record the thresholds and scene settings when comparing results. The RAM benchmark used different geometry, draw counts, and resolution.

Regrade a saved result locally with `python3 tooling/native/piano-metal-sustain/grade.py "$R/metal-sustain"`.

## Host verification

The arm64 build and ad hoc signature verified on the M5 Pro host with SDK 27.2. The background host process returned no Metal device, and the onscreen launch found no logged-in console session. Neither launch produced real GPU frame timings in that environment. The formatter and offline grader passed a 60-window fixture and rejected a missing frequency step, low fps, high p99, a reset, changed recovery counters, a command error, a missing console marker, and missing frame evidence. The installed smoke is the hardware test of shader compilation and frame capture on Adreno.
