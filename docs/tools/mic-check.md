# Piano microphone app check

`piano-mic-check` records the console user's default input with AVFoundation's `AVAudioRecorder`. It saves 48 kHz mono 16-bit PCM in an `.m4a` container. The optional stimulus plays a bundled 1 kHz WAV through the default output during capture. The optional playback check opens the saved file with `AVAudioPlayer` and waits for its completion callback.

The VPIO probe's `avrec` mode uses the same recorder and its `--tcc` reports the same AVFoundation authorization value. `piano-hal-rec` captures through the HAL's AudioQueue, `piano-mic-diag` reads the PianoAudio input before the HAL, and `audio-listen.py` captures the tablet's speaker output with the Mac's C920 microphone. [VPIO probe](../../tooling/native/piano-vpio-probe/piano-vpio-probe.m) · [Piano mic diagnostics](../../tooling/native/piano-mic-diag.md)

## Ubuntu mainline comparison

References are from the local `XiaomiPad8Pro-Ubuntu` clones at `xiaomipad-8pro-mainline` commit `b43fc2b` and `linux-sm8750-piano` commit `b5100d5f2e0b`.

| Path | Ubuntu mainline reference | UniXNU check |
| --- | --- | --- |
| Capture route | `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-desktop.dts:58–71` declares the VA backend. `device/audio/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf:54–93` maps VA DEC0/1 to DMIC0/2 on two-channel ALSA capture PCM 2. | `tooling/native/piano-mic-check/piano-mic-check.m:328–339,384–392` records the selected CoreAudio input through AVFoundation. |
| Capture evidence | `tools/mic-capture-check.py:123–195,230–268` grades the saved ALSA WAV offline, with an optional known-frequency source. | `tooling/native/piano-mic-check/analyze.py:53–115` grades the saved app capture offline, using a quiet take and a known 1 kHz stimulus. |
| Speaker evidence | `tools/speaker-playback-device.sh:71–124` plays one bounded tone through ALSA. `tools/speaker-playback-check.py:139–221` grades the stream, amplifier state and a current proxy. | `tooling/native/piano-mic-check/piano-mic-check.m:271–326` checks the output route and AVAudioPlayer completion. `tooling/native/audio-listen.py:16–42` captures the speaker on the Mac. `tooling/native/piano-mic-check/analyze.py:118–162` grades the acoustic result. |

## Build on the Mac host

```sh
make -C tooling/native/piano-mic-check SDK=macosx
make -C tooling/native/piano-mic-check SDK=macosx test
```

The arm64 build uses the selected Xcode macOS SDK and targets macOS 14 or later. The Makefile generates the WAV, packages it with the signed `out/piano-mic-check/PianoMicCheck.app`, and verifies the signature. Keep that bundle together when staging. Host tests cover the `afconvert` round trip, the capture grades, and replay grading with synthetic tones, noise, and damaged WAVs.

## Run on the installed system

Use an installed test boot whose command line contains exactly one `piano-mic-unattended=1`. The opt-in defaults off. Verify the running line with `/usr/sbin/sysctl -n kern.piano_bootargs` on the tablet. The block falls back to `kern.bootargs` on earlier kernels.

Use the harness's desktop gate to reach the unlocked desktop with the same user as SSH. The tablet needs **Mic** as the default input and an unmuted **Speaker** at a comfortable volume. For a run with the **Mic (noise reduced)** input selected, pass `noise-reduced` in place of `raw`. On the Mac, have FFmpeg and the **HD Pro Webcam C920** microphone available, with macOS microphone access already granted to the host capture process. The installed harness can use its C920 GUI recorder daemon for that access. Keep the room quiet and stop other audio on both devices.

From the checkout with the signed bundle, use these lines after the installed-session desktop gate. The block uses `R`, `rssh` with stdin forwarding, `rsudo`, `pull`, `verdict`, and `fexpect`.

```bash
repo=$PWD
source "$repo/tooling/native/piano-mic-check/mic-check.spec"
rssh '/usr/sbin/sysctl -n kern.bootsessionuuid; /usr/sbin/sysctl -n kern.piano_bootargs'
mic_check_block "$repo" raw
```

The preflight checks the staged app with `--tcc` in the console user's LaunchServices session. On an opt-in boot, an undetermined or denied status triggers an app-only seed. The seed follows the earlier camera and VPIO microphone runs that used the console user's ProtectedSystem TCC database and restarted the user's `tccd`. It writes one microphone row for `com.unixnu.piano-mic-check` bound to the staged arm64 CDHash. The grant persists on installed Data for that code signature. The database receipt alone is insufficient. A fresh app launch must report `mic_status=3` and `result=ok` before either recording starts.

The block stages the signed bundle in a private, fresh directory using `rssh` and verifies the archive hash and app signature on the tablet. It uses `rsudo` to get the current UID from `/dev/console`, enters its bootstrap session with `/bin/launchctl asuser`, and drops to that UID with `sudo -n -u "#$u"`, as in the frametime check. It opens the bundle through LaunchServices so TCC can identify the GUI app, and reads the app's result line from its redirected standard output. [Frametime console launch](frametime.md#build-and-run)

