#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise both extractors with synthetic ACDB and tuning data."""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent

for name in ("spk-prot-extract.py", "spk-dolby-extract.py"):
    subprocess.run([sys.executable, HERE / name, "--self-test"], check=True)
