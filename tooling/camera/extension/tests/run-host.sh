#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
set -eu
cd "$(dirname "$0")"
work=$(mktemp -d "${TMPDIR:-/tmp}/piano-camera-output.XXXXXX")
trap 'rm -rf "$work"' EXIT
xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  output_host.c -o "$work/output-host"
"$work/output-host"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -O1 -g \
  -Wall -Wextra -Werror -Wno-unused-parameter -fobjc-arc -fsanitize=address,undefined \
  provider_host.m -o "$work/provider-host" \
  -framework Foundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo \
  -framework IOSurface -framework IOKit -framework CoreGraphics
"$work/provider-host"
