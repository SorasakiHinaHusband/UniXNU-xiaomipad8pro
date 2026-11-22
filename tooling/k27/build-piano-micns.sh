#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang.
# Build package/PianoMicNS ("Mic (noise reduced)", private/host-checks MIC-NS.md section 8): the host tests first (the core against
# PianoAudio's tap ABI, package/PianoAudio/AudioTap.h, in simulated time: passthrough integrity under +-200 ppm drift, RNNoise on
# white noise and silence, the tap's events), then the AudioServerPlugIn bundle out/piano-micns/PianoMicNS.driver: arm64 (the HAL's
# driver service, com.apple.audio.Core-Audio-Driver-Service.helper, is arm64e and loads arm64 plug-ins), macOS 14, -Wall -Wextra
# -Werror for this repository's files, RNNoise (third_party/rnnoise, BSD-3-Clause, unmodified) with its own warnings off, signed ad
# hoc as a bundle (codesign: the Info.plist and the executable sealed; the driver service refuses an invalid signature). Prints the
# executable's CDHash (it goes into the trust cache with the userland layer) and writes out/piano-micns/build.json.
#   environment: CC (default xcrun clang), PIANO_MICNS_WAV (optional: a recording, mono 16-bit 24/48 kHz, through micns_host --wav)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
P=$D/package/PianoMicNS; R=$P/third_party/rnnoise; A=$D/package/PianoAudio
O=$D/out/piano-micns; T=$O/host-tests; B=$O/PianoMicNS.driver
CC=${CC:-xcrun clang}
rm -rf "$O"; mkdir -p "$T" "$O/obj"
RN=(denoise rnn rnn_data pitch kiss_fft celt_lpc)
RFLAGS=(-std=c11 -O2 -w -DNDEBUG -I "$R/include" -I "$R/src")
OURS=(-std=c11 -O2 -Wall -Wextra -Werror -I "$P" -I "$A" -I "$R/include")
# 1 host tests (this Mac's arch)
for f in "${RN[@]}"; do $CC "${RFLAGS[@]}" -c "$R/src/$f.c" -o "$T/rn-$f.o"; done
$CC "${OURS[@]}" "$P/tests/micns_host.c" "$P/MicNsCore.c" "$T"/rn-*.o -lm -o "$T/micns_host"
"$T/micns_host" || { echo "Error: PianoMicNS host tests failed" >&2; exit 1; }
$CC -std=c11 -Wall -Wextra -Werror -I "$A" "$A/tests/tap_host.c" -o "$T/tap_host" && "$T/tap_host"
if [ -n "${PIANO_MICNS_WAV:-}" ]; then "$T/micns_host" --wav "$PIANO_MICNS_WAV" "$O/micns-host-out.wav"; fi
# 2 the bundle: arm64, macOS 14
ARCH=(-arch arm64 -mmacosx-version-min=14.0)
for f in "${RN[@]}"; do $CC "${ARCH[@]}" "${RFLAGS[@]}" -c "$R/src/$f.c" -o "$O/obj/rn-$f.o"; done
$CC "${ARCH[@]}" "${OURS[@]}" -c "$P/MicNsCore.c" -o "$O/obj/MicNsCore.o"
$CC "${ARCH[@]}" "${OURS[@]}" -c "$P/PianoMicNS.c" -o "$O/obj/PianoMicNS.o"
mkdir -p "$B/Contents/MacOS"
cp "$P/Info.plist" "$B/Contents/Info.plist"
$CC "${ARCH[@]}" -bundle "$O/obj/PianoMicNS.o" "$O/obj/MicNsCore.o" "$O"/obj/rn-*.o -framework CoreAudio -framework CoreFoundation \
  -framework IOKit -lm -Wl,-exported_symbol,_PianoMicNS_Create -o "$B/Contents/MacOS/PianoMicNS"
plutil -lint "$B/Contents/Info.plist" > /dev/null
nm -u "$B/Contents/MacOS/PianoMicNS" | grep -q '_IOServiceOpen' || { echo "Error: the plug-in does not open the tap" >&2; exit 1; }
nm -g "$B/Contents/MacOS/PianoMicNS" | grep -q ' T _PianoMicNS_Create$' || { echo "Error: no factory symbol" >&2; exit 1; }
codesign --force --sign - --timestamp=none "$B"
codesign --verify --strict --verbose=1 "$B" 2>&1 | tail -1
CDHASH=$(codesign -dvvv "$B" 2>&1 | sed -n 's/^CDHash=\([0-9a-f]*\)$/\1/p' | head -1)
[ -n "$CDHASH" ] || { echo "Error: no CDHash" >&2; exit 1; }
python3 - "$B" "$CDHASH" "$O/build.json" "$D" <<'PY'
import hashlib, json, pathlib, subprocess, sys
b, cdhash, out, d = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
files = {str(p.relative_to(b)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(b.rglob('*')) if p.is_file()}
commit = subprocess.run(['git', '-C', d, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
json.dump({'bundle': b.name, 'cdhash': cdhash, 'files': files, 'commit': commit, 'arch': 'arm64', 'min_os': '14.0'},
          open(out, 'w'), indent=1)
PY
echo "PianoMicNS: $B (CDHash $CDHASH, $(stat -f %z "$B/Contents/MacOS/PianoMicNS") bytes)"
