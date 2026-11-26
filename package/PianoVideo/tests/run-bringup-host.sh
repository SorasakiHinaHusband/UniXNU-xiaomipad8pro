#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

CDPATH='' cd -- "$(dirname -- "$0")/../../.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/piano-video-host.XXXXXX")
trap 'rm -r -- "$build_dir"' EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c package/PianoVideo/QcomMdt.c -o "$build_dir/QcomMdt.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I package/PianoVideo -I package/include \
  package/PianoVideo/tests/bringup_host.cpp "$build_dir/QcomMdt.o" -o "$build_dir/bringup_host"
# The model uses a synthetic image. The native build can also check the optional vpu35_4v.mbn.
VIDEO_HOST_FW='' "$build_dir/bringup_host"
