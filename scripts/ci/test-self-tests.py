#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host tests for the self-test runner's discovery, scheduling and duration summary."""
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch
from xml.etree import ElementTree

spec = importlib.util.spec_from_file_location('self_tests', Path(__file__).with_name('self-tests.py'))
self_tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(self_tests)


class SuiteTests(unittest.TestCase):
    def test_python_host_runner_is_discovered(self):
        runner = 'package/Example/tests/run-example-host.py'
        with patch.object(self_tests, 'tracked', return_value=[runner]):
            tests, unrunnable = self_tests.collect(['Example'], {})
        self.assertEqual(tests, [('py', runner, None)])
        self.assertEqual(unrunnable, [])

    def test_suite_member_needs_no_standalone_build_line(self):
        runner = 'package/Example/tests/run-example-host.sh'
        member = 'package/Example/tests/covered_host.c'
        unregistered = 'package/Example/tests/new_host.c'
        with (
            patch.object(self_tests, 'tracked', return_value=[runner, member, unregistered]),
            patch.object(self_tests, 'build_line', return_value=None),
        ):
            tests, unrunnable = self_tests.collect(['Example'], {member: f'SUITE: {runner}'})

        self.assertEqual(tests, [('sh', runner, None), ('c', member, None)])
        self.assertEqual(unrunnable, [unregistered])

    def test_tracked_paths_with_non_ascii_and_quotes(self):
        path = 'scripts/ci/test-é"quoted.py'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tracked = root / path
            tracked.parent.mkdir(parents=True)
            tracked.touch()
            subprocess.run(['git', 'init', '-q', tmp], check=True)
            subprocess.run(['git', '-C', tmp, 'add', '--', path], check=True)
            with patch.object(self_tests, 'ROOT', root):
                self.assertEqual(self_tests.collect(None, {}), ([('py', path, None)], []))

    def test_build_line_quotes_scratch_path_and_keeps_other_arguments(self):
        class Scratch:
            def __enter__(self):
                return str(root / 'scratch folder')

            def __exit__(self, *exc):
                pass

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'scratch folder').mkdir()
            fake = root / 'cc'
            fake.write_text('#!/bin/sh\ncp ./program "$2"\n')
            fake.chmod(0o755)
            program = root / 'program'
            program.write_text('#!/bin/sh\nprintf "%s\\n" "$1"\n')
            program.chmod(0o755)
            line = ('cc -o ./example && ./example ./example.extra'
                    ' && ./example "$(printf %s ./example)"')
            with patch.object(self_tests, 'ROOT', root), \
                 patch.object(self_tests.tempfile, 'TemporaryDirectory', return_value=Scratch()), \
                 patch.dict(os.environ, {'PATH': tmp + os.pathsep + os.environ['PATH']}):
                _, ok, _, output = self_tests.run_one('c', 'example.c', line)

        self.assertTrue(ok, output)
        self.assertEqual(output.splitlines(), ['./example.extra', str(root / 'scratch folder/bin/example')])

    def test_timeout_preserves_output_from_both_streams(self):
        err = subprocess.TimeoutExpired([], 1, output=b'stdout\xff', stderr=b'stderr')
        with patch.object(self_tests.subprocess, 'run', side_effect=err):
            _, ok, _, output = self_tests.run_one('py', 'test-example.py', None)
        self.assertFalse(ok)
        self.assertIn('stdout\ufffd', output)
        self.assertIn('stderr', output)

    def test_list_reports_stale_skips_only_when_unfiltered(self):
        kept = 'scripts/ci/test-actual.py'
        stale = 'scripts/ci/test-obsolete.py'
        for choice in ([], ['--only', 'actual']):
            with self.subTest(choice=choice):
                output = io.StringIO()
                with patch.object(sys, 'argv', ['self-tests.py', '--list', *choice]), \
                     patch.object(self_tests, 'tracked', return_value=[kept]), \
                     patch.object(self_tests, 'load_skips', return_value={kept: 'reason', stale: 'reason'}), \
                     redirect_stdout(output):
                    self.assertEqual(self_tests.main(), 0)
                self.assertEqual(stale in output.getvalue(), not choice)

    def test_repeated_filters_select_the_union_without_duplicates(self):
        k27 = 'tooling/k27/test-one.py'
        k273 = 'tooling/k273/test-two.py'
        other = 'tooling/elsewhere/test-three.py'
        output = io.StringIO()
        with patch.object(sys, 'argv', ['self-tests.py', '--list', '--only', 'tooling/k273', '--only', 'test-one',
                                       '--only', 'test-two']), \
             patch.object(self_tests, 'tracked', return_value=[k27, k273, other]), \
             patch.object(self_tests, 'load_skips', return_value={}), redirect_stdout(output):
            self.assertEqual(self_tests.main(), 0)
        self.assertEqual([line.split()[-1] for line in output.getvalue().splitlines()], [k27, k273])

    def test_junit_escapes_attributes_and_handles_non_xml_output(self):
        failed = 'scripts/ci/test-"failure".py'
        skipped = 'scripts/ci/test-"skip".py'
        reason = 'quoted "skip" & reason'
        with tempfile.TemporaryDirectory() as tmp:
            junit = Path(tmp) / 'test.xml'
            with patch.object(sys, 'argv', ['self-tests.py', '--junit', str(junit)]), \
                 patch.object(self_tests, 'tracked', return_value=[failed, skipped]), \
                 patch.object(self_tests, 'load_skips', return_value={skipped: reason}), \
                 patch.object(self_tests, 'run_one', return_value=(failed, False, 1, 'bad\x1b < output')), \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(self_tests.main(), 1)
            cases = {case.attrib['name']: case for case in ElementTree.parse(junit).getroot()}
        self.assertEqual(cases[failed].find('failure').text, 'bad\ufffd < output')
        self.assertEqual(cases[skipped].find('skipped').attrib['message'], reason)


