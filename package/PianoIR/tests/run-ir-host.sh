#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
D=$(cd "$(dirname "$0")/../../.." && pwd)
P="$D/package/PianoIR"

flags=(-Wall -Wextra -Werror)
if [ "${1:-}" = --sanitize ]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  shift
fi
if [ "$#" -eq 2 ] && [ "$1" = --out ]; then
  out=$2
  mkdir -p "$out"
elif [ "$#" -eq 0 ]; then
  out=$(mktemp -d "${TMPDIR:-/tmp}/piano-ir.XXXXXX")
  trap 'rm -rf "$out"' EXIT
else
  echo "usage: $0 [--sanitize] [--out DIR]" >&2
  exit 2
fi

"${CC:-cc}" -std=c11 "${flags[@]}" -c "$D/package/PianoTouch/GeniSpi.c" -o "$out/GeniSpi.o"
"${CXX:-c++}" -std=c++20 "${flags[@]}" -I "$P" -I "$D/package/PianoTouch" "$P/tests/ir_checks.cpp" "$out/GeniSpi.o" -o "$out/ir_checks"
"$out/ir_checks"

if [ "$(uname)" = Darwin ]; then
  cli="$out/piano-ir"
  "${CC:-cc}" -std=c11 -O2 "${flags[@]}" -I "$P" -framework IOKit -framework CoreFoundation \
    "$D/tooling/native/piano-ir/piano-ir.c" -o "$cli"

  # These cases must fail during argument checking, before opening an IOKit service.
  check_usage() {
    local expected=$1 output rc=0
    shift
    output=$("$cli" "$@" 2>&1) || rc=$?
    if [ "$rc" -ne 2 ] || [[ "$output" != *"$expected"* ]]; then
      printf 'PianoIR CLI regression (%s): exit %s, %s\n' "$*" "$rc" "$output" >&2
      exit 1
    fi
  }
  check_usage 'usage:' raw --carrier 4295005296 1000
  check_usage 'usage:' raw --duty 4294967336 1000
  check_usage 'usage:' raw --carrier 0 1000
  check_usage 'usage:' nec 0x00 0x45 --carrier 56001
  check_usage 'usage:' nec 0x00 0x45 --repeat 99999999999999999999999
  check_usage 'usage:' blink 121
  check_usage 'usage:' status extra
  check_usage 'zero duration' raw 0
  check_usage 'longer than 500 ms' raw 300000 100000 100001
  check_usage 'not a duration' raw - <<< 'pulse 1000 space xyz pulse 562'
  echo "PianoIR CLI tests: 10 checks, 0 failures"
fi
