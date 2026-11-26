#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d)
trap 'rm -rf "$O"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "$D/tests/args_host.c" -o "$O/args_host"
"$O/args_host"
