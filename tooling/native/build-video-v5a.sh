#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Ordinary arm64 tool; no private entitlement and no target access.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
O=${1:-$D/out/native-tools/video-v5a}
mkdir -p "$(dirname "$O")"
xcrun clang++ -std=c++17 -arch arm64 -mmacosx-version-min=15.0 -O2 -Wall -Wextra -Werror \
  -Wno-deprecated-declarations -I "$D/package/PianoVideo" "$D/tooling/native/video-v5a.cpp" \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework IOSurface -o "$O"
codesign -f -s - "$O"
"$O" --self-test
printf 'Built %s\n' "$O"
