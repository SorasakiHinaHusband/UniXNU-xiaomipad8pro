#!/usr/bin/env python3
"""Check the /chosen syscfg-v2-data table of an ADT file the way macOS 27.3's libMobileGestalt reads it (docs/identity.md section 9).

  check-syscfg.py ADT          PASS when /chosen has syscfg-v2-data and MobileGestalt's sysconfig parser accepts it
  check-syscfg.py --self-test  the parser rules on built-in good and bad tables

Without the property, every process's first syscfg-backed MobileGestalt answer (AppKit's hardware accent color in every app's
NSApplication init) waits 5 s for an AppleDiagnosticDataAccessReadOnly service that piano never has. The rules, as MobileGestalt
applies them: at least the 24-byte header; the magic 'SCfg' as a little-endian u32 at offset 0; the entry count at offset 0x14;
24 + 20 * count bytes; an entry is a 4-byte key and 16 bytes of data, or 'CNTB' with key, size and offset of data that must lie
inside the table. Exit 0 = PASS, 1 = FAIL (the reason is printed).
"""
import struct
import sys

MAGIC = 0x53436667   # 'SCfg'
CNTB = 0x434E5442    # 'CNTB'
HEADER, ENTRY = 0x18, 0x14


def parse_adt(data, offset=0):
    """(properties, children) of the node at offset, and the offset after it (the flattened ADT format)."""
    nprops, nchildren = struct.unpack_from('<II', data, offset)
    offset += 8
    props = {}
    for _ in range(nprops):
        name = data[offset:offset + 32].split(b'\0', 1)[0].decode(errors='replace')
        size = struct.unpack_from('<I', data, offset + 32)[0] & 0x7fffffff
        props[name] = data[offset + 36:offset + 36 + size]
        offset += 36 + ((size + 3) & ~3)
    children = []
    for _ in range(nchildren):
        child, offset = parse_adt(data, offset)
        children.append(child)
    return (props, children), offset


def key_text(value):
    """A syscfg key as MobileGestalt names it (the u32 byte-swapped: stored 'SHLC' reads as CLHS)."""
    return struct.pack('>I', value).decode('latin-1')


def check_table(blob):
    """The entry keys of a table MobileGestalt accepts; raises ValueError with the reason otherwise."""
    if len(blob) < HEADER:
        raise ValueError(f'{len(blob)} bytes, less than the {HEADER}-byte header')
    magic, = struct.unpack_from('<I', blob, 0)
    if magic != MAGIC:
        raise ValueError(f'magic 0x{magic:08x}, not 0x{MAGIC:08x} (MobileGestalt: "syscfg is not initialized!")')
    count, = struct.unpack_from('<I', blob, 0x14)
    if len(blob) < HEADER + ENTRY * count:
        raise ValueError(f'{len(blob)} bytes cannot hold {count} entries ({HEADER + ENTRY * count} needed)')
    keys = []
    for i in range(count):
        at = HEADER + ENTRY * i
        key, = struct.unpack_from('<I', blob, at)
        if key == CNTB:
            real, size, start = struct.unpack_from('<III', blob, at + 4)
            if start > len(blob) or start + size > len(blob):
                raise ValueError(f'entry {i} ({key_text(real)}): data at {start}+{size} outside the {len(blob)}-byte table')
            key = real
        keys.append(key_text(key))
    return keys


def self_test():
    empty = struct.pack('<6I', MAGIC, HEADER, HEADER, 0, 0, 0)
    one = struct.pack('<6I', MAGIC, HEADER + ENTRY, HEADER + ENTRY, 0, 0, 1) + struct.pack('<I', 0x434C4853) + bytes(16)
    big = struct.pack('<6I', MAGIC, 0, 0, 0, 0, 1) + struct.pack('<5I', CNTB, 0x52656720, 4, HEADER + ENTRY, 0) + b'abcd'
    good = {'empty': (empty, []), 'one inline entry': (one, ['CLHS']), 'one CNTB entry': (big, ['Reg '])}
    bad = {'short': empty[:20], 'zeroed': bytes(24), 'count too big': empty[:0x14] + struct.pack('<I', 1),
           'CNTB out of range': big[:-1]}
    failures = []
    for name, (blob, want) in good.items():
        try:
            got = check_table(blob)
            if got != want:
                failures.append(f'{name}: keys {got}, wanted {want}')
        except ValueError as e:
            failures.append(f'{name}: refused ({e})')
    for name, blob in bad.items():
        try:
            check_table(blob)
            failures.append(f'{name}: accepted')
        except ValueError:
            pass
    print('SELF-TEST ' + ('FAIL: ' + '; '.join(failures) if failures else f'PASS ({len(good)} good, {len(bad)} bad tables)'))
    return 1 if failures else 0


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    if len(sys.argv) != 2:
        print(__doc__.strip().split('\n\n')[1])
        return 2
    (props, children), _ = parse_adt(open(sys.argv[1], 'rb').read())
    chosen = [c for c in children if c[0].get('name', b'').rstrip(b'\0') == b'chosen']
    if len(chosen) != 1:
        print(f'FAIL: {len(chosen)} /chosen nodes')
        return 1
    blob = chosen[0][0].get('syscfg-v2-data')
    if blob is None:
        print('FAIL: no /chosen syscfg-v2-data: every process waits 5 s in MobileGestalt (docs/identity.md section 9)')
        return 1
    try:
        keys = check_table(blob)
    except ValueError as e:
        print(f'FAIL: /chosen syscfg-v2-data: {e}')
        return 1
    print(f'PASS: /chosen syscfg-v2-data, {len(blob)} bytes, {len(keys)} entries{" " + " ".join(keys) if keys else ""}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
