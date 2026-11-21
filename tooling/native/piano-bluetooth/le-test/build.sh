#!/bin/bash
# build.sh OUT: the LE test's two host-run binaries, ad-hoc signed arm64: le-peripheral (for the Mac next to the tablet) and
# piano-bt-le-central (probe.m for a pushed run on an image whose trust-cached piano-bt-probe predates --le-read).
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd); O=${1:?usage: build.sh OUT}
mkdir -p "$O"; SDK=$(xcrun --sdk macosx --show-sdk-path)
F=(-O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -fstack-protector-strong -fobjc-arc -fblocks -framework Foundation -framework CoreBluetooth)
xcrun --sdk macosx clang -target arm64-apple-macos27.0 -isysroot "$SDK" "${F[@]}" "$H/le-test/le-peripheral.m" -o "$O/le-peripheral"
xcrun --sdk macosx clang -target arm64-apple-macos27.0 -isysroot "$SDK" "${F[@]}" -framework IOBluetooth "$H/probe.m" -o "$O/piano-bt-le-central"
cp "$H/le-test/le-peripheral.command" "$O/le-peripheral.command"
codesign --force --sign - --identifier com.unixnu.piano.bluetooth.le-peripheral "$O/le-peripheral"
codesign --force --sign - --identifier com.unixnu.piano.bluetooth.le-central "$O/piano-bt-le-central"
xattr -c "$O"/le-peripheral "$O"/piano-bt-le-central "$O"/le-peripheral.command 2>/dev/null || true
shasum -a 256 "$O"/le-peripheral "$O"/piano-bt-le-central
