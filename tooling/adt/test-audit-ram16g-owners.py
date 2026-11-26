#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Synthetic fixtures only: no captured console, identifiers, paths or secrets."""
import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

SCRIPT = Path(__file__).with_name('audit-ram16g-owners.py')
spec = importlib.util.spec_from_file_location('owners', SCRIPT)
owners = importlib.util.module_from_spec(spec)
spec.loader.exec_module(owners)

# Deliberately coarse synthetic descriptors, independent of any actual EFI map.
DESCRIPTORS = [
    (7, 0xe3800000, 0x10000, 8),
    (4, 0xbda00000, 0x17400, 8),
    (4, 0xa7000000, 0x600, 8),
    (6, 0xa7600000, 0x200, (1 << 63) | 8),
    (4, 0xa7800000, 0x800, 8),
]


def fixture(descriptors=None, overrides=None, gop=(0xfc000000, 0xfc100000)):
    """Known uniform fixture rows; overrides model corrupt or changed map receipts."""
    descriptors = DESCRIPTORS if descriptors is None else descriptors
    lines = [f'EFI_MAP status=complete attempt=1 key=0x1 map_bytes={48 * len(descriptors)} '
             f'stride=48 version=1 descriptors={len(descriptors)} slabs=322']
    for index, (kind, base, pages, attr) in enumerate(descriptors):
        lines.append(f'EFI_DESC index={index} type={kind} base={base:#x} pages={pages:#x} attr={attr:#018x}')
    state = 'observed' if gop[1] > gop[0] else 'unknown'
    lines.append(f'OWNER kind=gop state={state} base={gop[0]:#x} end={gop[1]:#x}')
    lines += ['OWNER kind=secure-world state=unknown', 'OWNER kind=vm state=unknown',
              'OWNER kind=retained-dma state=partial']
    for region, (start, end) in owners.RANGES.items():
        counts = Counter()
        for base in range(start, end, owners.SLAB):
            runtime = int(base == 0xa7600000)
            row = dict(efi='runtime' if runtime else 'non-runtime', runtime_type=runtime,
                       runtime_attr=runtime, gap=0, overlap=0, conventional=int(region == 'CMA'),
                       wb=1, gop_overlap=0)
            row.update((overrides or {}).get(region, {}))
            row.update((overrides or {}).get(base, {}))
            lines.append(f'SLAB range={region} base={base:#x} end={base + owners.SLAB:#x} ' +
                         ' '.join(f'{k}={v}' for k, v in row.items()))
            counts[row['efi']] += 1
            counts['gop'] += row['gop_overlap']
        lines.append(f'SUMMARY range={region} slabs={(end - start) // owners.SLAB} '
                     f'non_runtime={counts["non-runtime"]} runtime={counts["runtime"]} '
                     f'unknown={counts["unknown"]} gop_overlap={counts["gop"]}')
    return '\n'.join(lines)


def changed(index, **changes):
    descriptors = copy.deepcopy(DESCRIPTORS)
    fields = dict(zip(('kind', 'base', 'pages', 'attr'), descriptors[index]))
    fields.update(changes)
    descriptors[index] = tuple(fields.values())
    return descriptors


# Invented addresses, sizes and counters; interleaving and CRLF mimic log transport.
CONSOLE = '\r\n'.join([
    'noise PASSED; no owner receipt',
    'NativeUfs: EFI_MAP_DMA sid=0x60 phase=before-takeover mcq=0 transfer_iova=0x123000 '
    'transfer_slots=16 transfer_run=0x1 transfer_doorbell=0x0 task_iova=0x124000 '
    'task_slots=4 task_run=0x0 task_doorbell=0x0',
    'NativeUfs: EFI_MAP_DMA sid=0x60 phase=legacy-transfer-list-stopped',
    'NativeUfs: DMA virt=0xabcdef000 phys=901000000 contiguous=10000',
    'NativeUfs: SID 0x60 found on bank 5 (sctlr e0 cbar 1f000 cba2r 1); redirecting it',
    'PianoDwc3: device DCFG 00000000 GEVNTADR 0000000012500000 '
    'GEVNTSIZ 80000800 GEVNTCOUNT 00000004',
    'PianoDwc3: SMMU stream 0x40: existing entry slot 9 (smr 80000040 s2cr 00000009; '
    'bank sctlr 000000e0 cbar 0001f000 cba2r 00000001) -> keep bank 9',
    'PianoDwc3: DMA region 0x900000000 (+0x10000), non-cacheable CPU mapping omitted',
    'PianoDpu: SMMU group 7 smr 80020800 (id 0x800 mask 0x2) s2cr 00000007 '
    '(translate, bank 7) cbar 0001f000 (stage 1, stage 2 bypass) cba2r 00000001',
    'noise PianoDpu: pipe VIG0 rect 0: 100x200 @0,0 -> 100x200 @0,0 '
    'addr fc000000 stride 800 format omitted',
    'PianoSensors: arena: 0x9d000000 + 0x400000 (synthetic) mapped write-combined',
    'PianoPower: adsp: smc: PAS mem_setup (DSP, 0xb8000000 + 0x100000): ok (status 0, x0 0)',
    'PianoPower: adsp: smc: PAS mem_setup (DTB, 0x9e000000 + 0x10000): calling',
    'PianoA830: t g7.4 SCM c 42000202 3 d 9b000000',
    'PianoA830: t g7 ZAP end ok 123us',
    'PianoVideo: start: piano-video=6 cc=1 smmu=2 carveout=0 (carveout 0x9c000000)',
    'PianoVideo: V9 allocated arena 1048576 ledger-pages 256 table-pages 4 metadata 4096',
    'PianoWifi: dma: 8 MiB at 0x910000000 (synthetic allocation)',
])


