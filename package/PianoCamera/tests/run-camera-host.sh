#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

CDPATH='' cd -- "$(dirname -- "$0")/../../.."
# shellcheck source=scripts/ci/host-test-jobs.sh
source scripts/ci/host-test-jobs.sh
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/piano-camera-host.XXXXXX")
trap 'rm -r -- "$build_dir"' EXIT

# The parser self-test uses its own synthetic tables. The native test uses the checked-in tables.
run_camera_test() {
  if [[ $1 == sensor_tables_self_test ]]; then
    python3 tooling/camera/sensor-tables.py --self-test
    return
  fi

  python3 tooling/camera/sensor-tables.py check data/camera
  python3 tooling/camera/sensor-tables.py header data/camera "$build_dir/CameraTables.h"

  # The model writes whole 16 MiB frames, so use the native build's -O1.
  "${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I package/PianoCamera -I package/PianoPower -I package/PianoKeyboard -I package/include -I "$build_dir" \
    package/PianoCamera/tests/camera_host.cpp -o "$build_dir/camera_host"
  PIANO_CAMERA_MANIFEST=data/camera/MANIFEST "$build_dir/camera_host"
}

run_host_jobs "$build_dir" run_camera_test sensor_tables_self_test camera_host
