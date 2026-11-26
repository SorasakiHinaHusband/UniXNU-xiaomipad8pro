#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise pinned checkout recovery against an entirely local repository."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


FETCH = Path(__file__).resolve().parents[1] / 'fetch-pins.py'


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


class FetchPins(unittest.TestCase):
    def test_retries_checkout_interrupted_before_origin_was_added(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'source'
            research = root / 'research'
            checkout = research / 'fixture'
            source.mkdir()
            checkout.mkdir(parents=True)
            git(source, 'init', '-q')
            git(source, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test',
                'commit', '--allow-empty', '-qm', 'Pin fixture')
            commit = git(source, 'rev-parse', 'HEAD')
            git(checkout, 'init', '-q')

            lock = root / 'fixture.lock'
            lock.write_text(f'[pin.fixture]\ngit = {json.dumps(source.as_uri())}\ncommit = "{commit}"\n')
            result = subprocess.run(
                [sys.executable, str(FETCH), '--lock', str(lock), '--only', 'fixture'],
                env=dict(os.environ, UNIXNU_RESEARCH=str(research)),
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(git(checkout, 'rev-parse', 'HEAD'), commit)


if __name__ == '__main__':
    unittest.main()
