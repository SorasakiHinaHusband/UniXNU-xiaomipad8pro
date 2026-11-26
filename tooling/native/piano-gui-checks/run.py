#!/usr/bin/env python3
"""Mac-side protocol for the installed gui-checks.spec."""
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

from grade import brightness_at, parse_luma, parse_panel, visible_change, wifi_off, wifi_ready, wifi_transition


class CheckError(Exception):
    pass


def env(name):
    return os.environ[name]


def bash(name, *args, body=None, timeout=35, heartbeat=None):
    command = ["/bin/bash", "-c", f'{name} "$@"', "gui-checks", *map(str, args)]
    if heartbeat is None:
        return subprocess.run(command, input=b"" if body is None else body, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout, check=False)
    with subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as child:
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                try:
                    output, _ = child.communicate(timeout=.4)
                    return subprocess.CompletedProcess(command, child.returncode, output)
                except subprocess.TimeoutExpired:
                    heartbeat()
            raise subprocess.TimeoutExpired(command, timeout)
        finally:
            if child.poll() is None:
                child.kill()


def verdict(status, message):
    result = bash("verdict", f"{status} {message}")
    print(result.stdout.decode(), end="", flush=True)


def save(label, data, wifi=False):
    if wifi:
        data = subprocess.run(["python3", env("GUI_REDACT")], input=data, capture_output=True,
                              timeout=5, check=True).stdout
    (Path(env("R")) / f"{label}.txt").write_bytes(data)


def parse_reply(text):
    messages = [json.loads(line[4:]) for line in text.splitlines() if line.startswith("GUI ")]
    if len(messages) != 1 or not isinstance(messages[0], dict):
        raise ValueError("expected a complete GUI receipt")
    return messages[0]


def remote(op, *args, artifact=None, required=True, limit=24, heartbeat=None):
    wifi = op.startswith("wifi-")
    # The legacy tsudo interpolates remote arguments in a shell command.
    # Keep the SSID in stdin/staged data, and pass only fixed tokens/paths/levels.
    assert all(re.fullmatch(r"[A-Za-z0-9/_.-]+", str(arg)) for arg in (op, *args))
    result = bash(f"SSH_T={limit} run_remote", op, *args, timeout=limit + 3, heartbeat=heartbeat)
    if artifact:
        save(artifact, result.stdout, wifi)
    try:
        info = parse_reply(result.stdout.decode(errors="replace"))
    except ValueError:
        info = {"error": "missing GUI receipt"}
    if required and (result.returncode or "error" in info or info.get("status", 0) != 0):
        raise CheckError(f"{op}: {info.get('error', 'action failed')}")
    return info


def known(label, condition):
    verdict("PASS" if condition else "FAIL", label)
    if not condition:
        raise CheckError(label)


def shot(label):
    result = bash("shot", label)
    if result.returncode:
        raise CheckError(f"C920 shot {label}")
    images = list(Path(env("R")).glob(f"shot-{label}-[0-9]*.jpg"))
    if len(images) != 1 or not images[0].stat().st_size:
        raise CheckError(f"C920 shot missing for {label}")
    return images[0]


def ssid_bytes(path):
    try:
        content = Path(path).read_bytes()
        lines = content.splitlines()
        if len(lines) == 1 and not lines[0].startswith(b"ssid="):
            target = lines[0]
        else:
            values = [line[5:] for line in lines if line.startswith(b"ssid=")]
            target, = values
        target.decode("utf-8")
        if not 1 <= len(target) <= 32 or any(byte < 32 or byte == 127 for byte in target):
            raise ValueError
        return target
    except (OSError, ValueError):
        raise CheckError("expected one UTF-8 SSID in the Mac's local fixture file") from None


def stage_target():
    target = ssid_bytes(env("GUI_SSID_FILE"))
    dest = env("GUI_TARGET")
    result = bash("rssh", f"umask 077; /bin/cat > {dest} && test -s {dest}",
                  body=target)
    if result.returncode:
        raise CheckError("could not stage the local SSID")


def wifi_state(label=None):
    return remote("wifi-state", env("GUI_TARGET"), artifact=label, required=False, limit=8)


def wait_wifi(label, action, predicate, seconds=30):
    started = action.get("t")
    if not isinstance(started, (int, float)) or not math.isfinite(started):
        raise CheckError(f"{label}: missing action timestamp")
    deadline = time.monotonic() + seconds
    samples = []
    while time.monotonic() < deadline:
        state = wifi_state()
        ts = state.get("t")
        if type(ts) in (int, float) and math.isfinite(ts):
            samples.append((ts, state))
            if started <= ts <= started + seconds and predicate(state):
                break
            if ts > started + seconds:
                break
        time.sleep(0.5)
    data = "".join(json.dumps(state, sort_keys=True) + "\n" for _, state in samples)
    save(label, data.encode(), wifi=True)
    return samples


