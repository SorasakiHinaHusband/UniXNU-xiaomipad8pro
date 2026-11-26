#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Capture with the tablet harness, then fetch the root-owned PNG over its verified chunk transport.
set -euo pipefail
umask 077
here=$(cd "$(dirname "$0")" && pwd)
layout=()
case "${1:-}" in
    --bga8|--10bit) layout=("$1"); shift ;;
esac
if [ "$#" != 3 ] || [[ $1 != /* || $2 != /* || -z $3 ]]; then
    echo "usage: capture-ssh.sh [--bga8|--10bit] /tablet/gop-shot /tablet/output.png MAC_OUTPUT.png" >&2
    exit 2
fi
remote_tool=$1 remote_png=$2 local_png=$3
case "$local_png" in /*) ;; *) local_png=$PWD/$local_png ;; esac
if [ -e "$local_png" ] || [ -L "$local_png" ]; then
    echo "capture-ssh: the Mac output already exists" >&2
    exit 1
fi
scratch=$(mktemp -d "$(dirname "$local_png")/.gop-shot.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT

export GOP_SHOT_TSUDO=${GOP_SHOT_TSUDO:-/tmp/tsudo}
rsudo() { "$GOP_SHOT_TSUDO" --raw "$1"; }
export -f rsudo

command=$(python3 -c 'import shlex, sys; print(shlex.join(sys.argv[1:]))' "$remote_tool" ${layout[@]+"${layout[@]}"} "$remote_png")
rsudo "$command"
python3 "$here/../../persist/fetch-chunks.py" rsudo "$remote_png" "$scratch/frame.png"
# The verified file and destination share a filesystem. Publish without replacing an existing path.
python3 - "$scratch/frame.png" "$local_png" <<'PY'
import os
import sys

source, dest = sys.argv[1:]
with open(source, 'rb') as png:
    header = png.read(16)
    size = png.seek(0, os.SEEK_END)
    if size < 57 or header != b'\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR':
        sys.exit('capture-ssh: invalid or empty PNG')
    png.seek(-12, os.SEEK_END)
    if png.read(12) != b'\x00\x00\x00\x00IEND\xaeB\x60\x82':
        sys.exit('capture-ssh: incomplete PNG')
os.link(source, dest)
PY
