#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/firmware-inputs.XXXXXX")
trap 'rm -rf -- "$W"' EXIT

if DARWINBOOT="$W/no checkout" bash "$D/test-darwinboot-extra-ram.sh" > "$W/log" 2>&1; then
  echo 'the test accepted an explicitly missing DarwinBoot checkout' >&2
  exit 1
fi
grep -F 'FAIL: DARWINBOOT checkout does not contain' "$W/log"
env -u DARWINBOOT UNIXNU_RESEARCH="$W/no sources" bash "$D/test-darwinboot-extra-ram.sh" > "$W/log" 2>&1
grep -F 'SKIP: no DarwinBoot checkout' "$W/log"

if bash "$D/make-own-binaries.sh" "$W/no image.zst" "$W/output dir" > "$W/log" 2>&1; then
  echo 'the extractor accepted a missing UEFI image' >&2
  exit 1
fi
grep -F 'missing UEFI input image' "$W/log"
test ! -e "$W/output dir"
