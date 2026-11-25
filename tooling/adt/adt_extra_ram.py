#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""RAM16G R1: add the /chosen piano-extra-ram placeholder (136 zero bytes) to a flattened ADT. DarwinBoot with
firmware/patches/darwinboot/0002 finds it, reserves DRAM region1's runs below XNU's window and fills it (u32 version 1, u32 count,
8 x (u64 base, u64 size)); PianoA830 reads it when piano-gpu-tier=1. Without the placeholder DarwinBoot reserves nothing.

    adt_extra_ram.py IN OUT      (also make-piano-adt.py --extra-ram)
"""
import struct
import sys

NAME = 'piano-extra-ram'
BYTES = 8 + 16 * 8


def _node_end(t, off):
    """Offset after the node at off; also its (props, children, first child offset, name)."""
    nprops, nchildren = struct.unpack_from('<II', t, off)
    o = off + 8
    name = None
    for _ in range(nprops):
        pname = t[o:o + 32].split(b'\0', 1)[0].decode()
        (length,) = struct.unpack_from('<I', t, o + 32)
        length &= 0x7fffffff
        if pname == 'name':
            name = t[o + 36:o + 36 + length].split(b'\0', 1)[0].decode()
        o += 36 + ((length + 3) & ~3)
    props_end = o
    for _ in range(nchildren):
        o = _node_end(t, o)[0]
    return o, nprops, nchildren, props_end, name


def find_chosen(t):
    """(node offset, props end) of /chosen (a child of the root)."""
    _, _, nchildren, o, _ = _node_end(t, 0)
    for _ in range(nchildren):
        end, _, _, props_end, name = _node_end(t, o)
        if name == 'chosen':
            return o, props_end
        o = end
    raise ValueError('no /chosen node')


def has_extra_ram(t):
    off, props_end = find_chosen(t)
    nprops = struct.unpack_from('<I', t, off)[0]
    o = off + 8
    for _ in range(nprops):
        if t[o:o + 32].split(b'\0', 1)[0].decode() == NAME:
            return True
        o += 36 + (((struct.unpack_from('<I', t, o + 32)[0] & 0x7fffffff) + 3) & ~3)
    return False


def add_extra_ram(tree):
    t = bytearray(tree)
    if has_extra_ram(t):
        raise ValueError(f'/chosen already has {NAME}')
    off, props_end = find_chosen(t)
    prop = NAME.encode().ljust(32, b'\0') + struct.pack('<I', BYTES) + bytes(BYTES)
    t[props_end:props_end] = prop
    struct.pack_into('<I', t, off, struct.unpack_from('<I', t, off)[0] + 1)
    assert _node_end(t, 0)[0] == len(t)
    return bytes(t)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    open(sys.argv[2], 'wb').write(add_extra_ram(data))
    print(f'{sys.argv[2]}: /chosen {NAME} placeholder ({BYTES} bytes) added')
