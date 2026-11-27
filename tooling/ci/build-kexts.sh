#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Our native kexts for the 27.3 line on a machine that has run tooling/ci/k27-kernel.sh (the k27 tree with its native kernel and the
# kernel's object recipes, which every kext compiles with): inside tooling/ci/with-kernelcache.sh (the kernelcache member only, deleted
# afterwards) the 27.3 clone of the tree is prepared and its kernel relinked at the collection's addresses
# (tooling/k273/prepare-k27-tree.sh), then tooling/k27/build-native-image.sh builds the kexts of configs/k27-native-kexts.txt and
# stops there (PIANO_STOP_AFTER=kexts): no kernel collection, ADT or image is made (those are built on the user's side).
#
#   tooling/ci/build-kexts.sh      -> $OUT/piano-kexts.tar.gz (out/native-kexts: our objects only), $OUT/kernel-k273-native.macho, *.sha256
#   Environment: WORK, OUT, K27_DIR, K27_TREE_TOP as tooling/ci/k27-kernel.sh.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
WORK=${WORK:-$PWD/out/ci}; OUT=${OUT:-$WORK/out}; M=${K27_DIR:-$WORK/k27-tree}; TOP=${K27_TREE_TOP:-0066}
[ -f "$M/out/k27/kernel-k27-$TOP-native.macho" ] || { echo "no $M/out/k27/kernel-k27-$TOP-native.macho: tooling/ci/k27-kernel.sh first" >&2; exit 1; }
mkdir -p "$OUT" out/k27 out/k273
export WORK OUT M TOP
SOC_DIR="$WORK/UniXNU-sm8750-common"
if [ ! -d "$SOC_DIR/kits/Sm8750Platform" ]; then
  read -r SOC_URL SOC_REV < <(python3 -c 'import tomllib; d=tomllib.load(open("unixnu.lock","rb"))["pin"]["UniXNU-sm8750-common"]; print(d["git"], d["commit"])')
  git clone "$SOC_URL" "$SOC_DIR"
  git -C "$SOC_DIR" checkout "$SOC_REV"
