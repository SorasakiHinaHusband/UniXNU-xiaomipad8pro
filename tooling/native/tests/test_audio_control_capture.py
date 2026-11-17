#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("capture", Path(__file__).parents[1] / "check-audio-control-capture.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)

PROBE = """pid_before pid=123 rc=0
default_output phase=test status=0 device=50
control_list_read phase=test status=0 count=1
control phase=test id=52 class=0x766c6d65(vlme) base=0x6c65766c(levl) scope_status=0 scope=0x6f757470(outp) element_status=0 element=0 class_status=0 base_status=0 scalar_status=0 scalar=1 db_status=0 db=-4.529953e-06
device_volume phase=test element=0 status=0 size=4 scalar=1
virtual_main phase=test status=0 size=4 scalar=1
probe_rc=0
pid_after pid=123 rc=0
"""


class CaptureTests(unittest.TestCase):
    def test_integer_getter(self):
        for value in (0, 1, 25, 60, 100):
            self.assertEqual(capture.getter(f"{value}\ngetter_rc=0\n", value), value)

    def test_timeout_never_becomes_line_number(self):
        with self.assertRaisesRegex(capture.InvalidCapture, "TIMEOUT"):
            capture.getter("/tmp/tmo.sh: line 3: 99 Alarm clock: 14\n")
        with self.assertRaisesRegex(capture.InvalidCapture, "TIMEOUT"):
            capture.getter("60\ngetter_rc=142\n")

    def test_getter_rejects_invalid_or_unsuccessful_data(self):
        for text in ("60\n", "60\ngetter_rc=1\n", "101\ngetter_rc=0\n", "-1\ngetter_rc=0\n",
                     "line 3\ngetter_rc=0\n", "60\n50\ngetter_rc=0\n", "warning\n60\ngetter_rc=0\n"):
            with self.subTest(text=text), self.assertRaises(capture.InvalidCapture):
                capture.getter(text)

    def test_exponent_db_and_stable_pid(self):
        self.assertEqual(capture.probe(PROBE, 100), "device=50 controls=52")
        self.assertEqual(capture.identity(PROBE), "device=50 controls=52 pid=123")

    def test_probe_failures(self):
        for text in (PROBE.replace("db=-4.529953e-06", "db=nan"),
                     PROBE.replace("db=-4.529953e-06", "db=-inf"),
                     PROBE.replace("db=-4.529953e-06", "db=1"),
                     PROBE.replace("scalar_status=0", "scalar_status=-1"),
                     PROBE.replace("probe_rc=0", "probe_rc=142"),
                     PROBE.replace("pid_after pid=123", "pid_after pid=124"),
                     PROBE.replace("virtual_main phase=test status=0 size=4 scalar=1", "virtual_main phase=test status=0 size=4 scalar=0")):
            with self.subTest(text=text), self.assertRaises(capture.InvalidCapture):
                capture.identity(text)


if __name__ == "__main__":
    unittest.main()
