# Piano microphone app check

`piano-mic-check` records the console user's default input with AVFoundation's `AVAudioRecorder`. It saves 48 kHz mono 16-bit PCM in an `.m4a` container. The optional stimulus plays a bundled 1 kHz WAV through the default output during capture. The optional playback check opens the saved file with `AVAudioPlayer` and waits for its completion callback.

The VPIO probe's `avrec` mode uses the same recorder and its `--tcc` reports the same AVFoundation authorization value. The existing VPIO TCC seeder is limited to RAM boots. For an installed session, grant the signed app microphone access through the logged-in user's privacy prompt or System Settings. Earlier checks cover other parts of the audio path. `piano-hal-rec` captures through the HAL's AudioQueue, `piano-mic-diag` reads the PianoAudio input before the HAL, and `audio-listen.py` listens for the tablet's speakers using the host microphone. [VPIO probe](../../tooling/native/piano-vpio-probe/piano-vpio-probe.m) · [Piano mic diagnostics](../../tooling/native/piano-mic-diag.md)

## Ubuntu mainline comparison

References are from the local `XiaomiPad8Pro-Ubuntu` clones at `xiaomipad-8pro-mainline` commit `b43fc2b` and `linux-sm8750-piano` commit `b5100d5f2e0b`.

| Path | Ubuntu mainline reference | UniXNU check |
| --- | --- | --- |
| Capture route | `device/audio/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf:54–93` maps VA DEC0/1 to DMIC0/2 on the two-channel ALSA capture PCM 2. The capture-only profile uses the same route in `device/audio/ucm2/Qualcomm/sm8750/Piano-Capture-Diag/HiFi.conf:13–61`. | `tooling/native/piano-mic-check/piano-mic-check.m:288–299,344–352` records the selected CoreAudio input through AVFoundation. The qualification stays at the app and harness layers. |
| Capture evidence | `tools/mic-capture-check.py:123–195,230–268` grades the saved ALSA WAV offline, with an optional known-frequency source. | `tooling/native/piano-mic-check/analyze.py:50–112` grades the saved app capture offline, using a quiet take and a known 1 kHz stimulus. |
| Speaker evidence | `tools/speaker-playback-device.sh:71–124` plays one bounded tone through ALSA. `tools/speaker-playback-check.py:139–221` grades the stream, amplifier state and a current proxy. | `tooling/native/piano-mic-check/piano-mic-check.m:270–285` reports AVAudioPlayer completion. `tooling/native/audio-listen.py:16–19,36–76` provides independent acoustic capture and tone detection on the Mac. |

## Build on the Mac host

```sh
make -C tooling/native/piano-mic-check SDK=macosx
make -C tooling/native/piano-mic-check SDK=macosx test
```

The arm64 build uses the selected Xcode macOS SDK and targets macOS 14 or later. The Makefile generates the WAV, packages it with the signed `out/piano-mic-check/PianoMicCheck.app`, and verifies the signature. Keep that bundle together when staging. Host tests use synthetic recordings and exercise both analyzers, the `afconvert` round trip, and the silence and missing-tone exits.

## Run on the installed system

Log in to the tablet desktop with the harness's SSH account, keep the screen unlocked, and leave the room quiet. In **System Settings → Sound**, select **Mic** for input and **Speaker** for output. Ensure the speaker is unmuted at a comfortable volume and stop other audio. For a separate run with **Mic (noise reduced)**, select that input and pass `noise-reduced` in place of `raw`.

From the Mac checkout, add these lines to the installed-system spec after its desktop gate, where `R`, `rssh`, `rsudo`, `pull`, `verdict`, and `fexpect` are available. Run with the existing installed-session transport. The fragment also works from a Bash shell on the Mac with those same helpers loaded.

```bash
repo=$PWD
source "$repo/tooling/native/piano-mic-check/mic-check.spec"
MIC_CHECK_REQUEST_ACCESS=1 mic_check_block "$repo" raw
```

