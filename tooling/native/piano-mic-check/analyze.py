#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Grade a pulled piano-mic-check M4A or WAV on a macOS host."""

import argparse
import array
import math
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import wave

INPUT_MIN_DBFS = -85.0
TONE_MIN_DBFS = -55.0
TONE_MIN_SNR_DB = 12.0
QUIET_MAX_DBFS = -25.0
QUIET_PEAK_MAX_DBFS = -6.0
PCM_SCALE = 32768
PCM_POWER_SCALE = PCM_SCALE ** 2


def power_db(power):
    return 10 * math.log10(max(power, 1e-20))


def ac_power(samples):
    mean = sum(samples) / len(samples)
    power = sum(value * value for value in samples) / len(samples)
    return mean, (power - mean * mean) / PCM_POWER_SCALE


def tone_in_window(samples, rate, mean, ac):
    # Match the probe's 200 ms Goertzel scan around its 1 kHz stimulus.
    best_power, best_hz = 0.0, None
    for hz in range(980, 1021, 5):
        coefficient = 2 * math.cos(math.tau * hz / rate)
        previous = older = 0.0
        for sample in samples:
            older, previous = previous, sample - mean + coefficient * previous - older
        power = 2 * (previous * previous + older * older - coefficient * previous * older)
        power /= len(samples) ** 2 * PCM_POWER_SCALE
        if power > best_power:
            best_power, best_hz = power, hz
    level = power_db(best_power)
    return best_hz, level, level - power_db(ac - best_power)


def analyze_wav(path, args):
    with wave.open(str(path), "rb") as wav:
        rate, frames = wav.getframerate(), wav.getnframes()
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or not 4000 <= rate <= 192000:
            raise ValueError("expected mono 16-bit PCM at 4–192 kHz")
        if not frames:
            raise ValueError("recording contains no frames")
        duration = frames / rate
        print(f"sample_rate_hz={rate}\nframes={frames}\nduration_s={duration:.3f}")
        duration_ok = args.seconds is None or abs(duration - args.seconds) <= 0.25

        quiet_levels = []
        peak_dbfs = -200.0
        best_tone = previous_tone = None
        window = rate // 5
        for second, offset in enumerate(range(0, frames, rate)):
            count = min(rate, frames - offset)
            samples = array.array("h")
            samples.frombytes(wav.readframes(count))
            if len(samples) != count:
                raise ValueError("truncated PCM data")
            if sys.byteorder == "big":
                samples.byteswap()
            rms_dbfs = power_db(sum(value * value for value in samples) / (count * PCM_POWER_SCALE))
            peak = power_db((max(abs(value) for value in samples) / PCM_SCALE) ** 2)
            peak_dbfs = max(peak_dbfs, peak)
            print(f"second={second} rms_dbfs={rms_dbfs:.2f} peak_dbfs={peak:.2f}")

            if count == rate:
                quiet_levels.append(power_db(ac_power(samples)[1]))
            for start in range(0, count - window + 1, window):
                segment = samples[start:start + window]
                mean, ac = ac_power(segment)
                frequency, level, snr = tone_in_window(segment, rate, mean, ac)
                if level < TONE_MIN_DBFS or snr < TONE_MIN_SNR_DB:
                    previous_tone = None
                    continue
                current_tone = (snr, level, frequency)
                if previous_tone is not None:
                    stronger = max(previous_tone, current_tone)
                    if best_tone is None or stronger > best_tone:
                        best_tone = stronger
                previous_tone = current_tone

    print(f"tone_detected={'yes' if best_tone is not None else 'no'}")
    if best_tone is not None:
        snr, level, frequency = best_tone
        print(f"tone_frequency_hz={frequency} tone_snr_db={snr:.2f} tone_dbfs={level:.2f}")

    if args.mode == "quiet":
        if quiet_levels:
            print(f"noise_floor_dbfs={statistics.median(quiet_levels):.2f}")
        sane = bool(quiet_levels) and all(
            INPUT_MIN_DBFS <= level <= QUIET_MAX_DBFS for level in quiet_levels
        ) and peak_dbfs <= QUIET_PEAK_MAX_DBFS and best_tone is None
        print(f"noise_floor_sane={'yes' if sane else 'no'}")
        passed = duration_ok and sane
    else:
        passed = duration_ok and best_tone is not None
    if not duration_ok:
        print("recording duration differs from request by more than 0.25 s", file=sys.stderr)
    print(f"grade={'pass' if passed else 'fail'}")
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--mode", choices=("tone", "quiet"), required=True)
    parser.add_argument("--seconds", type=int, help="expected capture duration")
    args = parser.parse_args()
    if args.seconds is not None and args.seconds < 1:
        parser.error("--seconds must be positive")
    if args.recording.suffix.lower() not in (".m4a", ".wav"):
        parser.error("expected an .m4a or .wav")
    try:
        if not args.recording.is_file() or args.recording.stat().st_size == 0:
            raise ValueError("missing or empty recording")
        with tempfile.TemporaryDirectory(prefix="piano-mic-check.") as folder:
            pcm = Path(folder) / "normalized.wav"
            subprocess.run(
                ["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16", "-c", "1",
                 str(args.recording.resolve()), str(pcm)],
                check=True,
            )
            return analyze_wav(pcm, args)
    except (EOFError, OSError, ValueError, subprocess.CalledProcessError, wave.Error) as error:
        print(f"analyze: {error}", file=sys.stderr)
        print("grade=error")
        return 2


if __name__ == "__main__":
    sys.exit(main())
