#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host build/self-test only. No target access; no private entitlement.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
O=${1:-$D/out/native-tools/video-v6}
mkdir -p "$(dirname "$O")"
xcrun clang++ -std=c++17 -arch arm64 -mmacosx-version-min=15.0 -O2 -Wall -Wextra -Werror \
  -Wno-deprecated-declarations -fobjc-arc -I "$D/package/PianoVideo" "$D/tooling/native/video-v6.mm" \
  -framework Foundation -framework AVFoundation -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
  -framework VideoToolbox -framework IOSurface -o "$O"
codesign -f -s - "$O"
"$O" --self-test "$D/package/PianoVideo/tests/fixtures/v6"
printf 'Built %s\n' "$O"
