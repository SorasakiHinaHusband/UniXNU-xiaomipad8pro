#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host-only regressions for the audio CLI and WAV boundaries."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

NATIVE = Path(__file__).resolve().parents[1]


def chunk(name, payload):
    return struct.pack("<4sI", name, len(payload)) + payload + bytes(len(payload) % 2)


def wav(*chunks):
    return chunk(b"RIFF", b"WAVE" + b"".join(chunks))


def pcm_fmt(rate, channels=1):
    return struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)


class PythonBoundaries(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.folder = Path(scratch.name)

    def run_cli(self, script, *args, status, marker=None):
        result = subprocess.run([sys.executable, NATIVE / script, *args], capture_output=True, text=True)
        self.assertEqual(result.returncode, status, (result.stdout, result.stderr))
        self.assertNotIn("Traceback", result.stderr)
        if marker:
            self.assertIn(marker, result.stdout)

    def test_truncated_and_partial_pcm(self):
        path = self.folder / "input.wav"
        fmt = chunk(b"fmt ", pcm_fmt(16000, channels=2))
        pcm = chunk(b"data", bytes(16000 * 4))
        valid = wav(fmt, pcm)
        for contents in (b"", valid[:-2], wav(fmt, chunk(b"data", bytes(16000 * 4 - 2)))):
            with self.subTest(size=len(contents)):
                path.write_bytes(contents)
                self.run_cli("audio-listen.py", "detect", path, status=2)
                self.run_cli("piano-vpio-probe/piano-vpio-measure.py", "block",
                             self.folder, self.folder, "input", status=2)
        path.write_bytes(wav(chunk(b"fmt ", pcm_fmt(16000)), pcm)[:-2])
        self.run_cli("piano-mic-check/analyze.py", path, "--mode", "tone", status=2, marker="grade=error")

    def test_last_complete_detection_window(self):
        path = self.folder / "input.wav"
        path.write_bytes(wav(chunk(b"fmt ", pcm_fmt(16000)), chunk(b"data", bytes(1600))))
        self.run_cli("audio-listen.py", "detect", path, status=0, marker="(1 windows of 50 ms)")

    def test_nonfinite_and_bad_cli_values(self):
        missing = self.folder / "missing.wav"
        self.run_cli("audio-listen.py", "detect", missing, status=2)
        self.run_cli("audio-listen.py", "record", "--seconds", "nan", "--out", missing, status=2)
        self.assertFalse(missing.exists())
        self.run_cli("piano-mic-route.py", "fixture", missing, "--carrier", "9" * 350, status=2)
        self.assertFalse(missing.exists())
        self.run_cli("piano-vpio-probe/piano-vpio-measure.py", "stimuli", self.folder / "assets",
                     "--rate", "0", status=2)
        self.assertFalse((self.folder / "assets").exists())


if __name__ == "__main__":
    unittest.main()
