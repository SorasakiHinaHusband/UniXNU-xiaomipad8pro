#!/usr/bin/env python3
"""Is a committed IOMobileFramebufferABI.h usable for a kernel collection that has no AppleParavirtDisplay?

    check-iomfb-abi-header.py --header REF.h --kc KC --tree K27_TREE --out OUT.h

The declarations' slot numbers and arm64e PAC diversities ARE the ABI: a slot's diversity is the string discriminator of the
introducing declaration's mangled name (scripts/ptrauth_disc.py). So the header is valid for a collection when, slot by slot,
every diversity it declares is the diversity that collection's IOMobileFramebufferLegacy vtable holds there, and the vtable has
the slot count the header declares. The instance size is NOT part of that: the header's own is replaced with the collection's
(measured from the class's OSMetaClass constructor, as scripts/gen-iomfb-abi.py does).

Exit 0 with OUT.h written when every check holds; exit 1 with the first differences printed otherwise (never a silent pass).
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--header', type=Path, required=True)
parser.add_argument('--kc', type=Path, required=True)
parser.add_argument('--tree', type=Path, required=True, help='the k27 tree (its scripts/kc-vtable.py and scripts/fileset_kc.py)')
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()

KEXT = 'com.apple.iokit.IOMobileGraphicsFamily'
CLS = 'IOMobileFramebufferLegacy'

def fail(message):
    print(f'check-iomfb-abi-header: {message}', file=sys.stderr)
    return 1

text = args.header.read_text()

# The header: the slot count it declares, its (class, slot, diversity) comments and the instance size it declares.
m = re.search(r'IOMobileFramebufferLegacySlots\s*=\s*(\d+)', text)
if not m:
    sys.exit(fail(f'{args.header}: no IOMobileFramebufferLegacySlots'))
declared_slots = int(m.group(1))
m = re.search(r'sizeof\(IOMobileFramebufferLegacy\)\s*==\s*(0x[0-9a-fA-F]+)', text)
if not m:
    sys.exit(fail(f'{args.header}: no static_assert on the instance size'))
header_size = m.group(1)

pairs, owners, owner = [], [], None
for line in text.splitlines():
    c = re.match(r'^class\s+(\w+)', line)
    if c:
        owner = c.group(1)
        continue
    d = re.search(r'//\s*(\d+)\s+(0x[0-9a-fA-F]+)\s*$', line)
    if d and owner:
        pairs.append((int(d.group(1)), int(d.group(2), 16)))
        owners.append(owner)
if not pairs:
    sys.exit(fail(f'{args.header}: no "<slot> <diversity>" comments to check'))

# The collection's vtable for the class (the tree's own reader, so both sides come from the same code).
out = subprocess.run(['python3', str(args.tree / 'scripts/kc-vtable.py'), str(args.kc), CLS],
                     capture_output=True, text=True)
if out.returncode != 0:
    sys.exit(fail(f'kc-vtable.py {args.kc} {CLS} failed:\n{out.stderr[-2000:]}'))
vtable = []
for line in out.stdout.splitlines()[1:]:
    f = line.split()
    if len(f) < 2:
        continue
    try:
        vtable.append((int(f[0]), int(f[1], 16)))
    except ValueError:
        continue
if not vtable:
    sys.exit(fail('kc-vtable.py printed no slots'))

diffs = [(slot, disc, vtable[slot][1]) for slot, disc in pairs if slot >= len(vtable) or disc != vtable[slot][1]]
if declared_slots != len(vtable):
    sys.exit(fail(f'{args.header} declares {declared_slots} slots, {args.kc} has {len(vtable)}'))
if diffs:
    lines = [f'slot {s}: header {d:#06x}, the collection {t:#06x}' for s, d, t in diffs[:12]]
    sys.exit(fail(f'{len(diffs)} of {len(pairs)} declared slots differ from {args.kc}:\n  ' + '\n  '.join(lines)))

# The collection's instance size: the movz w3, #imm its OSMetaClass constructor passes (scripts/gen-iomfb-abi.py's rule).
sys.path.insert(0, str(args.tree / 'scripts'))
import fileset_kc  # noqa: E402

kc = fileset_kc.Kernelcache(args.kc.read_bytes())
symbol = f'__ZN{len(CLS)}{CLS}9MetaClassC2Ev'
va = next((v for n, k, s, d, v in kc.symbols(kc.macho(KEXT)) if n == symbol), None)
if va is None:
    sys.exit(fail(f'{symbol} is not in {KEXT} of {args.kc}'))
off = kc.file_offset(va)
size = None
for i in range(32):
    word = int.from_bytes(kc.data[off + 4 * i: off + 4 * i + 4], 'little')
    if word & 0xffe0001f == 0x52800003:        # movz w3, #imm16
        size = (word >> 5) & 0xffff
        break
if not size:
    sys.exit(fail(f'{symbol}: no instance size'))

# Same declarations, this collection's instance size (the only thing the ABI check leaves open).
adapted = text
for pattern in (r'iomfbLegacyState\[[^\]]*\]', r'sizeof\(IOMobileFramebufferLegacy\)\s*==\s*0x[0-9a-fA-F]+'):
    adapted = re.sub(pattern, lambda mo: f'iomfbLegacyState[{size:#x} - sizeof(IOService)]'
                     if pattern.startswith('iomfbLegacyState') else f'sizeof(IOMobileFramebufferLegacy) == {size:#x}', adapted)
if adapted == text and size != int(header_size, 16):
    sys.exit(fail('the instance size is declared in a form this tool does not rewrite'))
args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_text(adapted)
print(f'check-iomfb-abi-header: {len(vtable)} slots, {len(pairs)} declared, all diversities match; '
      f'instance size {header_size} -> {size:#x} ({args.header.name} -> {args.out})')