The block records four seconds of room noise, then four seconds with the stimulus. The bundled WAV has a half-second of silence, a one-second burst at −36 dBFS peak with 20 ms ramps, and another half-second of silence. A qualification capture with the previous −18 dBFS stimulus clipped at both PCM rails. Its unclipped zero crossings suggest a peak near +8 dBFS before saturation. At the same speaker volume and mic gain, the 18 dB reduction projects a peak near −10 dBFS, with tone RMS about 31 dB above the measured −44 dBFS quiet floor. The new level's capture headroom and C920 audibility still need a live run.

After both saved recordings pass the tablet and host checks, the block opens `quiet.m4a` and `tone.m4a` in separate AVAudioPlayer launches. Each launch waits for a first-sample receipt from a fresh 18-second C920 take. Each replay must finish on the tablet's **Speaker** while the C920 is still recording.

Without the boot arg, the attended flow uses an existing grant or `MIC_CHECK_REQUEST_ACCESS=1` to request consent for an undetermined app. Click **Allow** within 60 seconds when prompted. The app replays the tone take after capture and the operator records the audible result.

## Read the evidence

The app reports the microphone authorization state and numeric `mic_status` using AVFoundation's values 0 (not determined), 1 (restricted), 2 (denied), and 3 (authorized). The capture log has the input name and HAL stream format, the saved recording format, per-second RMS and peak dBFS, and the default output name during a speaker run. The analyzer reports the frequency and SNR of the detected 1 kHz signal. Exact digital zero displays as −200 dBFS.

The printed `$R/mic-check.*` directory holds `boot.txt`, the `tcc*.txt` receipts, the pulled M4As, and the input grades. `remote-stage.txt` points to the private copies on the tablet. For an unattended PASS, look for `mic_status=3` in `tcc.txt` or `tcc-after-*.txt`, `playback_complete=yes` in both `replay-*.txt` logs, and `grade=pass` in `replay.grade.txt`. The paired `c920-quiet.wav` and `c920-tone.wav` preserve the acoustic evidence. Check the adjacent `*.record.txt` logs when the recorder fails to start or complete.

| Native exit | Result | Meaning |
| --- | --- | --- |
| 0 | `ok` | Requested operation completed |
| 1 | `error` | Audio, file, route change, duration, or playback failure |
| 2 | `usage` | Invalid command or arguments |
| 3 | `no_permission` | AVFoundation microphone access is unavailable |
| 4 | `silence` | No 200 ms window reaches −85 dBFS AC RMS |
| 5 | `tone_missing` | Capture has signal but the stimulus check failed |

The native binary accepts `SECONDS OUTPUT.m4a [--tone] [--playback]` with an unused output path, 1–300 integer seconds, and at least 3 seconds for the tone. `--tcc` reads authorization and `--request-access` prompts when it is undetermined. `--analyze FILE [--tone]` checks a saved mono file. On opt-in boots, `--replay FILE.m4a` plays a saved file through **Speaker** without opening a microphone.

To regrade evidence on the Mac, use the standard-library analyzer. The capture modes normalize M4A or WAV to mono 16-bit PCM with `/usr/bin/afconvert` in a temporary directory. Replay mode reads the C920's 16 kHz mono PCM WAV directly.

```sh
python3 tooling/native/piano-mic-check/analyze.py /path/to/tone.m4a --mode tone --seconds 4
python3 tooling/native/piano-mic-check/analyze.py /path/to/quiet.m4a --mode quiet --seconds 4
python3 tooling/native/piano-mic-check/analyze.py /path/to/c920-tone.wav \
  --mode replay --control /path/to/c920-quiet.wav --seconds 18
```

- **Tone** requires two adjacent 200 ms windows with a 980–1020 Hz component, scanned in 5 Hz steps. Each window must reach −55 dBFS tone RMS and 12 dB SNR. SNR compares the estimated tone power to the remaining AC power in that window.
- **Quiet** requires each full second's AC RMS to fall between −85 and −25 dBFS, the capture's peak to stay at or below −6 dBFS, and no test tone to be detected. The reported noise floor is the median of those full-second AC RMS levels. These are initial qualification bands for a quiet room.
- **Replay** requires two adjacent 200 ms windows at 980–1020 Hz, each at −75 dBFS tone RMS or higher and at least 12 dB SNR. The weaker window must also be at least 10 dB above the quiet replay's strongest sustained 1 kHz level. A qualifying tone in the quiet replay fails the control. Both C920 takes must contain nonconstant PCM and match the requested duration within 0.25 s.
- The capture modes also require the duration to be within 0.25 s when `--seconds` is supplied. The host analyzer exits 0 for a pass, 1 for audio outside the grade, and 2 for invalid or unreadable input.
