#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Verify the actual linked Bluetooth dependency personalities before spending a boot."""
import json
import plistlib
import struct
import sys
from pathlib import Path


def metadata(path):
    data = Path(path).read_bytes()
    at = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if command == 0x19:
            for index in range(struct.unpack_from('<I', data, at + 64)[0]):
                name, segment, _, length, offset = struct.unpack_from('<16s16sQQI', data, at + 72 + index * 80)
                if name.rstrip(b'\0') == b'__info' and segment.rstrip(b'\0') == b'__PRELINK_INFO':
                    return plistlib.loads(data[offset:offset + length].rstrip(b'\0'))['_PrelinkInfoDictionary']
        at += size
    raise ValueError('no prelink metadata')


def main():
    entries = {row['CFBundleIdentifier']: row for row in metadata(sys.argv[1])}
    descriptor = json.loads((Path(__file__).parent / 'kext/PianoBluetooth.json').read_text())
    problems = []
    usb_profile = any(b in entries for b in ('com.unixnu.driver.PianoUsbHostStack',
                                           'com.unixnu.driver.PianoUsbHostStackA2'))
    shim = json.loads((Path(__file__).parent / 'kext/PianoUsbHostStandIn.json').read_text())
    platform = entries.get('com.unixnu.driver.ARMPlatformStandIn', {})
    if not platform.get('_PrelinkExecutableLoadAddr'):
        problems.append('native platform stand-in executable absent')
    for bundle in shim['stands_in_for']:
        if bundle in entries:
            problems.append(f'{bundle}: Apple-only hardware implementation was retained')
    # These dependencies were absent before the Bluetooth library was retained.
    # Every personality in each must be explicitly gated, including any unexpected
    # new personality in a future input KC (checking only the patched names misses it).
    for bundle, patches in descriptor['patch_personalities'].items():
        if usb_profile and bundle != 'com.apple.iokit.IOBluetoothFamily':
            continue  # the existing USB-profile checker owns these personalities
        entry = entries.get(bundle)
        if entry is None:
            problems.append(f'{bundle}: absent from collection')
            continue
        personalities = entry.get('IOKitPersonalities', {})
        for name, props in personalities.items():
            if bundle == 'com.apple.iokit.IOBluetoothFamily' and name == 'IOBluetoothHCIController':
                if props.get('IOProviderClass') != 'IOResources' or props.get('IOResourceMatch') != 'PianoBluetoothStack':
                    problems.append('Bluetooth singleton is not gated on PianoBluetoothStack')
            elif props.get('IOProviderClass') != 'PianoBluetoothUnusedHardware':
                problems.append(f'{bundle}/{name}: hardware personality remains active')
        if set(personalities) != set(patches):
            problems.append(f'{bundle}: personality set differs from reviewed dependency closure')
    family = entries.get('com.apple.iokit.IOBluetoothFamily', {})
    if not family.get('_PrelinkExecutableLoadAddr'):
        problems.append('BluetoothFamily executable absent (a fake personality is insufficient)')
    if problems:
        raise SystemExit('\n'.join(problems))
    print('Bluetooth stack: real Family retained, singleton gated, imported hardware personalities inert')


if __name__ == '__main__':
    main()