def wifi_join():
    target = env("GUI_TARGET")
    menu = remote("wifi-menu", artifact="wifi-menu")
    known("Wi-Fi menu opened", menu.get("menu") is True)
    shot("wifi-menu")
    row = remote("wifi-find", target, artifact="wifi-row", required=False)
    known("Wi-Fi target row found", row.get("menu") is True and row.get("row") is True)
    shot("wifi-row")
    before = wifi_state("wifi-before-click")
    if before.get("target") is True:
        # The installed boot can already be joined. Create a non-target baseline without
        # selecting any network. The AX helper rechecks this baseline before pressing.
        remote("wifi-disassociate", artifact="wifi-baseline-disassociate")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if wifi_state().get("target") is False:
                break
            time.sleep(.3)
    clicked = remote("wifi-click", target, artifact="wifi-click")
    click_samples = wait_wifi("wifi-join-timeline", clicked, wifi_ready)
    shot("wifi-join")
    join_ok = wifi_transition(clicked.get("before", {}), click_samples, clicked["t"])
    verdict("PASS" if join_ok else "FAIL", "Wi-Fi row click associated to target with DHCP within 30 s")
    return join_ok


def wifi_toggle():
    try:
        off = remote("wifi-off", artifact="wifi-off-action")
        off_hits = wait_wifi("wifi-off-timeline", off, wifi_off, 15)
        off_state = off_hits[-1][1] if off_hits else {}
        off_ok = bool(off_hits) and off_hits[-1][0] <= off["t"] + 15 and wifi_off(off_state)
        verdict("PASS" if off_ok else "FAIL", "Wi-Fi menu toggle powered off and dropped the link within 15 s")
        off_frame_at = time.monotonic()
        shot("wifi-off")
        time.sleep(max(0, 5 - (time.monotonic() - off_frame_at)))
        before_on = wifi_state("wifi-before-on")
        on = remote("wifi-on", artifact="wifi-on-action")
        on_samples = wait_wifi("wifi-on-timeline", on, wifi_ready)
        shot("wifi-on")
        on_ok = wifi_off(before_on) and wifi_transition(before_on, on_samples, on["t"])
        verdict("PASS" if on_ok else "FAIL", "Wi-Fi menu toggle powered on and rejoined target with DHCP within 30 s")
        return off_ok and on_ok
    finally:
        state = wifi_state("wifi-final")
        if state.get("power") is False:
            restored = remote("wifi-on", artifact="wifi-restore-power")
            samples = wait_wifi("wifi-restore-timeline", restored, lambda state: state.get("power") is True)
            if not samples or samples[-1][1].get("power") is not True:
                raise CheckError("Wi-Fi power recovery failed")
        elif state.get("power") is not True:
            raise CheckError("could not confirm final Wi-Fi power")


def wifi_check():
    stage_target()
    if wifi_state("wifi-initial").get("power") is not True:
        raise CheckError("Wi-Fi must start powered on")
    try:
        key_escape()
        joined = wifi_join()
    except (CheckError, subprocess.SubprocessError) as error:
        verdict("FAIL", f"Wi-Fi row click: {error}")
        joined = False
    toggled = wifi_toggle()
    return joined and toggled


def read_brightness(label=None):
    result = bash("SSH_T=8 run_remote", "display-sample", timeout=11)
    reply = result.stdout.decode(errors="replace")
    # Keep only the readback keys; other registry properties are irrelevant here.
    selected = "\n".join(line for line in reply.splitlines() if re.match(
        r'^\s*"(piano-backlight-(?:(?:primary-|secondary-)?(?:lit|code))|IODisplayParameters)" = ', line))
    if label:
        receipt = "\n".join(line for line in reply.splitlines() if line.startswith("GUI "))
        save(f"{label}-readback", (receipt + "\n" + selected + "\n").encode())
    try:
        if result.returncode:
            raise ValueError
        readback = parse_reply(reply)
        return readback["slider"], readback["level"], readback["nits"], parse_panel(selected)
    except (KeyError, ValueError):
        raise CheckError("incomplete brightness readback") from None


def sample(target, label):
    deadline = time.monotonic() + 10
    settled = False
    while time.monotonic() < deadline:
        if brightness_at(target, *read_brightness()) and time.monotonic() <= deadline:
            settled = True
            break
        time.sleep(0.4)
    photo = shot(label)
    current = read_brightness(label)
    luminance = subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                                "-i", str(photo), "-frames:v", "1", "-vf",
                                "crop=iw*0.45:ih*0.45:iw*0.25:ih*0.25,signalstats,"
                                "metadata=print:key=lavfi.signalstats.YAVG:file=-", "-f", "null", "-"],
                               check=True, capture_output=True, timeout=8).stdout
    save(f"{label}-luma", luminance)
    return settled and brightness_at(target, *current), (target, parse_luma(luminance.decode()))


