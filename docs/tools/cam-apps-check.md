# Camera app verification on macOS 27.3

Build the local arm64 probe before running the harness action.

```sh
mkdir -p out/piano-cam-apps-check
bash tooling/native/piano-cam-apps-check/build.sh out/piano-cam-apps-check/cam-apps-probe
```

Register `verify cam-apps-check` and its fixed step list with the harness, then source the fragment from the approved actions
file. Dispatch that verify step to `cam_apps_check_installed` after the harness desktop gate. Use `BOOT=installed`,
`KIND=observe`, and `REC_C920=0`. The action runs in the invoking machine's GUI session. The harness owns run scheduling
and the results directory `R`. Set `CAM_APPS_BIN` if the probe was built elsewhere.
[Spec fragment](../../tooling/native/piano-cam-apps-check/cam-apps-check.spec)

The local console needs macOS 27.3, an unlocked desktop, both PianoCamera devices idle, and both apps closed. The harness
identity needs Accessibility and QuickTime Player Automation approval. Prepare the apps with existing camera permissions,
FaceTime's front camera selected, and QuickTime's High quality preset. Account setup and permission prompts require prior
configuration outside this check.

- **FaceTime** opens the app, observes its window, and waits for a single front-camera stream with increasing source and
  delivered-frame counters. The probe reads the window and requests an ordinary quit.
- **QuickTime** presses the New Movie Recording menu item through Accessibility. Once live counters advance, it records
  for about five seconds, saves a fresh movie, then quits and reopens the exact file in QuickTime Player. The muted
  playback clock must advance by more than 1.5 seconds over about three seconds. AVFoundation checks the QuickTime
  container and H.264 track. If its decoder is unavailable, the action uses a local FFmpeg software decode when present
  at `/opt/homebrew/bin/ffmpeg` or the absolute path in `CAM_APPS_FFMPEG`.
- **Evidence** includes filtered PianoCamera lifecycle counters, the new camera-stack crash report count, timing and
  movie metadata, and SHA-256 hashes of three sample luminance grids. Frame buffers and raw logs stay in memory.
  QuickTime's movie is staged in an owner-only run directory under `/private/var/tmp/cam-apps/`. The action quits each
  app it starts and removes the run directory on completion or interruption. Both checks require a confirmed clean exit
  and zero new crash reports.

The FaceTime luminance check expects a cumulative `nonblack=N` field on periodic `camera output sensor=2` events in
`com.unixnu.PianoCamera`'s `extension` category. That field must count delivered frames with non-black image content.
The current provider source emits frame and source counts without luminance. FaceTime is VOID without that telemetry,
even when frames advance. The installed 27.3 app paths are pending verification.
[Provider output counters](../../tooling/camera/extension/PianoCameraExtension.m)

The harness grades `$R/cam-apps-evidence.json` through `grade.py`. Exit 0 means both apps passed, 1 means a known
failure with complete observation, and 2 means at least one result is VOID due to missing or ambiguous evidence,
unmet prerequisites, or unconfirmed cleanup. Each app prints its own verdict.

## Host verification

```sh
dir=tooling/native/piano-cam-apps-check
bin="$PWD/out/piano-cam-apps-check/cam-apps-probe"
CAM_APPS_TEST_BIN="$bin" PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 -m unittest discover -s "$dir/tests" -v
CAM_APPS_TEST_BIN="$bin" PYTHONDONTWRITEBYTECODE=1 /opt/homebrew/bin/python3 -m unittest discover -s "$dir/tests" -v
bash -n "$dir/build.sh"
bash -n "$dir/cam-apps-check.spec"
shellcheck -s bash "$dir/build.sh" "$dir/cam-apps-check.spec"
```

The host tests use mocked app and log evidence. With the built probe and FFmpeg, the native file test generates bright and
black H.264 QuickTime files and exercises wrong-container and corrupt-file cases. The development host's locked macOS
27.2.1 session returned VOID before either app launched.
