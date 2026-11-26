import json
import platform
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

FIXTURES = Path(__file__).resolve().parent
sys.path.insert(0, str(FIXTURES.parent))
from grade import brightness_at, parse_luma, parse_panel, visible_change, wifi_off, wifi_transition
import run


class RecordedEvidence(unittest.TestCase):
    def setUp(self):
        self.registry = (FIXTURES / "recorded-backlight.txt").read_text()
        self.luma = (FIXTURES / "recorded-luma.txt").read_text()

    def test_installed_panel_snapshot(self):
        self.assertEqual(parse_panel(self.registry), (178, 2293, True))
        self.assertFalse(parse_panel(self.registry.replace('secondary-code" = 178', 'secondary-code" = 177'))[2])
        self.assertFalse(parse_panel(self.registry.replace('primary-lit" = Yes', 'primary-lit" = No'))[2])
        with self.assertRaises(ValueError):
            parse_panel(self.registry.split("IODisplayParameters")[0])
        with self.assertRaises(ValueError):
            parse_panel(self.registry * 2)

    def test_recorded_c920_change(self):
        before, low, high = [parse_luma(line) for line in self.luma.splitlines()
                             if line.startswith("lavfi.")]
        self.assertEqual((before, low, high), (10.4933, 24.8314, 122.34))
        self.assertTrue(visible_change((.30, low), (.70, high), (.09, before), (.09, before)))
        self.assertFalse(visible_change((.30, low), (.70, low + 1), (.09, before), (.09, before)))
        self.assertFalse(visible_change((.30, low), (.70, high), (.09, high), (.09, before)))
        self.assertTrue(visible_change((.30, low), (.70, high), (.70, high), (.70, high)))
        self.assertFalse(visible_change((.30, low), (.70, high), (.84, low), (.84, high)))
        with self.assertRaises(ValueError):
            parse_luma(self.luma)

    def test_core_and_panel_agree_at_user_target(self):
        panel = parse_panel(self.registry)
        self.assertTrue(brightness_at(.30, .25, .35, 2.293, panel))
        for slider, core, nits, reading in [
            (.249, .30, 2.293, panel),
            (.30, .351, 2.293, panel),
            (.30, .30, 3.293, panel),
            (.30, .30, 2.293, (178, 2293, False)),
        ]:
            self.assertFalse(brightness_at(.30, slider, core, nits, reading))


class WorkflowReceipts(unittest.TestCase):
    def test_complete_and_duplicate_receipts(self):
        receipt = 'GUI {"menu":true,"row":true,"rowCount":3}'
        self.assertTrue(run.parse_reply(f"ssh chatter\n{receipt}\n")["row"])
        for text in ("GUI {", f"{receipt}\n{receipt}", 'GUI []', ""):
            with self.assertRaises(ValueError):
                run.parse_reply(text)

    def test_wifi_uses_preclick_baseline_and_deadline(self):
        before = {"power": True, "associated": False, "target": False, "active": False, "lease": False, "t": 99}
        ready = {"power": True, "associated": True, "target": True, "active": True, "lease": True}
        self.assertTrue(wifi_transition(before, [(130, ready)], 100))
        for sample in [(130.001, ready), (99.9, ready), (102, dict(ready, target=False)),
                       (103, dict(ready, lease=False)), (103, dict(ready, associated=False))]:
            self.assertFalse(wifi_transition(before, [sample], 100))
        self.assertFalse(wifi_transition(dict(before, target=True), [(103, ready)], 100))
        self.assertFalse(wifi_transition(dict(before, t=102), [(103, ready)], 100))
        self.assertFalse(wifi_transition({}, [(103, ready)], 100))

    def test_off_requires_power_and_link_drop(self):
        off = {"power": False, "associated": False, "target": False, "active": False, "lease": False}
        self.assertTrue(wifi_off(off))
        self.assertFalse(wifi_off(dict(off, active=True)))
        self.assertFalse(wifi_off(dict(off, power=True)))

    def test_ssid_file_keeps_exact_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "fixture"
            path.write_bytes("ssid= Café's Wi-Fi \npass=offline-placeholder\n".encode())
            self.assertEqual(run.ssid_bytes(path), " Café's Wi-Fi ".encode())
            path.write_bytes(b"  test-fixture  \n")
            self.assertEqual(run.ssid_bytes(path), b"  test-fixture  ")
            for contents in (b"ssid=one\nssid=two\n", b"ssid=\n", b"x" * 33, b"ssid=\xff\n"):
                path.write_bytes(contents)
                with self.assertRaises(run.CheckError):
                    run.ssid_bytes(path)

    def test_hid_drag_keeps_button_held_during_move(self):
        geometry = {"x": 100, "y": 30, "width": 220, "height": 20, "slider": .30}
        history = []
        port = Mock()
        port.send.side_effect = lambda line: history.append(line)

        def move(_port, x, y, held=False):
            history.append((x, y, held))

        with (patch.object(run, "remote", side_effect=[geometry, dict(geometry, slider=.70)]) as probe,
              patch.object(run, "move_pointer", side_effect=move)):
            run.drag_on_port(port, .70, "fixture")
        self.assertEqual(history, [(170, 40, False), "P 1 0 0", "P 1 36 0",
                                   (250, 40, True), "P 0 0 0"])
        self.assertTrue(all(item.args == ("display-read",) for item in probe.call_args_list))


@unittest.skipUnless(platform.system() == "Darwin" and platform.machine() == "arm64", "arm64 macOS SDK required")
class NativeParser(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        cls.binary = Path(scratch.name) / "gui"
        subprocess.run(["/bin/bash", FIXTURES.parent / "build.sh", cls.binary], check=True)

    def test_ipconfig_lease_fixture(self):
        packet = (FIXTURES / "dhcp-ack.txt").read_text()
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "packet"
            for address, output, expected in [
                ("192.0.2.10", packet, True),
                ("192.0.2.10", packet.replace("ACK 0x5", "0x5"), True),
                ("192.0.2.11", packet, False),
                ("192.0.2.10", packet.replace("ACK 0x5", "NAK 0x6"), False),
                ("192.0.2.10", packet.replace("0x15180", "0x0"), False),
                ("192.0.2.10", packet.replace("0x15180", ""), False),
                ("169.254.0.10", packet.replace("192.0.2.10", "169.254.0.10"), False),
            ]:
                with self.subTest(address=address, output=output):
                    path.write_text(output)
                    result = subprocess.run([self.binary, "fixture-lease", address, path],
                                            text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0 if expected else 2)
                    self.assertIs(json.loads(result.stdout.removeprefix("GUI "))["lease"], expected)


if __name__ == "__main__":
    unittest.main()
