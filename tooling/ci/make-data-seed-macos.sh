#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The 27.3 line's Data seed on a macOS host: the port of the k27 tree's scripts/make-macos-data-seed.sh, which builds it on a Linux
# host with apfsprogs, an apfs kernel module, losetup and chroot-free mounting. Here: the OS volume's /System/Library/Templates/Data
# into a fresh APFS image, macOS's firmlink skeleton, then DarwinBoot's packed_ramdisk.py to DBRDISK1 (the packed RAM disk the entry
# hands XNU as the Data device).
#
#   tooling/ci/make-data-seed-macos.sh OS_IMAGE OUT_DIR [SIZE_MIB]
#     OS_IMAGE  the decrypted 27.3 OS disk image (out/inputs/k273/26C5035r/141-22233-021.dmg)
#     OUT_DIR   receives scratch-data.img (the packed seed) and data.img (the APFS image, for inspection)
#
# KNOWN GAP G3 in the install skill: the reference seeds are the owner's, every other host has to make its own, and the maintainer's
# own note says the fresh recipe still needs a build and a RAM test. This script is that recipe on macOS. Sizes: the packed seed
# holds at most 256 MiB of nonzero pages, so the image is sparse and mostly zero.
set -euo pipefail
# newfs_apfs, diskutil and mkfile live in /usr/sbin: a hosted runner step may not have it on PATH
export PATH="/usr/sbin:/sbin:$PATH"
OS_IMAGE=${1:?the decrypted OS image}
OUT=${2:?the output directory}
SIZE_MIB=${3:-1024}
PACKER=${PACKED_RAMDISK:-${DARWINBOOT:-}/Test/packed_ramdisk.py}
[ -f "$PACKER" ] || { echo "no packed_ramdisk.py: set PACKED_RAMDISK or DARWINBOOT (the DarwinBoot checkout)" >&2; exit 1; }
[ -n "${SEED_TEMPLATE_DIR:-}" ] || [ -f "$OS_IMAGE" ] || { echo "no $OS_IMAGE" >&2; exit 1; }   # /dev/null and friends only make sense with SEED_TEMPLATE_DIR
mkdir -p "$OUT"
W=$(mktemp -d "${TMPDIR:-/tmp}/piano-seed.XXXXXX")
OS_MNT=
DATA_DEV=
DATA_MNT="$W/data-mnt"
mounted() { mount | grep -q " on $1 "; }   # macOS has no mountpoint(1)
cleanup() {
  [ -n "$DATA_MNT" ] && mounted "$DATA_MNT" && diskutil unmount force "$DATA_MNT" > /dev/null 2>&1 || true
  [ -n "$DATA_DEV" ] && hdiutil detach "$DATA_DEV" -force > /dev/null 2>&1 || true
  [ -n "$OS_MNT" ] && hdiutil detach "$OS_MNT" -force > /dev/null 2>&1 || true
  rm -rf "$W"
}
trap cleanup EXIT

# SEED_TEMPLATE_DIR: a template tree to use instead of the OS image (the smoke test of this script, tools/ci/seed-smoke.yml)
TEMPLATE=${SEED_TEMPLATE_DIR:-}
if [ -z "$TEMPLATE" ]; then
  echo "attaching $OS_IMAGE (read only)"
  ATTACH=$(hdiutil attach -nobrowse -readonly "$OS_IMAGE")
  echo "$ATTACH"
  # the volume is the one that carries the Data template; hdiutil's output names no "Apple_APFS" line on this image
  OS_MNT=
  for m in /Volumes/*; do [ -d "$m/System/Library/Templates/Data" ] && { OS_MNT=$m; break; }; done
  if [ -z "$OS_MNT" ]; then
    echo "no /System/Library/Templates/Data among /Volumes/* (attach output: $ATTACH)" >&2
    for m in /Volumes/*; do [ -d "$m/System/Library" ] && ls "$m/System/Library/Templates" 2>/dev/null; done
    exit 1
  fi
  echo "template volume: $OS_MNT"
  TEMPLATE="$OS_MNT/System/Library/Templates/Data"
fi
[ -d "$TEMPLATE" ] || { echo "no template directory $TEMPLATE" >&2; exit 1; }

echo "a $SIZE_MIB MiB APFS image, volume Data"
IMG=$OUT/data.img
rm -f "$IMG"
mkfile -n "${SIZE_MIB}m" "$IMG"
DATA_DEV=$(hdiutil attach -nomount -nobrowse "$IMG" | awk '/^\/dev\/disk/ {print $1; exit}')
[ -n "$DATA_DEV" ] || { echo "hdiutil did not attach $IMG" >&2; exit 1; }
newfs_apfs -v Data "$DATA_DEV" > /dev/null
# newfs_apfs leaves an APFS container that may live on a synthesized device: ask diskutil which volume belongs to the
# physical store we just formatted, instead of guessing <device>s1 (guessing mounted the runner root disk in smoke run 4).
DATA_VOL_IDS=$(diskutil apfs list -plist | plutil -convert json -o - - | python3 -c '
import json, sys
want = sys.argv[1].replace("/dev/", "")   # diskutil names devices without the /dev/ prefix
data = json.load(sys.stdin)
for container in data.get("Containers", []):
    stores = [s.get("DeviceIdentifier", "") for s in container.get("PhysicalStores", [])]
    if any(s == want or s.startswith(want) for s in stores):
        for volume in container.get("Volumes", []):
            print(volume.get("DeviceIdentifier", ""))
        break
' "$DATA_DEV")
DATA_VOL=$(printf "%s\n" "$DATA_VOL_IDS" | awk 'NF {print "/dev/" $1; exit}')
[ -n "$DATA_VOL" ] || { echo "no APFS volume on the container of $DATA_DEV (physical stores: $(diskutil apfs list | head -20 | tr "\n" " "))" >&2; exit 1; }
echo "container on $DATA_DEV, volume $DATA_VOL"
# macOS usually auto-mounts the new volume (often at /Volumes/Data); use that mount point when it did
if mount | grep -q "^$DATA_VOL "; then
  DATA_MNT=$(mount | awk -v d="$DATA_VOL" '$1 == d {line = $0; sub(/^.* on /, "", line); sub(/ \\(.*/, "", line); print line; exit}')
  echo "already mounted at $DATA_MNT"