class SchedulingTests(unittest.TestCase):
    def test_runs_known_slow_suites_first_and_lists_in_discovery_order(self):
        priority = [
            'tooling/persist/test-k273-only.py',
            'package/PianoPower/tests/run-charge-host.sh',
            'package/PianoA830/adreno830/tests/run-host-tests.py',
            'package/PianoVideo/tests/run-bringup-host.sh',
            'package/PianoCamera/tests/run-camera-host.sh',
            'tooling/k27/test-cs-native4k.sh',
        ]
        fast = ['package/Example/test-fast.py', 'scripts/ci/test-fast.py']
        tracked = sorted(priority + fast)
        started = []

        def run_one(_kind, path, _line, _subjob_limit):
            started.append(path)
            return path, True, 0, ''

        with (
            patch.object(self_tests, 'tracked', return_value=tracked),
            patch.object(self_tests, 'load_skips', return_value={}),
            patch.object(self_tests, 'run_one', side_effect=run_one),
        ):
            listing = io.StringIO()
            with patch.object(sys, 'argv', ['self-tests.py', '--list']), redirect_stdout(listing):
                self.assertEqual(self_tests.main(), 0)
            self.assertEqual([line.split()[-1] for line in listing.getvalue().splitlines()], tracked)
            self.assertEqual(started, [])

            with patch.object(sys, 'argv', ['self-tests.py', '--jobs', '1']), redirect_stdout(io.StringIO()):
                self.assertEqual(self_tests.main(), 0)
            self.assertEqual(started, priority + fast)

    def test_pool_refills_while_longest_suite_is_running(self):
        longest = 'tooling/persist/test-k273-only.py'
        second = 'package/PianoPower/tests/run-charge-host.sh'
        fast = 'scripts/ci/test-fast.py'
        first_wave = threading.Barrier(2)
        fast_started = threading.Event()
        started = []

        def run_one(_kind, path, _line, _subjob_limit):
            started.append(path)
            if path in (longest, second):
                first_wave.wait(timeout=5)
            if path == longest and not fast_started.wait(timeout=5):
                raise AssertionError('the free worker did not start the next suite')
            if path == fast:
                fast_started.set()
            return path, True, 0, ''

        with (
            patch.object(sys, 'argv', ['self-tests.py', '--jobs', '2']),
            patch.object(self_tests, 'tracked', return_value=sorted([longest, second, fast])),
            patch.object(self_tests, 'load_skips', return_value={}),
            patch.object(self_tests, 'run_one', side_effect=run_one),
            redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(self_tests.main(), 0)
        self.assertCountEqual(started[:2], [longest, second])
        self.assertEqual(started[2:], [fast])

    def test_inner_jobs_share_only_spare_cpu_slots(self):
        paths = sorted(self_tests.PARALLEL_SHELL_SUITES)
        limits = {}

        def run_one(_kind, path, _line, subjob_limit):
            limits[path] = subjob_limit
            return path, True, 0, ''

        for jobs, extra, expected in [('3', [], 1), ('2', [], 2), ('3', ['--only', 'run-charge-host'], 4)]:
            with self.subTest(jobs=jobs, extra=extra):
                limits.clear()
                with (
                    patch.object(sys, 'argv', ['self-tests.py', '--jobs', jobs, *extra]),
                    patch.object(self_tests.os, 'cpu_count', return_value=4),
                    patch.object(self_tests, 'tracked', return_value=paths),
                    patch.object(self_tests, 'load_skips', return_value={}),
                    patch.object(self_tests, 'run_one', side_effect=run_one),
                    redirect_stdout(io.StringIO()),
                ):
                    self.assertEqual(self_tests.main(), 0)
                selected = [self_tests.CHARGE_HOST_TESTS] if extra else paths
                self.assertEqual(limits, dict.fromkeys(selected, expected))


class SlowestTests(unittest.TestCase):
    def test_reports_ten_slowest_at_end_even_on_failure(self):
        paths = [f'scripts/ci/test-{i:02d}.py' for i in range(12)]
        tests = [('py', path, None) for path in reversed(paths)]
        durations = dict(zip(paths, [2.5, 4.5, 4.5, 6.5, 8.5, 10.5, 12.5, 14.5, 16.5, 18.5, 20.5, 22.5]))

        def run_one(_kind, path, _line, _subjob_limit):
            ok = path != paths[-1]
            return path, ok, durations[path], '' if ok else 'test failed'

        output = io.StringIO()
        with (
            patch.object(sys, 'argv', ['self-tests.py', '--jobs', '2']),
            patch.object(self_tests, 'collect', return_value=(tests, [])),
            patch.object(self_tests, 'load_skips', return_value={}),
            patch.object(self_tests, 'run_one', side_effect=run_one),
            redirect_stdout(output),
        ):
            self.assertEqual(self_tests.main(), 1)

        summary = output.getvalue().splitlines()[-11:]
        self.assertEqual(summary[0], 'Slowest 10 tests:')
        # The two tied tests compete for the tenth spot, so the earlier path wins.
        expected_paths = [*reversed(paths[3:]), paths[1]]
        self.assertEqual([line.split() for line in summary[1:]], [
            [f'{durations[path]:.1f}s', path] for path in expected_paths
        ])


if __name__ == '__main__':
    unittest.main()