def load_input():
    path = Path(env("GUI_REPO")) / "tooling/native/piano-input.py"
    spec = importlib.util.spec_from_file_location("piano_input", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def key_escape():
    source = load_input()
    if source.main(["key", "esc"]):
        raise CheckError("HID Escape failed")


def move_pointer(port, x, y, held=False, tolerance=2.5):
    pulse = (lambda: port.send("P 1 0 0")) if held else None
    for _ in range(18):
        cursor = remote("cursor", limit=5, heartbeat=pulse)
        dx, dy = x - cursor["x"], y - cursor["y"]
        if abs(dx) <= tolerance and abs(dy) <= tolerance:
            return
        # Relative HID motion is accelerated. Limit each step and correct against Quartz.
        def step(delta):
            if abs(delta) <= tolerance:
                return 0
            return int(math.copysign(min(80, max(1, round(abs(delta) * .55))), delta))
        port.send(f"P {int(held)} {step(dx)} {step(dy)}")
    raise CheckError("HID pointer failed to converge on the slider")


def drag(target, label):
    source = load_input()
    try:
        port = source.Port(source.find_port())
        try:
            drag_on_port(port, target, label)
        finally:
            try:
                port.send("R")
            finally:
                port.close()
    except source.InputError as error:
        raise CheckError(f"HID drag: {error}") from None


def drag_on_port(port, target, label):
    geometry = remote("display-read")
    for attempt in range(2):
        current = geometry["slider"]
        left = geometry["x"] + geometry["height"] / 2
        width = geometry["width"] - geometry["height"]
        y = geometry["y"] + geometry["height"] / 2
        move_pointer(port, left + current * width, y)
        port.send("P 1 0 0")
        # Confirm a nonzero held report before the first remote cursor query.
        delta = (target - current) * width
        if abs(delta) >= 1:
            port.send(f"P 1 {round(delta * .45) or (1 if delta > 0 else -1)} 0")
        move_pointer(port, left + target * width, y, held=True)
        port.send("P 0 0 0")
        geometry = remote("display-read", artifact=f"{label}-hid-{attempt + 1}")
        if abs(geometry["slider"] - target) <= .05:
            return
    raise CheckError("HID drag did not land within five percentage points")


def brightness_check(method):
    key_escape()
    opened = remote("display-open", artifact=f"brightness-{method}-menu")
    original = opened["slider"]
    shot(f"brightness-{method}-menu")
    baseline_ok, baseline = sample(original, f"brightness-{method}-original")
    verdict("PASS" if baseline_ok else "FAIL", f"brightness-{method} original level and panel readback")
    action = (lambda level, label: remote("display-set", f"{level:.8f}", artifact=f"{label}-action")
              if method == "ax" else drag(level, label))
    results = []
    restored = False
    try:
        for stage, target in (("30", .30), ("70", .70), ("restore", original)):
            label = f"brightness-{method}-{stage}"
            action(target, label)
            step_ok, luma = sample(target, label)
            verdict("PASS" if step_ok else "FAIL", f"{label} slider, CoreBrightness and panel readback")
            results.append((step_ok, luma))
        restored = results[-1][0]
        observed = visible_change(*(entry[1] for entry in results), baseline)
        verdict("PASS" if observed else "FAIL", f"brightness-{method} visible C920 luminance change and return")
        return baseline_ok and all(ok for ok, _ in results) and observed
    finally:
        if not restored:
            # Attempt a restore through the same path even when a step failed.
            remote("display-open")
            action(original, f"brightness-{method}-cleanup")
            if not brightness_at(original, *read_brightness(f"brightness-{method}-cleanup")):
                verdict("FAIL", f"brightness-{method} original level recovery")
        key_escape()


def prepare_ax():
    # Probe in the same launchctl and sudo chain used by every action.
    ready = remote("ax-trust", artifact="ax-trust", required=False)
    if ready.get("trusted") is not True:
        seeded = bash("run_remote", "seed", timeout=16)
        save("ax-seed", seeded.stdout)
        if b"GUI_TCC_SEEDED\n" not in seeded.stdout:
            raise CheckError("could not seed the installed SSH-wrapper Accessibility grant")
        for _ in range(5):
            if remote("ax-trust", artifact="ax-trust", required=False).get("trusted") is True:
                break
            time.sleep(1)
        else:
            raise CheckError("console-user Accessibility probe did not become trusted")


def main(phase):
    # Let both protocol finally blocks run on TERM as well as Ctrl-C.
    def interrupted(signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    if phase == "ax-trust":
        try:
            prepare_ax()
        except (CheckError, subprocess.SubprocessError) as error:
            verdict("VOID", f"Wi-Fi GUI and both brightness methods: {error}")
            return 2
        return 0
    name, check = {
        "wifi": ("Wi-Fi GUI", wifi_check),
        "brightness-ax": ("Brightness AX value", lambda: brightness_check("ax")),
        "brightness-hid": ("Brightness HID drag", lambda: brightness_check("hid")),
    }[phase]
    try:
        ok = check()
        verdict("PASS" if ok else "FAIL", name)
        return 0 if ok else 1
    except (CheckError, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        verdict("FAIL", f"{name}: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1]))
