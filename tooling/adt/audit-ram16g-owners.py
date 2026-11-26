#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Offline receipts for the 322 RAM16G slabs. Never authorizes memory release.

Inputs are read-efi-map-audit.py's complete text and retained owner console text.
Only recognized numeric facts are emitted; source paths and raw log lines are not.
There is no tablet, MMIO, SCM, RM, subprocess or network interface in this tool.
"""
import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

PAGE = 4096
SLAB = 2 << 20
RUNTIME = 1 << 63
RANGES = {'CMA': (0xe3800000, 0xf3800000),
          'DXE': (0xbda00000, 0xd4e00000), 'FD': (0xa7000000, 0xa8000000)}
# Partition the documentation's combined tail/workspace rows for exact coverage.
CLASSES = (
    ('CMA/CNSS', 'CMA', 0xe3800000, 0xe5800000),
    ('CMA/QMC', 'CMA', 0xe5800000, 0xe6800000),
    ('CMA/VA-minidump', 'CMA', 0xe6800000, 0xe7800000),
    ('CMA/ADSP-heap', 'CMA', 0xe7800000, 0xe8400000),
    ('CMA/generic', 'CMA', 0xe8400000, 0xea800000),
    ('CMA/VM-comm', 'CMA', 0xea800000, 0xeb000000),
    ('CMA/QSEE-TA', 'CMA', 0xeb000000, 0xee800000),
    ('CMA/QSEE-client', 'CMA', 0xee800000, 0xf2800000),
    ('CMA/SP', 'CMA', 0xf2800000, 0xf3800000),
    ('DXE/lower-tail', 'DXE', 0xbda00000, 0xc0800000),
    ('DXE/secure-CDSP', 'DXE', 0xc0800000, 0xc5000000),
    ('DXE/display', 'DXE', 0xc5000000, 0xd4c00000),
    ('DXE/upper-tail', 'DXE', 0xd4c00000, 0xd4e00000),
    ('FD/image', 'FD', 0xa7000000, 0xa7400000),
    ('FD/reserved', 'FD', 0xa7400000, 0xa7600000),
    ('FD/mixed-runtime', 'FD', 0xa7600000, 0xa7800000),
    ('FD/workspace', 'FD', 0xa7800000, 0xa8000000),
)


def intersects(a, b, c, d):
    return a < d and c < b


def fields(line, expected):
    tokens = line.split()[1:]
    pairs = [token.split('=', 1) for token in tokens]
    if any(len(pair) != 2 for pair in pairs):
        raise ValueError('invalid map fields')
    result = dict(pairs)
    if len(result) != len(pairs) or set(result) != set(expected.split()):
        raise ValueError('missing, duplicate or unsupported map fields')
    return result


def number(value):
    if not re.fullmatch(r'(0x[0-9a-fA-F]+|[0-9]+)', value):
        raise ValueError('invalid map integer')
    return int(value, 16 if value.startswith('0x') else 10)


def classify(base, descriptors, gop):
    end = base + SLAB
    spans = sorted(((max(base, d['base']), min(end, d['end']), d)
                    for d in descriptors if intersects(base, end, d['base'], d['end'])),
                   key=lambda span: (span[0], span[1], span[2]['index']))
    overlap = any(a[1] > b[0] for a, b in zip(spans, spans[1:]))
    # Firmware counts unique covered pages, so an overlap cannot hide a gap.
    covered, cursor = 0, base
    for a, b, _ in spans:
        covered += max(0, b - max(a, cursor))
        cursor = max(cursor, b)
    gap = covered != SLAB
    rt_type = any(d['type'] in (5, 6) for _, _, d in spans)
    rt_attr = any(d['attr'] & RUNTIME for _, _, d in spans)
    full = not (gap or overlap)
    flags = dict(runtime_type=int(rt_type), runtime_attr=int(rt_attr), gap=int(gap),
                 overlap=int(overlap), conventional=int(full and all(d['type'] == 7 for _, _, d in spans)),
                 wb=int(full and all(d['attr'] & 8 for _, _, d in spans)),
                 gop_overlap=int(bool(gop and intersects(base, end, *gop))))
    flags['efi'] = 'runtime' if rt_type or rt_attr else 'non-runtime' if full else 'unknown'
    return flags, [dict(index=d['index'], base=a, end=b, type=d['type'], attr=d['attr'])
                   for a, b, d in spans]


def parse_map(text):
    headers, descriptors, rows, summaries, owners = [], [], {}, {}, {}
    slab_fields = 'range base end efi runtime_type runtime_attr gap overlap conventional wb gop_overlap'
    for line in text.splitlines():
        if not line.strip():
            continue
        if line.startswith('EFI_MAP '):
            headers.append(fields(line, 'status attempt key map_bytes stride version descriptors slabs'))
        elif line.startswith('EFI_DESC '):
            descriptors.append({k: number(v) for k, v in fields(line, 'index type base pages attr').items()})
        elif line.startswith('SLAB '):
            row = fields(line, slab_fields)
            row = {k: v if k in ('range', 'efi') else number(v) for k, v in row.items()}
            if row['base'] in rows:
                raise ValueError('duplicate slab')
            rows[row['base']] = row
        elif line.startswith('SUMMARY '):
            summary = fields(line, 'range slabs non_runtime runtime unknown gop_overlap')
            name = summary.pop('range')
            if name in summaries:
                raise ValueError('duplicate summary')
            summaries[name] = {k: number(v) for k, v in summary.items()}
        elif line.startswith('OWNER '):
            if line.startswith('OWNER kind=gop '):
                owner = fields(line, 'kind state base end')
                owner['base'], owner['end'] = number(owner['base']), number(owner['end'])
                if not 0 <= owner['base'] <= owner['end'] < 1 << 64:
                    raise ValueError('invalid GOP extent')
                if owner['state'] not in ('observed', 'unknown') or (
                        (owner['state'] == 'observed') != (owner['end'] > owner['base'])):
                    raise ValueError('inconsistent GOP state')
            else:
                owner = fields(line, 'kind state')
            if owner['kind'] in owners:
                raise ValueError('duplicate owner metadata')
            owners[owner['kind']] = owner
        else:
            raise ValueError('unsupported map record')
    if len(headers) != 1:
        raise ValueError('expected one complete map header')
    header = headers[0]
    if header.pop('status') != 'complete':
        raise ValueError('incomplete map')
    header = {k: number(v) for k, v in header.items()}
    if not (header['version'] == 1 and 1 <= header['attempt'] <= 3 and
            header['stride'] >= 40 and header['descriptors'] > 0 and
            header['map_bytes'] == header['stride'] * header['descriptors'] and
            header['slabs'] == 322 and header['descriptors'] == len(descriptors) and
            64 + len(descriptors) * 28 + 322 <= 32768):
        raise ValueError('invalid or truncated map metadata')
    if {d['index'] for d in descriptors} != set(range(len(descriptors))):
        raise ValueError('invalid descriptor indices')
    for d in descriptors:
        d['end'] = d['base'] + d['pages'] * PAGE
        if not (d['pages'] and d['base'] % PAGE == 0 and d['end'] < 1 << 64 and
                d['attr'] < 1 << 64 and d['type'] < 1 << 32):
            raise ValueError('invalid descriptor extent, type or attributes')
    expected = {b: name for name, (start, end) in RANGES.items() for b in range(start, end, SLAB)}
    if set(rows) != set(expected) or set(summaries) != set(RANGES):
        raise ValueError('missing or unexpected slabs/summaries')
    gop_owner = owners.get('gop', {})
    gop = (gop_owner['base'], gop_owner['end']) if gop_owner.get('state') == 'observed' else None
    for base, name in expected.items():
        row = rows[base]
        flags, segments = classify(base, descriptors, gop)
        if (row['range'] != name or row['end'] != base + SLAB or
                any(row[k] != v for k, v in flags.items())):
            raise ValueError('slab flags disagree with descriptors/GOP')
        row['segments'] = segments
        # Non-runtime is an observation of allocation type, never an owner proof.
        bad_type = any(s['type'] not in (1, 2, 3, 4, 7) for s in segments)
        if flags['efi'] == 'runtime' or flags['gop_overlap'] or base == 0xa7600000 or bad_type:
            row['R-EFI'] = 'excluded'
        elif flags['efi'] == 'unknown' or gop is None:
            row['R-EFI'] = 'unknown'
        elif not flags['wb'] or (name == 'CMA' and not flags['conventional']):
            row['R-EFI'] = 'excluded'
        else:
            row['R-EFI'] = 'observed'
    for name in RANGES:
        group = [r for r in rows.values() if r['range'] == name]
        counts = Counter(r['efi'] for r in group)
        actual = dict(slabs=len(group), non_runtime=counts['non-runtime'], runtime=counts['runtime'],
                      unknown=counts['unknown'], gop_overlap=sum(r['gop_overlap'] for r in group))
        if summaries[name] != actual:
            raise ValueError('summary disagrees with descriptors/slabs')
    return header, descriptors, rows


def parse_console(text, source):
    """Whitelist observations. IOVAs stay IOVAs; stages/calls never prove retirement."""
    observations = []

    def add(line, owner, kind, **facts):
        observations.append(dict(owner=owner, kind=kind, source=source, line=line, **facts))

    for line_number, line in enumerate(text.splitlines(), 1):
        m = re.search(r'NativeUfs: EFI_MAP_DMA sid=0x60 phase=before-takeover '
                      r'mcq=([01]) transfer_iova=(0x[0-9a-f]+) transfer_slots=(\d+) '
                      r'transfer_run=(0x[0-9a-f]+) transfer_doorbell=(0x[0-9a-f]+) '
                      r'task_iova=(0x[0-9a-f]+) task_slots=(\d+) '
                      r'task_run=(0x[0-9a-f]+) task_doorbell=(0x[0-9a-f]+)', line)
        if m:
            values = [int(v, 0) for v in m.groups()]
            mcq, transfer, slots, run, doorbell, task, tasks, task_run, task_doorbell = values
            if 1 <= slots <= 32 and 1 <= tasks <= 8:
                add(line_number, 'UFS', 'inherited-queues', sid=0x60, mcq=mcq,
                    transfer_iova=transfer, transfer_bytes=slots * 32, transfer_run=run,
                    transfer_doorbell=doorbell, task_iova=task, task_bytes=tasks * 80,
                    task_run=task_run, task_doorbell=task_doorbell, physical_translation='unknown')
        if 'NativeUfs: EFI_MAP_DMA sid=0x60 phase=legacy-transfer-list-stopped' in line:
            add(line_number, 'UFS', 'transfer-list-stopped', sid=0x60)
        m = re.search(r'PianoDwc3:.*GEVNTADR ([0-9a-f]{16}).*GEVNTSIZ ([0-9a-f]{8})'
                      r'.*GEVNTCOUNT ([0-9a-f]{8})', line)
        if m:
            address, size, count = (int(v, 16) for v in m.groups())
            add(line_number, 'USB', 'event-buffer', sid=0x40, iova=address,
                bytes=size & 0xffff, size_register=size, pending=count, physical_translation='unknown')
        m = re.search(r'PianoDwc3: SMMU stream 0x40:.*existing entry slot (\d+) '
                      r'\(smr ([0-9a-f]+) s2cr ([0-9a-f]+); bank sctlr ([0-9a-f]+) '
                      r'cbar ([0-9a-f]+) cba2r ([0-9a-f]+)\)', line)
        if m:
            slot, *words = m.groups()
            add(line_number, 'USB', 'stream-route', sid=0x40, slot=int(slot),
                **dict(zip(('smr', 's2cr', 'sctlr', 'cbar', 'cba2r'), (int(w, 16) for w in words))))
        m = re.search(r'PianoDwc3: DMA region (0x[0-9a-f]+) \(\+(0x[0-9a-f]+)\)', line)
        if m:
            base, size = (int(v, 16) for v in m.groups())
            add(line_number, 'USB', 'physical-buffer', base=base, end=base + size)
        m = re.search(r'PianoDpu: SMMU group (\d+) smr ([0-9a-f]+).*s2cr ([0-9a-f]+) '
                      r'.*cbar ([0-9a-f]+).*cba2r ([0-9a-f]+)', line)
        if m:
            group, *words = m.groups()
            smr, s2cr, cbar, cba2r = (int(w, 16) for w in words)
            matches = [sid for sid in (0x800, 0x801, 0x802, 0x803)
                       if smr & (1 << 31) and not ((sid ^ (smr & 0x7fff)) & ~(smr >> 16 & 0x7fff))]
            add(line_number, 'DPU', 'stream-route', group=int(group), smr=smr, s2cr=s2cr,
                cbar=cbar, cba2r=cba2r, matching_sids=matches)
        m = re.search(r'PianoDpu: pipe (\w+) rect (\d+): (\d+)x(\d+) @(\d+),(\d+) '
                      r'.*addr ([0-9a-f]+) stride (\d+)', line)
        if m:
            pipe, rect, width, height, x, y, address, stride = m.groups()
            add(line_number, 'DPU', 'plane', pipe=pipe, rect=int(rect), width=int(width),
                height=int(height), x=int(x), y=int(y), iova=int(address, 16), stride=int(stride),
                full_fetch_extent='unknown')
        m = re.search(r'PianoSensors: arena: (0x[0-9a-f]+) \+ (0x[0-9a-f]+).*mapped', line)
        if m:
            base, size = (int(v, 16) for v in m.groups())
            add(line_number, 'Sensors', 'physical-buffer', base=base, end=base + size)
        m = re.search(r'PianoPower: adsp: smc: PAS mem_setup \((DSP|DTB), '
                      r'(0x[0-9a-f]+) \+ (0x[0-9a-f]+)\): ok \(status 0,', line)
        if m:
            image, base, size = m.groups()
            base, size = int(base, 16), int(size, 16)
            add(line_number, 'ADSP/' + image, 'physical-buffer', base=base, end=base + size)
        m = re.search(r'PianoA830: t g7\.4 SCM c 42000202 3 d ([0-9a-f]+)', line)
        if m:
            add(line_number, 'GPU/ZAP', 'setup-call', base=int(m[1], 16), extent='unknown')
        if re.search(r'PianoA830: (?:t g7 ZAP end ok|row3 ZAP x0 0 x1 0)', line):
            add(line_number, 'GPU/ZAP', 'success-status')
        m = re.search(r'PianoVideo: start: piano-video=(\d+).*carveout=([01]).*'
                      r'carveout (0x[0-9a-f]+)', line)
        if m:
            add(line_number, 'Video', 'configuration', level=int(m[1]), carveout_enabled=int(m[2]),
                carveout_base=int(m[3], 16), arena_physical_extents='unknown')
        m = re.search(r'PianoVideo: V9 allocated arena (\d+).*table-pages (\d+)', line)
        if m:
            add(line_number, 'Video', 'arena-size', bytes=int(m[1]), table_pages=int(m[2]),
                physical_extents='unknown')
    return observations


def audit(map_text, owner_text, console_text='', run_label='unspecified'):
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', run_label):
        raise ValueError('invalid run label')
    header, descriptors, slabs = parse_map(map_text)
    observations = parse_console(owner_text, 'owners') + parse_console(console_text, 'console')
    for row in slabs.values():
        row['native_overlaps'] = [i for i, o in enumerate(observations)
                                  if o['kind'] == 'physical-buffer' and
                                  intersects(row['base'], row['end'], o['base'], o['end'])]
    classes = []
    for name, region, start, end in CLASSES:
        group = [slabs[b] for b in range(start, end, SLAB)]
        efi = dict(Counter(r['R-EFI'] for r in group))
        dpu = any(o['owner'] == 'DPU' for o in observations)
        classes.append(dict(name=name, range=region, base=start, end=end, slabs=len(group),
                            receipts={'R-EFI': efi,
                                      'R-DMA': 'observed-owner' if any(r['native_overlaps'] for r in group)
                                      else 'partial' if observations else 'unknown',
                                      'R-DPU': 'partial' if dpu else 'unknown',
                                      'R-CPU': 'unknown', 'R-SEC': 'unknown',
                                      'R-VM': 'unknown', 'R-DUMP': 'unknown'},
                            descriptor_indices=sorted({s['index'] for r in group for s in r['segments']}),
                            native_overlap_slabs=sum(bool(r['native_overlaps']) for r in group)))
    return dict(schema=1, run_label=run_label, provenance='operator must verify same-boot input pairing',
                gate='HOLD', released_mib=0, map=header, descriptors=descriptors,
                summary={region: dict(Counter(r['efi'] for r in slabs.values() if r['range'] == region))
                         for region in RANGES},
                classes=classes, observations=observations, slabs=list(slabs.values()))


def matrix(report):
    lines = [f"RAM16G {report['run_label']}: HOLD; released=0 MiB; 322 slabs",
             'Observed means map fact only. Partial/unknown receipts cannot admit memory.',
             'R-DMA partial means scoped console evidence only; R-SEC/R-VM/R-CPU/R-DUMP remain unknown.',
             '| Class | Slabs | R-EFI observed/excluded/unknown | R-DMA | R-DPU | R-CPU | R-SEC | R-VM |',
             '|---|---:|---|---|---|---|---|---|']
    for row in report['classes']:
        r = row['receipts']
        efi = '/'.join(str(r['R-EFI'].get(s, 0)) for s in ('observed', 'excluded', 'unknown'))
        lines.append(f"| {row['name']} | {row['slabs']} | {efi} | {r['R-DMA']} | {r['R-DPU']} | "
                     f"{r['R-CPU']} | {r['R-SEC']} | {r['R-VM']} |")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--efi-map', required=True, type=Path)
    parser.add_argument('--owners-console', required=True, type=Path)
    parser.add_argument('--console', type=Path, help='optional fuller retained console, same boot')
    parser.add_argument('--run-label', default='unspecified')
    parser.add_argument('--json', action='store_true', help='include descriptors, per-slab joins and numeric observations')
    args = parser.parse_args()
    try:
        report = audit(args.efi_map.read_text(), args.owners_console.read_text(),
                       args.console.read_text() if args.console else '', args.run_label)
    except (OSError, UnicodeError, ValueError):
        # Never echo a private input path or unrecognized raw input on failure.
        print('RAM16G audit: invalid/unreadable input; HOLD; released=0 MiB', file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2) if args.json else matrix(report))
    return 0


if __name__ == '__main__':
    sys.exit(main())
