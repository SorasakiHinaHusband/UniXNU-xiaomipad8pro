#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

CDPATH='' cd -- "$(dirname -- "$0")/../../.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/piano-camera-host.XXXXXX")
trap 'rm -r -- "$build_dir"' EXIT

python3 tooling/camera/sensor-tables.py --self-test
python3 tooling/camera/sensor-tables.py check data/camera
python3 tooling/camera/sensor-tables.py header data/camera "$build_dir/CameraTables.h"

# The model writes whole 16 MiB frames, so use the native build's -O1.
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I package/PianoCamera -I package/PianoPower -I package/PianoKeyboard -I package/include -I "$build_dir" \
  package/PianoCamera/tests/camera_host.cpp -o "$build_dir/camera_host"
PIANO_CAMERA_MANIFEST=data/camera/MANIFEST "$build_dir/camera_host"