On the first attended run, click **Allow** on the tablet's microphone prompt within 60 seconds. Omit `MIC_CHECK_REQUEST_ACCESS=1` for an unattended run with an existing grant. A denied app can be enabled in **System Settings → Privacy & Security → Microphone** before retrying. The preflight checks for `mic_status=3` using the same staged app and launch route as the recordings.

The block stages the signed bundle in a private, fresh directory using `rssh` and verifies the archive hash and app signature on the tablet. It uses `rsudo` to get the current UID from `/dev/console`, enters its bootstrap session with `/bin/launchctl asuser`, and drops to that UID with `sudo -n -u "#$u"`, as in the frametime check. It opens the bundle through LaunchServices so TCC can identify the GUI app, and reads the app's result line from its redirected standard output. [Frametime console launch](frametime.md#build-and-run)

The block records four seconds of room noise, then four seconds with the stimulus. The WAV has a half-second of silence, a one-second burst at −18 dBFS peak with 20 ms ramps, and another half-second of silence. When the second recording passes local tone detection, the app replays that M4A through the tablet speaker. The block pulls both files with the harness's verified `pull` helper, runs the host analyzer, and saves launch, authorization, capture and grade logs under the printed `$R/mic-check.*` directory. `remote-stage.txt` records where the private copies remain on the tablet.

Listen for the replay on the tablet and record whether it was clearly audible in the goal 7 evidence. The player callback verifies completion. To listen to the same saved take on the Mac, run `afplay /path/to/mic-check-results/tone.m4a` with the M4A from the printed evidence directory.

## Read the evidence

The app reports the microphone authorization state and numeric `mic_status` using AVFoundation's values 0 (not determined), 1 (restricted), 2 (denied), and 3 (authorized). The capture log has the input name and HAL stream format, the saved recording format, per-second RMS and peak dBFS, and the default output name during a speaker run. The analyzer reports the frequency and SNR of the detected 1 kHz signal. Exact digital zero displays as −200 dBFS.

| Native exit | Result | Meaning |
| --- | --- | --- |
| 0 | `ok` | Requested operation completed |
| 1 | `error` | Audio, file, route change, duration, or playback failure |
| 2 | `usage` | Invalid command or arguments |
| 3 | `no_permission` | AVFoundation microphone access is unavailable |
| 4 | `silence` | No 200 ms window reaches −85 dBFS AC RMS |
| 5 | `tone_missing` | Capture has signal but the stimulus check failed |

The native binary accepts `SECONDS OUTPUT.m4a [--tone] [--playback]` with an unused output path, 1–300 integer seconds, and at least 3 seconds for the tone. `--tcc` only reads authorization and `--request-access` prompts when it is undetermined. `--analyze FILE [--tone]` checks a saved mono file without opening a microphone.

To regrade evidence on the Mac, use the standard-library analyzer. It normalizes M4A or WAV to mono 16-bit PCM with `/usr/bin/afconvert` in a temporary directory and reports the decoded duration and levels.

```sh
python3 tooling/native/piano-mic-check/analyze.py /path/to/tone.m4a --mode tone --seconds 4
python3 tooling/native/piano-mic-check/analyze.py /path/to/quiet.m4a --mode quiet --seconds 4
```

- **Tone** requires two adjacent 200 ms windows with a 980–1020 Hz component, scanned in 5 Hz steps. Each window must reach −55 dBFS tone RMS and 12 dB SNR. SNR compares the estimated tone power to the remaining AC power in that window.
- **Quiet** requires each full second's AC RMS to fall between −85 and −25 dBFS, the capture's peak to stay at or below −6 dBFS, and no test tone to be detected. The reported noise floor is the median of those full-second AC RMS levels. These are initial qualification bands for a quiet room.
- **Both** require readable audio. When `--seconds` is supplied, the duration must be within 0.25 s of the request. The host analyzer exits 0 for a pass, 1 for audio outside the grade, and 2 for an invalid or unreadable input.