else
  mkdir -p "$DATA_MNT"
  diskutil mount -mountPoint "$DATA_MNT" "$DATA_VOL" > /dev/null || diskutil mount "$DATA_VOL" > /dev/null
  # diskutil mount without a mount point puts the volume under /Volumes: take wherever it really landed
  if mount | grep -q "^$DATA_VOL "; then
    REAL=$(mount | awk -v d="$DATA_VOL" '$1 == d {line = $0; sub(/^.* on /, "", line); sub(/ \(.*/, "", line); print line; exit}')
    if [ "$REAL" != "$DATA_MNT" ]; then echo "mounted at $REAL"; DATA_MNT=$REAL; fi
  else
    echo "the new APFS volume did not mount:" >&2; diskutil info "$DATA_VOL" 2>&1 | head -8 >&2; exit 1
  fi
fi
mounted "$DATA_MNT" || { echo "no mount point for $DATA_VOL" >&2; exit 1; }

echo "the template's /private/var skeleton"
sudo mkdir -p "$DATA_MNT/private/var"
sudo rsync -aH "$TEMPLATE/private/var/" "$DATA_MNT/private/var/"
for directory in tmp log run vm folders db/dslocal/nodes/Default; do
  sudo mkdir -p "$DATA_MNT/private/var/$directory"
done
sudo chmod 1777 "$DATA_MNT/private/var/tmp"
sudo mkdir -p -m 1777 "$DATA_MNT/private/tmp"
# The bulky read-only parts stay on the SystemOS disk; the same exclusions as the Linux recipe.
READONLY=(Library/Apple "Library/Application Support/Apple" Library/Documentation Library/WebServer)
for path in Library Users Applications Volumes cores opt usr/local System/Library/Caches System/Volumes/Hardware private/etc; do
  sudo mkdir -p "$DATA_MNT/$(dirname "$path")"
  if [ -e "$TEMPLATE/$path" ]; then
    EX=()
    for r in "${READONLY[@]}"; do EX+=("--exclude=/$r"); done
    sudo rsync -aH ${EX[@]+"${EX[@]}"} "$TEMPLATE/$path" "$DATA_MNT/$(dirname "$path")/"
  else
    sudo mkdir -p "$DATA_MNT/$path"
  fi
done
for path in "${READONLY[@]}"; do
  sudo ln -sfn "/System/Library/DroidVMData/$path" "$DATA_MNT/$path"
done
sudo ln -sfn /System/Cryptexes "$DATA_MNT/System/Cryptexes"
sudo chmod 1777 "$DATA_MNT/Volumes" 2>/dev/null || true
sudo find "$DATA_MNT" -maxdepth 2 | sort | head -40
sync
diskutil unmount force "$DATA_MNT" > /dev/null
hdiutil detach "$DATA_DEV" -force > /dev/null
DATA_DEV=

echo "packing to DBRDISK1 with $PACKER"
python3 "$PACKER" "$IMG" "$OUT/scratch-data.img"
ls -la "$OUT"
echo "Data seed: $OUT/scratch-data.img from $OS_IMAGE"
