# Installed Metal 3D sustain test

Goal 12 calls for a real 3D workload on the installed system, with frame-level timing and a sustained run free of GPU hangs and resets. The November 23 RAM run used the historical `piano-gpubench` app on `gpu/bench3d-1` (`3f1217fa`), at 3200×2136 with 4,096 lit icospheres. Its 600-second pass reported 51.78 completed frames/s and a 25.31 ms completion p99. It spent 534 of 600 seconds at 734 MHz. Drawable callbacks supplied zero presentation timestamps, so those figures describe rendering throughput. See [status](../STATUS.md) and [presentation feedback](../drivers/presented-time.md).

| Existing source | Role in the installed test |
| --- | --- |
| `tooling/native/piano-frametime/piano-frametime.m`, `scene.h`, `Makefile` | AppKit and Metal setup, an embedded shader, drawable callbacks, and a single arm64 executable. |
| `tooling/native/gpu-qualification-load.m`, `gpu-qualification-pixels.m` | Existing composition load and separate pixel checks for the GPU frequency qualification. |
| `package/PianoA830/board/PianoA830.cpp:1647–1682`, `package/PianoA830/adreno830/Adreno830Start.cpp:2958–2999` | Read-only `machdep.piano_gpu_state` publishes the voted GMU level, MHz, ceiling, and binding limits. |
| `package/PianoA830/board/PianoA830.cpp:1356–1383` | Read-only `machdep.piano_gpu_recovery` exposes recovery, watchdog, and fault counters. |

The CLI is explicitly launched by the operator and consumes the installed driver's existing interfaces. There is no driver port or boot argument for this test.
