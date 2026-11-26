# Installed Wi-Fi and brightness GUI checks

Run from the main-session Mac that owns the tablet's USB and C920 connections. The check uses the current macOS 27.3 build `26C5035r` boot with its installed Data store (`data-persist=2`). It holds the harness tablet lock for the whole run. It uses the existing USB SSH, `desk-login`, C920 `shot` and `piano-input` helpers. It compiles one arm64 user-space probe locally.

```sh
bash tooling/native/piano-gui-checks/run-installed.sh "$HOME/.config/piano/wifi.secret"
```

The optional argument is a Mac-local UTF-8 file containing one SSID line or a single `ssid=` entry in the usual Wi-Fi secret file. Use a visible, previously saved test network with a DHCP server and auto-join enabled. The runner extracts only the SSID and sends it on stdin to a mode-0600 temporary file on the tablet. Keep the file outside the repository. The standard file above is the default.

The runner sources [the spec block](../../tooling/native/piano-gui-checks/gui-checks.spec). The harness's installed observation compiler only admits fixed read-only operations. This block uses the main-session entry point above and runs against the boot already present. Run it with the camera available and the tablet screen in the standard C920 framing. The desktop account must match the harness USB SSH account. The runner logs in or unlocks through `desk-login` when needed.

## Results

Each run creates a private `private/host-checks/YYYYMMDD-runs/gui-checks.*` directory in the harness checkout. Read `verdict.txt` for the individual checks and `run.log` for their order. The command exits 0 when every check passes, 1 for a graded check failure, or 2 when Accessibility setup or verified cleanup fails. The setup diagnostics and photos remain in the private results directory. Wi-Fi photos can show nearby network names. Text evidence records booleans and counts and passes through `redact-wifi.py` before saving.

- **Wi-Fi row click.** The helper locates the menu-bar Wi-Fi extra owned by Control Center, opens the popover, inventories pressable rows and finds an exact SSID match. It expands Other Networks when necessary. The saved receipts confirm that the menu opened and the target row was found. If the boot already joined the target, the runner disassociates once to create a baseline. The helper checks again immediately before pressing the row. Within 30 seconds of the press, the driver must report the exact SSID on `en0` with active link and a DHCP ACK and lease matching the configured IPv4 address.
- **Wi-Fi toggle.** The same popover switch must cause GET19 to report off and the link to drop within 15 seconds. The runner holds off for at least five seconds, then presses the switch back on. Power, target association and a matching DHCP lease must return within 30 seconds. Both windows use tablet monotonic timestamps. The runner attempts to restore power through the menu if the cycle is interrupted.
- **Brightness via AX value.** The helper opens Control Center Display, reads the original slider level, sets `AXValue` to 30%, then 70%, then the original level. Every target must settle within ten seconds and still pass after its photo.
- **Brightness via simulated drag.** The runner reads the slider's AX bounds and thumb value and uses acknowledged pointer reports from `piano-input` to hold, move and release the left button. It uses Quartz cursor readings to correct for relative pointer acceleration. This path repeats the same targets and readbacks.

Both brightness grades require the AX slider and CoreBrightness's normalized `DisplayServicesGetBrightness` level to land within five percentage points. The `PianoBacklight` registry provides `BrightnessMilliNits` and the code and lit flags for both ICs. The chips must agree and be lit, and applied millinits must be within 5% of CoreBrightness's `DisplayNitsKey`. The hardware code is a physical control value, so the comparison uses nits. The C920 shot of the same center crop must rise by at least 3 YAVG units from 30% to 70% and change in the expected direction on return. If the original level lies within five points of 70%, the return shot is compared with the original shot.

## Accessibility

The remote runner checks the console UID, Finder, GUI bootstrap and unlocked state before using `launchctl asuser` and `sudo -u` to run the Objective-C helper in the console session. It probes `AXIsProcessTrusted` through the same path. When that probe needs a grant, the runner uses the SSH wrapper's signed requirement and system Accessibility row from the harness `tcc-seed.sh` convention. The installed variant preserves any existing TCC entry, creates only that row when absent, restarts the console user's `tccd`, then probes again. Cleanup deletes the temporary row by its requirement and timestamp and restarts that user's `tccd` again. The RAM seed itself requires volatile Data and also seeds camera consent.

## Offline verification

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tooling/native/piano-gui-checks/tests -v
shellcheck -s bash tooling/native/piano-gui-checks/{build.sh,run-installed.sh,session.sh,gui-checks.spec}
```

The panel fixture and luminance trace come from `20261120-installed-issues/verify-v131h-002447` in the private host checks. They contain scalar output from the earlier direct brightness checks. The DHCP packet and GUI receipts use offline fixture data. The native DHCP parser test compiles the probe on arm64 macOS and runs against a file. The AX selectors, installed TCC attribution and simulated drag still require a run against the installed tablet.