fi
ln -sfn "$SOC_DIR" "$D/UniXNU-sm8750-common"
ln -sfn "$SOC_DIR" "$(dirname "$D")/UniXNU-sm8750-common"
export SOC_DIR
EXTRA_INC="$WORK/extra-inc"
SDK_KH="$(xcrun --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Headers"
mkdir -p "$EXTRA_INC/IOKit"
ln -sfn "$SDK_KH/IOKit/storage" "$EXTRA_INC/IOKit/storage"
ln -sfn "$SDK_KH/TargetConditionals.h" "$EXTRA_INC/TargetConditionals.h"
for f in "$SDK_KH"/Availability*.h; do [ -e "$f" ] && ln -sfn "$f" "$EXTRA_INC/"; done
export EXTRA_INC
bash tooling/ci/with-kernelcache.sh bash -c '
  set -euo pipefail
  T=$WORK/k273-tree
  K=$(K273_TREE=$T PIANO_K27_TREE=$M bash tooling/k273/prepare-k27-tree.sh "$TOP" | tail -1)
  [ -f "$K" ] || { echo "prepare-k27-tree.sh gave no kernel" >&2; exit 1; }
  cp "$K" "$OUT/kernel-k273-native.macho"
  mkdir -p "$T/out/kc"
  # kc27.bin is the paravirt reference collection when the owner supplied one (it names the pure virtual slots the
  # IOMobileFramebuffer declarations need); otherwise the target collection stands in for it.
  [ -e "$T/out/kc/kc27.bin" ] || ln -sfn "$K273_KC" "$T/out/kc/kc27.bin"
  # NativeGopDisplay needs IOMobileFramebuffer declarations: from a paravirt reference collection when the caller supplies one
  # (IOMFB_ABI_REFERENCE_KC), else from the declarations the k27 tree commits, and only if tooling/k27/check-iomfb-abi-header.py
  # finds every slot and PAC diversity identical in this collection. The kext script decides that itself; a failure there is one
  # FAIL line in kexts-status.txt below, not a reason to leave the kext out unseen.
  KEXT_LIST=configs/k27-native-kexts.txt
  export PIANO_K27_TREE=$T K27_KERNEL=$K KC_BASE=$K273_KC KC_BUILDER=k27/kc-build273-piano.py PIANO_ADT_IDENTITY=configs/k273-adt.toml
  export CPATH="$EXTRA_INC:$SOC_DIR/kits" PIANO_STOP_AFTER=kexts
  # A hosted runner has no device-local inputs: the pinned boot DT, and the vendor firmware images (GPU zap, VPU, MCU, audio
  # preset). Those stay off a public CI artifact anyway (docs/ci.md: Apple and vendor payloads never leave the runner), so build
  # the kexts without them, one at a time, and let kexts-status.txt say per kext what this job could build and what it could not:
  # one kext that needs a local input must not take the whole build down.
  # A hosted runner has no bootstrap that generated out/x12377/inc-hid and inc-net-piano (Known gap G1: those come from the
  # owner's non-public xnu-linux workspace). The headers themselves are Apple's: seed the two include trees from the macOS SDK's
  # Kernel.framework when they are missing, so the kexts that need them can at least be compiled and checked here. This is NOT the
  # owner's generated set: tools/ci labels the result as built against SDK headers.
  SDK_KH="$(xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Headers"
  for pair in "inc-hid:IOKit/hid" "inc-net-piano:IOKit/network"; do
    d=${pair%%:*}; sub=${pair#*:}
    if [ ! -d "$T/out/x12377/$d/$sub" ] && [ -d "$SDK_KH/$sub" ]; then
      mkdir -p "$T/out/x12377/$d/$sub"
      cp -R "$SDK_KH/$sub/." "$T/out/x12377/$d/$sub/"
      echo "inc: $d seeded from the SDK's $sub ($(ls "$T/out/x12377/$d/$sub" | wc -l | tr -d " ") headers)"
    fi
  done
  export PIANO_GPU_NO_FIRMWARE=1 PIANO_KEYBOARD_NO_FIRMWARE=1
  : > "$WORK/kexts-status.txt"
  built=0; left=
  while read -r name rest; do
    case $name in ""|\#*) continue ;; esac
    printf "%s\n" "$name" > "$WORK/one-kext.txt"
    if PIANO_KEXT_LIST=$WORK/one-kext.txt bash tooling/k27/build-native-image.sh > "$WORK/kext-$name.log" 2>&1; then
      echo "ok   $name" | tee -a "$WORK/kexts-status.txt"
      built=$((built + 1))
    else
      echo "FAIL $name  $(tail -1 "$WORK/kext-$name.log")" | tee -a "$WORK/kexts-status.txt"
      echo "::warning::kext $name did not build on this runner: $(tail -2 "$WORK/kext-$name.log" | tr "\n" " ")"
      left="$left $name"
    fi
  done < "$KEXT_LIST"
  echo "kexts built: $built; left out:${left:- none}"
  [ "$built" -gt 0 ] || { echo "no kext built at all" >&2; exit 1; }
'
# our objects only (out/native-kexts/NAME/*.o, no generated headers or host-test outputs) and the descriptors; nothing of the
# kernelcache is in there
( cd out && find native-kexts -type f -name '*.o' | sort > "$WORK/kext-objects.txt" )
[ -s "$WORK/kext-objects.txt" ] || { echo "no kext objects under out/native-kexts" >&2; exit 1; }
# A staging tree, so the archive has exactly two roots (native-kexts/, tooling/) and no -C/-T ordering that bsdtar and GNU tar
# read differently: operands may not follow options, and a -T list is resolved against the -C in force when it is read.
STAGE=$WORK/kexts-stage; rm -rf "$STAGE"; mkdir -p "$STAGE"
( cd out && tar -cf - -T "$WORK/kext-objects.txt" ) | tar -xf - -C "$STAGE"
mkdir -p "$STAGE/tooling/k27"; cp -R "$D/tooling/k27/kext" "$STAGE/tooling/k27/kext"
tar -czf "$OUT/piano-kexts.tar.gz" -C "$STAGE" native-kexts tooling
( cd "$OUT" && shasum -a 256 piano-kexts.tar.gz kernel-k273-native.macho > kexts.sha256 && cat kexts.sha256 )
