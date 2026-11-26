# SPDX-License-Identifier: MIT
"""Exercise both file analyzers without opening an audio device."""

import array
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import wave

RATE = 48000
SECONDS = 3


def write_wav(path, samples, rate=RATE):
    pcm = array.array("h", samples)
    if sys.byteorder == "big":
        pcm.byteswap()
    with wave.open(str(path), "wb") as wav:
        wav.setparams((1, 2, rate, 0, "NONE", "not compressed"))
        wav.writeframes(pcm.tobytes())


def run(command, status, *markers):
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == status and all(marker in result.stdout for marker in markers), (
        f"{command}: exit={result.returncode}, expected {status} and {markers}\n"
        f"{result.stdout}\n{result.stderr}"
    )


def test_replay(host, folder):
    rate = 16000
    rng = random.Random(72)

    def take(name, noise=12, amplitude=0, frequency=1000, length=0.8, seconds=SECONDS):
        path = folder / f"c920-{name}.wav"
        samples = []
        for i in range(seconds * rate):
            at = i / rate
            signal = amplitude * math.sin(math.tau * frequency * at) if 0.65 <= at < 0.65 + length else 0
            samples.append(round(signal) + rng.randint(-noise, noise))
        write_wav(path, samples, rate)
        return path

    quiet = take("quiet")
    tone = take("tone", amplitude=180)
    contaminated = take("contaminated", amplitude=100)
    low_snr = take("low-snr", noise=360, amplitude=180)
    transient = take("transient", amplitude=1800, length=0.2)
    wrong = take("wrong", amplitude=220, frequency=1300)
    tone_near_control = take("tone-near-control", noise=1, amplitude=11)
    weak_control = take("weak-control", noise=1, amplitude=5)
    blank = take("blank", noise=0)
    short = take("short", seconds=2)
    truncated = take("truncated")
    truncated.write_bytes(truncated.read_bytes()[:-256])
    for replay, control, status, marker in (
        (tone, quiet, 0, "replay_tone_detected=yes"),
        (quiet, quiet, 1, "replay_tone_detected=no"),
        (low_snr, quiet, 1, "replay_tone_detected=no"),
        (transient, quiet, 1, "replay_tone_detected=no"),
        (wrong, quiet, 1, "replay_tone_detected=no"),
        (tone, contaminated, 1, "quiet_control_clean=no"),
        (tone_near_control, weak_control, 1, "replay_tone_detected=yes"),
        (tone, blank, 2, "grade=error"),
        (short, quiet, 2, "grade=error"),
        (truncated, quiet, 2, "grade=error"),
    ):
        run([*host, replay, "--mode", "replay", "--control", control, "--seconds", str(SECONDS)],
            status, marker, ("grade=pass", "grade=fail", "grade=error")[status])


def main(binary, stimulus, folder):
    host = [sys.executable, Path(__file__).with_name("analyze.py")]
    with wave.open(stimulus, "rb") as wav:
        assert (wav.getframerate(), wav.getnframes(), wav.getsampwidth(), wav.getnchannels()) == (
            RATE, 2 * RATE, 2, 1
        )
        assert wav.readframes(RATE // 2) == bytes(RATE)

    run([binary, "--analyze", stimulus, "--tone"], 0, "tone_frequency_hz=1000", "duration_s=2.000")
    run([*host, stimulus, "--mode", "tone", "--seconds", "2"], 0, "tone_detected=yes")
    run([*host, stimulus, "--mode", "tone", "--seconds", "3"], 1, "grade=fail")
    run([binary, "2", "unused.m4a", "--tone"], 2, "result=usage")

    rng = random.Random(17)
    quiet, zero, wrong, transient, dc, loud = (
        folder / f"{stem}.wav" for stem in ("quiet", "zero", "wrong", "transient", "dc", "loud")
    )
    write_wav(quiet, (rng.randint(-100, 100) for _ in range(SECONDS * RATE)))
    write_wav(zero, [0] * (SECONDS * RATE))
    write_wav(wrong, (round(1000 * math.sin(math.tau * 1300 * i / RATE)) for i in range(SECONDS * RATE)))
    write_wav(transient, (round(8000 * math.sin(math.tau * 1000 * i / RATE)) if i < RATE // 5 else 0
                          for i in range(SECONDS * RATE)))
    write_wav(dc, [800] * (SECONDS * RATE))
    write_wav(loud, (rng.randint(-25000, 25000) for _ in range(SECONDS * RATE)))
    for data, mode, status, marker in (
        (quiet, "quiet", 0, "noise_floor_sane=yes"),
        (quiet, "tone", 1, "tone_detected=no"),
        (zero, "quiet", 1, "noise_floor_sane=no"),
        (dc, "quiet", 1, "noise_floor_sane=no"),
        (loud, "quiet", 1, "noise_floor_sane=no"),
        (wrong, "tone", 1, "tone_detected=no"),
        (transient, "tone", 1, "tone_detected=no"),
    ):
        run([*host, data, "--mode", mode, "--seconds", str(SECONDS)], status, marker)
    for data in (zero, dc):
        run([binary, "--analyze", data], 4, "result=silence")
    for data in (quiet, wrong, transient):
        run([binary, "--analyze", data, "--tone"], 5, "result=tone_missing")

    m4a = folder / "tone.m4a"
    subprocess.run(["/usr/bin/afconvert", "-f", "m4af", "-d", "LEI16", stimulus, m4a], check=True)
    run([*host, m4a, "--mode", "tone", "--seconds", "2"], 0, "tone_frequency_hz=1000", "grade=pass")
    run([binary, "--analyze", m4a, "--tone"], 0, "tone_frequency_hz=1000", "result=ok")
    corrupt = folder / "corrupt.m4a"
    corrupt.write_bytes(b"invalid audio")
    run([*host, corrupt, "--mode", "tone"], 2, "grade=error")
    run([binary, "--analyze", corrupt, "--tone"], 1, "result=error")
    run([binary, "--replay", stimulus], 2, "result=usage")
    test_replay(host, folder)
    print("mic-check host tests passed (WAV, PCM M4A, capture grades and C920 replay controls)")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="piano-mic-check-test.") as directory:
        main(sys.argv[1], sys.argv[2], Path(directory))