class AuditTest(unittest.TestCase):
    def test_partition_and_no_release_with_complete_map(self):
        report = owners.audit(fixture(), CONSOLE)
        self.assertEqual(len(report['slabs']), 322)
        self.assertEqual(sum(c['slabs'] for c in report['classes']), 322)
        self.assertEqual({r['base'] for r in report['slabs']},
                         {b for c in report['classes'] for b in range(c['base'], c['end'], owners.SLAB)})
        self.assertEqual(report['summary'], {'CMA': {'non-runtime': 128},
                                           'DXE': {'non-runtime': 186},
                                           'FD': {'non-runtime': 7, 'runtime': 1}})
        self.assertEqual(sum(c['receipts']['R-EFI'].get('observed', 0) for c in report['classes']), 321)
        self.assertEqual((report['gate'], report['released_mib']), ('HOLD', 0))
        for row in report['classes']:
            for check in ('R-CPU', 'R-SEC', 'R-VM', 'R-DUMP'):
                self.assertEqual(row['receipts'][check], 'unknown')

    def test_runtime_attribute_without_runtime_type(self):
        desc = changed(1, kind=7, attr=(1 << 63) | 8)
        report = owners.audit(fixture(desc, {'DXE': dict(efi='runtime', runtime_type=0,
                                                       runtime_attr=1, conventional=1)}), '')
        self.assertTrue(all(r['R-EFI'] == 'excluded' for r in report['slabs'] if r['range'] == 'DXE'))

    def test_fixed_fd_excluded_even_without_runtime_flags(self):
        text = fixture(changed(3, kind=4, attr=8), {0xa7600000: dict(
            efi='non-runtime', runtime_type=0, runtime_attr=0)})
        row = next(r for r in owners.audit(text, '')['slabs'] if r['base'] == 0xa7600000)
        self.assertEqual(row['R-EFI'], 'excluded')

    def test_missing_wb_and_nonconventional_cma_excluded(self):
        for desc, changes in [(changed(0, attr=1), dict(wb=0)),
                              (changed(0, kind=4), dict(conventional=0)),
                              (changed(0, kind=0), dict(conventional=0))]:
            report = owners.audit(fixture(desc, {'CMA': changes}), '')
            self.assertTrue(all(r['R-EFI'] == 'excluded' for r in report['slabs'] if r['range'] == 'CMA'))

    def test_gop_overlap_and_missing_gop_are_not_observed(self):
        report = owners.audit(fixture(overrides={0xe3800000: dict(gop_overlap=1)},
                                      gop=(0xe3801000, 0xe3802000)), '')
        self.assertEqual(report['slabs'][0]['R-EFI'], 'excluded')
        report = owners.audit(fixture(gop=(0, 0)), '')
        self.assertEqual(report['slabs'][0]['R-EFI'], 'unknown')

    def test_gap_and_duplicate_extent_remain_unknown(self):
        for desc, flags in [(changed(0, base=0xe3801000, pages=0xffff), dict(gap=1, overlap=0)),
                            (DESCRIPTORS + [DESCRIPTORS[0]], dict(gap=0, overlap=1))]:
            overrides = {'CMA': dict(efi='unknown', conventional=0, wb=0, **flags)} if flags['overlap'] else {
                0xe3800000: dict(efi='unknown', conventional=0, wb=0, **flags)}
            report = owners.audit(fixture(desc, overrides), '')
            self.assertEqual(report['slabs'][0]['R-EFI'], 'unknown')

    def test_duplicate_half_cannot_hide_a_gap(self):
        # Two copies of the first half sum to one slab but leave its upper half absent.
        desc = changed(0, pages=0x100)
        desc += [(7, 0xe3800000, 0x100, 8)]
        overrides = {'CMA': dict(efi='unknown', gap=1, conventional=0, wb=0),
                     0xe3800000: dict(overlap=1)}
        report = owners.audit(fixture(desc, overrides), '')
        self.assertEqual(report['slabs'][0]['R-EFI'], 'unknown')

    def test_runtime_type_without_runtime_attribute(self):
        text = fixture(changed(1, kind=5), {'DXE': dict(efi='runtime', runtime_type=1)})
        report = owners.audit(text, '')
        self.assertEqual(report['summary']['DXE'], {'runtime': 186})
        self.assertTrue(all(r['R-EFI'] == 'excluded' for r in report['slabs'] if r['range'] == 'DXE'))

    def test_half_open_native_overlap_and_override(self):
        for base, size, expected in [(0xe3800000, 0x200000, 1),
                                     (0xe3600000, 0x200000, 0),
                                     (0xf3800000, 0x200000, 0)]:
            log = f'PianoSensors: arena: {base:#x} + {size:#x} mapped write-combined'
            report = owners.audit(fixture(), log)
            self.assertEqual(sum(bool(r['native_overlaps']) for r in report['slabs']), expected)
            self.assertEqual(report['released_mib'], 0)

    def test_partial_receipts_bus_addresses_not_physical(self):
        report = owners.audit(fixture(), CONSOLE)
        by_kind = {o['kind']: o for o in report['observations']}
        queue = by_kind['inherited-queues']
        self.assertEqual((queue['transfer_bytes'], queue['task_bytes']), (512, 320))
        self.assertEqual(queue['physical_translation'], 'unknown')
        event = by_kind['event-buffer']
        self.assertEqual((event['bytes'], event['pending']), (2048, 4))
        dpu = next(o for o in report['observations'] if o['owner'] == 'DPU' and o['kind'] == 'stream-route')
        self.assertEqual(dpu['matching_sids'], [0x800, 0x802])
        self.assertEqual(by_kind['plane']['full_fetch_extent'], 'unknown')
        self.assertFalse(any(o['owner'] == 'ADSP/DTB' for o in report['observations']))
        self.assertEqual(sum(bool(r['native_overlaps']) for r in report['slabs']), 0)
        self.assertTrue(all(c['receipts']['R-DMA'] == 'partial' for c in report['classes']))
        self.assertTrue(all(c['scoped_physical_exclusions'] for c in report['classes']))
        self.assertEqual(next(o for o in report['observations'] if o['kind'] == 'inherited-route')['bank'], 5)
        self.assertEqual(next(o for o in report['observations'] if o['owner'] == 'WLAN')['end'], 0x910800000)

    def test_missing_or_irrelevant_console_does_not_close_any_owner_check(self):
        for log in ('', 'PASSED; Gunyah watchdog armed; OWNER secure-world safe'):
            report = owners.audit(fixture(), log)
            self.assertEqual(report['observations'], [])
            self.assertTrue(all(c['receipts']['R-DMA'] == 'unknown' for c in report['classes']))

    def test_supplemental_console_is_optional_and_scoped(self):
        report = owners.audit(fixture(), '', CONSOLE, 'synthetic-qual1')
        self.assertTrue(all(o['source'] == 'console' for o in report['observations']))
        self.assertIn('| CMA/CNSS | 16 | 16/0/0 | partial | partial | unknown | unknown | unknown |',
                      owners.matrix(report))

    def test_truncated_corrupt_duplicate_or_inconsistent_map_rejected(self):
        text = fixture()
        slab = next(line for line in text.splitlines() if line.startswith('SLAB '))
        desc = next(line for line in text.splitlines() if line.startswith('EFI_DESC '))
        mutations = [text.rsplit('\n', 1)[0], text.replace(slab + '\n', ''),
                     text + '\n' + slab, text + '\n' + desc,
                     text.replace('status=complete', 'status=capacity'),
                     text.replace('version=1', 'version=2'), text.replace('pages=0x10000', 'pages=0x0'),
                     text.replace('wb=1', 'wb=0', 1), text.replace('non_runtime=128', 'non_runtime=127'),
                     text.replace('index=1', 'index=0'), text.replace('slabs=322', 'slabs=321', 1),
                     text.replace('base=0xe3800000', 'base=0xe3800001', 1),
                     text.replace('attempt=1', 'attempt=4'), text.replace('stride=48', 'stride=39')]
        for mutation in mutations:
            with self.subTest(mutation=mutations.index(mutation)), self.assertRaises(ValueError):
                owners.audit(mutation, CONSOLE)

    def test_cli_json_and_error_redaction(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_path, console_path = root / 'map.txt', root / 'owners.txt'
            map_path.write_text(fixture())
            console_path.write_text(CONSOLE)
            command = [sys.executable, str(SCRIPT), '--efi-map', str(map_path),
                       '--owners-console', str(console_path), '--json']
            completed = subprocess.run(command, capture_output=True, text=True, check=True)
            report = json.loads(completed.stdout)
            self.assertEqual(report['released_mib'], 0)
            self.assertNotIn(directory, completed.stdout)
            map_path.unlink()
            completed = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 2)
            self.assertNotIn(directory, completed.stderr)
            self.assertEqual(completed.stdout, '')


if __name__ == '__main__':
    unittest.main()
