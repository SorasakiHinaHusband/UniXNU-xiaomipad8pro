# SPDX-License-Identifier: BSD-2-Clause-Patent
# Source this to read the merged line, with a fallback for kernels before k27 0076.
piano_bootargs() {
  local args
  if args=$(LC_ALL=C sysctl -n kern.piano_bootargs 2>&1); then
    printf '%s\n' "$args"
  elif [ "$args" = "sysctl: unknown oid 'kern.piano_bootargs'" ]; then
    sysctl -n kern.bootargs 2>/dev/null
  else
    return 1
  fi
}
