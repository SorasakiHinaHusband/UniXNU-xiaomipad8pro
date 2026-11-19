#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026, Leo Zhang.
"""Compile the actual 0006 policy/import/retry code against a fault-injected host runtime.

No GPU, seed staging, tablet, global notify post, or private source tree is used.
The Objective-C++ harness tests Foundation weak/strong ownership and actual
surfaceBuffer publication; it does not model GPU fences or the WS capture ABI.
"""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, default=root / 'out/frontend-pressure-host')
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
patch = Path(__file__).resolve().parents[1] / 'frontend/0006-frontend-bounded-import-reclaim.patch'
# Reconstruct the new side of the diff. The resource section is one complete hunk;
# this deliberately fails if a future patch no longer includes the entire section.
new = []
in_hunk = False
for line in patch.read_text().splitlines(keepends=True):
    if line.startswith('@@ '):
        in_hunk = True
    elif in_hunk and line[:1] in ('+', ' '):
        new.append(line[1:])
new = ''.join(new)
start = new.index('static NSMapTable<NSValue *,NBBuffer *> *gSurfaceImports;')
end = new.index('// PVRTC (PowerVR', start)
code = new[start:end]
assert 'static NBBuffer *surfaceBuffer(' in code and 'gone.clear();candidate=nil;return buffer;' in code
accounting = new[new.index('static uint64_t runtimeAllocatedBytes()'):].split('\n', 1)[0]
(args.out / 'frontend_pressure_actual.inc').write_text(accounting + '\n' + code)
method_start = new.index('- (id<MTLBuffer>)newBufferWithBytesNoCopy:')
method_end = new.index('\n}', method_start) + 2
current_method = next(line for line in new.splitlines() if line.startswith('- (NSUInteger)currentAllocatedSize {uint64_t mc[10]'))
(args.out / 'frontend_pressure_nocopy.inc').write_text(new[method_start:method_end] + '\n' + current_method)
binary = args.out / 'frontend_pressure_host'
subprocess.run(['xcrun', 'clang++', '-std=c++17', '-fobjc-arc', '-fblocks', '-g', '-O1',
                '-fsanitize=address,undefined', '-Wno-deprecated-declarations',
                '-I', str(args.out), str(Path(__file__).with_suffix('.mm')),
                '-framework', 'Foundation', '-framework', 'IOSurface', '-o', str(binary)], check=True)
subprocess.run([str(binary)], timeout=30, check=True)
