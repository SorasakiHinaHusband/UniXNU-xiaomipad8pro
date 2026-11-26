#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

CDPATH='' cd -- "$(dirname -- "$0")/../../.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/piano-video-host.XXXXXX")
trap 'rm -r -- "$build_dir"' EXIT

checks=(-O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all)
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1}"
export UBSAN_OPTIONS="halt_on_error=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"

"${CC:-cc}" -std=c11 "${checks[@]}" -c package/PianoVideo/QcomMdt.c -o "$build_dir/QcomMdt.o"
"${CXX:-c++}" -std=c++20 "${checks[@]}" -I package/PianoVideo -I package/include \
  package/PianoVideo/tests/bringup_host.cpp "$build_dir/QcomMdt.o" -o "$build_dir/bringup_host"
# The model uses a synthetic image. The native build can also check the optional vpu35_4v.mbn.
VIDEO_HOST_FW='' "$build_dir/bringup_host"

# Compile the production VTHost worker for the cancel test. Only kernel APIs
# are substituted; the test includes the production translation unit directly.
for header in IOKit/IOLib.h pexpert/pexpert.h sys/proc.h IOKit/IOService.h \
  IOKit/IOUserClient.h IOKit/IOMemoryDescriptor.h kern/thread.h kern/clock.h libkern/crypto/sha2.h; do
  mkdir -p "$build_dir/shim/$(dirname "$header")"
  printf '#include "vt_cancel_kernel_shim.h"\n' > "$build_dir/shim/$header"
done
for test in video_host geometry_host session_host stream_host hevc_host enc_host vt_host vt_surface_host vt_sync_host vt_contract_host vt_cancel_host; do
  "${CXX:-c++}" -std=c++20 "${checks[@]}" -I "$build_dir/shim" -I package/PianoVideo/tests \
    -I package/PianoVideo -I package/PianoVideo/vt -I package/include \
    "package/PianoVideo/tests/$test.cpp" -o "$build_dir/$test"
  "$build_dir/$test"
done
python3 package/PianoVideo/tests/enc_stream_check.py --self-test --pattern-tool "$build_dir/enc_host"
python3 package/PianoVideo/tests/fixture_host.py
python3 package/PianoVideo/tests/hevc_files_host.py --host "$build_dir/hevc_host" \
  package/PianoVideo/tests/fixtures/v9/main10-1080.mp4 package/PianoVideo/tests/fixtures/v9/main8-1080.mp4
if [ "$(uname -s)" = Darwin ]; then
  "${CXX:-c++}" -std=c++20 "${checks[@]}" -Wno-deprecated-declarations \
    package/PianoVideo/tests/vt_serializer_host.mm -framework CoreFoundation -framework CoreMedia \
    -framework CoreVideo -framework VideoToolbox -o "$build_dir/vt_serializer_host"
  "$build_dir/vt_serializer_host"
fi
