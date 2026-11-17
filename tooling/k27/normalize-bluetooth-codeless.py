#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Normalize one 27.3 codeless interface in a private copy of the KC input.

The stock metadata uses INT64_MAX for executable addresses, without a fileset
entry. The collection linker recognizes codeless bundles by absent address keys.
Keep its real library identity and gated properties; do not invent an executable.
"""
import os
import plistlib
import struct
import sys
from pathlib import Path

NAME = 'com.apple.driver.usb.AppleUSBHostPlatformProperties'
SENTINEL = (1 << 63) - 1


def normalize(source, output):
    if source.resolve() == output.resolve():
        raise ValueError('input must remain read-only')
    data = bytearray(source.read_bytes())
    if struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise ValueError('expected thin Mach-O collection')
    at, section, filesets = 32, None, set()
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if command == 0x19:
            for index in range(struct.unpack_from('<I', data, at + 64)[0]):
                name, segment, _, length, offset = struct.unpack_from('<16s16sQQI', data, at + 72 + index * 80)
                if name.rstrip(b'\0') == b'__info' and segment.rstrip(b'\0') == b'__PRELINK_INFO':
                    section = offset, length
        elif command == 0x80000035:
            name_at = at + struct.unpack_from('<I', data, at + 24)[0]
            filesets.add(bytes(data[name_at:data.index(0, name_at)]).decode())
        at += size
    if section is None:
        raise ValueError('no prelink metadata')
    offset, length = section
    info = plistlib.loads(bytes(data[offset:offset + length]).split(b'\0')[0])
    rows = [r for r in info['_PrelinkInfoDictionary'] if r['CFBundleIdentifier'] == NAME]
    changed = False
    if rows:
        if len(rows) != 1 or NAME in filesets:
            raise ValueError('codeless interface layout differs')
        row = rows[0]
        if row.get('_PrelinkExecutableLoadAddr') == SENTINEL:
            if row.get('_PrelinkExecutableSourceAddr') != SENTINEL or row.get('OSBundleLibraries') != {
                'com.apple.driver.AppleUSBHostMergeProperties': '1.2', 'com.apple.kpi.iokit': '23.0.0'
            }:
                raise ValueError('codeless interface dependencies or sentinel differ')
            del row['_PrelinkExecutableLoadAddr']
            del row['_PrelinkExecutableSourceAddr']
            changed = True
        elif '_PrelinkExecutableLoadAddr' in row:
            raise ValueError('unexpected executable address on interface')
    if changed:
        blob = plistlib.dumps(info, fmt=plistlib.FMT_XML) + b'\0'
        if len(blob) > length:
            raise ValueError('normalized metadata exceeds existing section')
        data[offset:offset + length] = blob + bytes(length - len(blob))
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + '.new')
    temporary.write_bytes(data)
    os.replace(temporary, output)
    print(f'Bluetooth codeless interface: {"normalized sentinel" if changed else "already represented"}; input unchanged')


if __name__ == '__main__':
    try:
        normalize(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from None
