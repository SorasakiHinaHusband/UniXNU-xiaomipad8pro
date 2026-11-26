#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host regressions for malformed evidence and standalone image manifests."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

NATIVE = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, NATIVE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(name, *args):
    return subprocess.run([sys.executable, str(NATIVE / (name + '.py')), *map(str, args)],
                          capture_output=True, text=True)


class Graders(unittest.TestCase):
    def test_v6_missing_hash_and_wrong_no_output_count(self):
        v6 = load('video-v6-verdict')
        digest = 'a' * 64
        phases = {'ibp-async': 60, 'ibp-temporal': 60, 'ip-temporal': 60,
                  'ibp-no-output': 60, 'recreate-at-idr': 30}
        manifest = {'fixtures': {clip: {'output': [
            {'pts': i * 3000, 'sha256': digest} for i in range(60)]} for clip in ('ibp', 'ip')}}
        lines = []
        for phase, count in phases.items():
            nulls = int(phase == 'ibp-no-output')
            lines.append(f'video-v6: summary phase={phase} submitted={count} callbacks={count} '
                         f'images={count-nulls} nulls={nulls} errors=0 early=1 high-water=2')
            for i in range(count):
                image = int(not nulls or i > 0)
                sha = digest if image else '-'
                lines.append(f'video-v6: frame phase={phase} token={i+1} callback=1 status=0 '
                             f'pts={i}/30 image={image} sha256={sha}')
        self.assertFalse(any(error.split()[0] in phases
                             for error in v6.evaluate('', '\n'.join(lines), manifest)))
        # An exact PTS outside the reference and a missing digest once compared equal.
        bad_hash = '\n'.join(lines).replace(f'pts=0/30 image=1 sha256={digest}', 'pts=60/30 image=1', 1)
        self.assertIn('ibp-async per-PTS software hash', v6.evaluate('', bad_hash, manifest))
        bad_null = '\n'.join(lines).replace(f'pts=1/30 image=1 sha256={digest}\nvideo-v6: frame phase=ibp-no-output',
                                           'pts=1/30 image=0 sha256=-\nvideo-v6: frame phase=ibp-no-output')
        self.assertTrue(any(error.startswith('ibp-no-output ') for error in v6.evaluate('', bad_null, manifest)))

    def test_v6_cli_reports_malformed_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            console, tool, manifest = (Path(tmp) / name for name in ('console', 'tool', 'manifest'))
            console.write_text('V6 native close session invalid\n')
            tool.write_text('video-v6: summary missing-phase\n')
            manifest.write_text(json.dumps({'fixtures': {clip: {'output': []} for clip in ('ip', 'ibp')}}))
            for paths in ((console, tool, manifest), (console, tool, Path(tmp) / 'missing')):
                result = run('video-v6-verdict', *paths)
                self.assertEqual(result.returncode, 1)
                self.assertIn('V6_EVIDENCE_FAIL checks=', result.stdout)
                self.assertNotIn('Traceback', result.stderr)

    def test_v3_missing_and_truncated_capture_fails_with_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for body, status in ((None, 'INCONCLUSIVE'), ('', 'INCONCLUSIVE'),
                                 ('PianoVideo: V3 evidence contract vid-v3b:\n', 'FAIL / INCOMPLETE')):
                if body is not None:
                    (root / 'console.out').write_text(body)
                result = run('video-v3-verdict', root)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(status, (root / 'V3-VERDICT.md').read_text())

    def test_gpu_incomplete_tables_cannot_pass_optimized_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = Path(tmp) / 'capture'
            capture.write_text(''.join(f'R3a ARC {rail} present=1 count=2\n'
                                       f'R3a ARC {rail} index=0 vlvl=0\nR3a ARC {rail} index=1 vlvl=100\n'
                                       for rail in ('gfx', 'mx', 'cx')))
            result = subprocess.run([sys.executable, '-O', str(NATIVE / 'check-gpu-qualification.py'),
                                     '--tables-only', str(capture)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn('FAIL C5 evidence:', result.stdout)
            self.assertNotIn('Traceback', result.stderr)
            result = run('check-gpu-qualification', capture / 'missing')
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('Traceback', result.stderr)

    def test_mic_windows_require_all_phases_and_finite_times(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            measure = root / 'measure.py'
            measure.write_text('def read(pcm): return bytes(40 * 48000), 48000, 1\n'
                               'def metrics(*args): return {"tone_dbfs": -60}\n')
            report = root / 'report'
            report.write_text(json.dumps(dict(failures=[], file_onset=8, phases=[dict(carrier_amplitude=1)])))
            timeline = root / 'timeline'
            plays = [dict(event='play', phase=p, before=100 + i * 8, rc=0)
                     for i, p in enumerate(('unity', 'lower', 'mute', 'recover'))]
            silence = [dict(event='silence-start', at=93), dict(event='silence-end', at=99)]
            for rows, wanted in ((plays, 0), (plays[:1], 2),
                                 ([dict(plays[0], before=float('nan')), *plays[1:]], 2)):
                timeline.write_text('\n'.join(json.dumps(row) for row in silence + rows))
                result = run('check-mic-diag', 'windows', measure, root / 'pcm', timeline, report)
                self.assertEqual(result.returncode, wanted, result.stdout + result.stderr)

    def test_boot_timeline_empty_and_mixed_timestamps(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = Path(tmp) / 'capture'
            for data in (None, '', '  \n\t\n', '100.000\t\n'):
                if data is not None:
                    capture.write_text(data)
                result = run('boot-timeline', capture)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn('Traceback', result.stderr)
            capture.write_text('console header\n100.000\tKernel boot args: fixture\n')
            result = run('boot-timeline', capture, '--xnu-start', '00:00:00')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('T from', result.stdout)
            result = run('boot-timeline', capture, '--xnu-start', '99:10:00')
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('Traceback', result.stderr)

    def test_halt_timeout_ignores_platform_action_after_cut(self):
        halt = load('halt-trace-verdict')
        lines = [
            'piano-halt: PEHaltRestart type 1 at t=100 ms',
            "piano-halt: phase 'Notifying HaltRestart action handlers'",
            'piano-halt: IOPlatformHaltRestartAction -> first',
            'piano-halt: timed out after 30 s at t=30100 ms',
            'piano-halt: IOPlatformHaltRestartAction -> later',
        ]
        self.assertIn('action of first ', halt.verdict(lines)[1])
        with tempfile.TemporaryDirectory() as tmp:
            capture = Path(tmp) / 'capture'
            capture.touch()
            self.assertEqual(run('halt-trace-verdict', capture).returncode, 1)
            result = run('halt-trace-verdict', capture.with_name('missing'))
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('Traceback', result.stderr)
        self.assertEqual(run('halt-trace-verdict', '--self-test').returncode, 0)


class RootManifest(unittest.TestCase):
    def test_v1_writer_matches_wire_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = root / 'out'
            paths = [root / 'first=fixture.img', root / 'second.img']
            for i, path in enumerate(paths):
                path.write_bytes(bytes([i]) * 512)
            result = run('make-root-manifest', out, f'OS={paths[0]}', f'SystemOS={paths[1]}')
            self.assertEqual(result.returncode, 0, result.stderr)
            header = (out / 'manifest.bin').read_bytes()
            self.assertEqual(len(header), 4096)
            self.assertEqual(struct.unpack_from('<8sIIQ', header), (b'SHNGROOT', 1, 2, 3 << 20))
            self.assertEqual(header[4064:], hashlib.sha256(header[:4064]).digest())
            receipt = json.loads((out / 'layout.json').read_text())
            for i, name in enumerate(('OS', 'SystemOS')):
                offset = (i + 1) << 20
                sha = hashlib.sha256(paths[i].read_bytes()).digest()
                entry = struct.unpack_from('<32sQQII32s40s', header, 24 + 128*i)
                self.assertEqual(entry, (name.encode().ljust(32, b'\0'), offset, 512, 512, 1, sha, bytes(40)))
                self.assertEqual(receipt['windows'][i], dict(name=name, image=str(paths[i]), offset=offset,
                                                             size=512, sha256=sha.hex()))

    def test_rejects_layouts_the_v1_reader_cannot_use(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            good, empty, odd = (root / name for name in ('good', 'empty', 'odd'))
            good.write_bytes(bytes(512))
            empty.touch()
            odd.write_bytes(b'\0')
            for specs in ([], [f'={good}'], [f'OS={empty}'], [f'OS={odd}'], [str(good)], [f'OS={root}'],
                          [f'OS={good}', f'OS={good}'], [f'é={good}'], [f'OS\n={good}'],
                          [f'{n}={good}' for n in 'ABCDE']):
                with self.subTest(specs=specs):
                    out = root / 'out'
                    result = run('make-root-manifest', out, *specs)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertNotIn('Traceback', result.stderr)
                    self.assertFalse((out / 'manifest.bin').exists())


if __name__ == '__main__':
    unittest.main()
