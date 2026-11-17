#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang. All rights reserved.
"""Build the locked macOS Bluetooth runtime outside every Git checkout.

This builds files only. It does not mount disks, run bluetoothd, access a
controller, alter the image recipe, or install anything on the host/tablet.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
PIN = 'effe4ec5c57cd400a5f818b21c9903fef237edbc'
BUILD = '26C5035r'
TARGET = 'arm64e-apple-macos27.0'
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE / 'patcher'))
native = importlib.import_module('native_macos27')


def command(argv, **kwargs):
    return subprocess.run([str(a) for a in argv], check=True, timeout=120, **kwargs)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def signature(path):
    command(['codesign', '--verify', '--strict', path], capture_output=True)
    detail = command(['codesign', '--display', '--verbose=4', path], capture_output=True, text=True).stderr
    cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    identifier = re.search(r'^Identifier=(.+)$', detail, re.M)
    if not cdhash or not identifier:
        raise ValueError('Missing code signature identity: ' + path.name)
    return {'cdhash': cdhash[1], 'identifier': identifier[1]}


def entitlements(path):
    data = command(['codesign', '--display', '--entitlements', ':-', path], capture_output=True).stdout
    return plistlib.loads(data) if data.strip() else {}


def sign(path, identifier=None, entitlement=None, preserve=False):
    args = ['codesign', '--force', '--sign', '-', '--pagesize', '4096']
    if identifier:
        args += ['--identifier', identifier]
    if entitlement:
        args += ['--entitlements', entitlement]
    if preserve:
        args += ['--preserve-metadata=identifier,entitlements,flags']
    command(args + [path], capture_output=True)
    return signature(path)


def external_output(path):
    path = path.expanduser().resolve()
    for parent in (path, *path.parents):
        if (parent / '.git').exists():
            raise ValueError('Apple runtime output must be outside every Git checkout')
    if path == Path('/') or path == Path.home():
        raise ValueError('Use a dedicated external output directory')
    return path


def compile_program(source, output, *, sdk, extra=()):
    args = ['xcrun', '--sdk', 'macosx', 'clang', '-target', TARGET, '-isysroot', sdk,
            '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
            '-fstack-protector-strong', *extra, HERE / source, '-o', output]
    command(args, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bluetoothd', required=True, type=Path, help='External, original macOS 27.3 daemon')
    parser.add_argument('--output', required=True, type=Path, help='External directory; runtime/ is created inside it')
    parser.add_argument('--sdk', type=Path, help='macOS SDK, default xcrun macosx')
    args = parser.parse_args()
    out = external_output(args.output)
    runtime = out / 'runtime'
    build = out / 'build'
    for directory in (build, *(runtime / d for d in ('bin', 'lib', 'helpers', 'scripts', 'launchd'))):
        directory.mkdir(parents=True, exist_ok=True)
    # A failed partial invocation must never leave an apparently usable report.
    (runtime / 'report.json').unlink(missing_ok=True)
    source = args.bluetoothd.expanduser().resolve(strict=True)
    thin = build / 'bluetoothd.arm64e'
    if source.read_bytes()[:4] == b'\xcf\xfa\xed\xfe':
        shutil.copyfile(source, thin)
    else:
        command(['lipo', source, '-thin', 'arm64e', '-output', thin], capture_output=True)
    image = native.NativeImage(thin)  # Hash, UUID, CPU ABI, cave and tables checked here.
    original_entitlements = entitlements(thin)
    original_signature = signature(thin)
    sdk = str(args.sdk or command(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], capture_output=True, text=True).stdout.strip())
    header = build / 'qbt_macos27_layout.h'
    header.write_text(native.layout_header())
    type0 = runtime / 'lib/QcaBTType0.dylib'
    compile_program('userland/Type0.c', type0, sdk=sdk,
                    extra=['-dynamiclib', '-install_name', native.NATIVE_DYLIB.decode(), '-fvisibility=hidden',
                           '-I', str(build), '-DQBT_LAYOUT_HEADER="qbt_macos27_layout.h"'])
    sign(type0, 'com.unixnu.piano.bluetooth.Type0')
    blue = runtime / 'bin/BlueTool'
    compile_program('userland/BlueTool.c', blue, sdk=sdk, extra=['-fblocks'])
    own_entitlements = build / 'no-container.plist'
    own_entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
    sign(blue, 'com.unixnu.piano.bluetooth.BlueTool', own_entitlements)
    patched = image.vendor_routing().volatile_root_keys().h4_shutdown().capability_adapter()
    daemon = runtime / 'bin/bluetoothd'
    daemon.write_bytes(patched.data)
    daemon.chmod(0o755)
    signed = sign(daemon, preserve=True)
    if entitlements(daemon) != original_entitlements or signed['identifier'] != original_signature['identifier']:
        raise ValueError('Original daemon entitlements or identifier changed')
    final = native.ios._inspect.Image(daemon)
    if final.uuid != native.ORIGINAL_UUID or final.subtype != 0x80000002:
        raise ValueError('Signed daemon ABI or UUID changed')
    if final.data.count(native.NATIVE_DYLIB + b'\0') != 1:
        raise ValueError('Capability-library load command differs')
    mount = runtime / 'helpers/piano-bt-runtime-mount'
    compile_program('mount-runtime.c', mount, sdk=sdk)
    mount_entitlements = build / 'mount-entitlements.plist'
    mount_entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True,
                                                  'com.apple.private.bindfs-allow': True}))
    sign(mount, 'com.unixnu.piano.bluetooth.mount', mount_entitlements)
    probe = runtime / 'helpers/piano-bt-probe'
    compile_program('probe.m', probe, sdk=sdk,
                    extra=['-fobjc-arc', '-fblocks', '-framework', 'Foundation', '-framework', 'IOBluetooth', '-framework', 'CoreBluetooth'])
    sign(probe, 'com.unixnu.piano.bluetooth.probe', own_entitlements)
    for src, dest in (('start-runtime.sh', 'scripts/piano-bluetooth-start'),
                      ('com.unixnu.piano-bluetooth-path1.plist', 'launchd/com.unixnu.piano-bluetooth-path1.plist')):
        target = runtime / dest
        shutil.copyfile(HERE / src, target)
        target.chmod(0o755 if dest.startswith('scripts/') else 0o644)
    code_names = ('bin/bluetoothd', 'bin/BlueTool', 'lib/QcaBTType0.dylib',
                  'helpers/piano-bt-runtime-mount', 'helpers/piano-bt-probe')
    files = {}
    for name in code_names:
        path = runtime / name
        path.chmod(0o755)
        files[name] = dict(signature(path), sha256=sha(path), bytes=path.stat().st_size,
                           entitlement_count=len(entitlements(path)))
    marker = runtime / 'bin/PianoBluetooth.runtime'
    marker.write_text(json.dumps({'format': 1, 'build': BUILD, 'native_input_sha256': native.ORIGINAL_SHA256,
                                 'files': {n: files[n]['sha256'] for n in code_names[:3]}}, sort_keys=True) + '\n')
    marker.chmod(0o644)
    checked = (*code_names[:3], 'bin/PianoBluetooth.runtime')
    (runtime / 'runtime.sha256').write_text(''.join(f'{sha(runtime / n)}  {n}\n' for n in checked))
    for name in ('bin/PianoBluetooth.runtime', 'runtime.sha256', 'scripts/piano-bluetooth-start',
                 'launchd/com.unixnu.piano-bluetooth-path1.plist'):
        path = runtime / name
        files[name] = {'sha256': sha(path), 'bytes': path.stat().st_size}
    inputs = {str(p.relative_to(HERE)): sha(p) for p in sorted(HERE.rglob('*'))
              if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc'}
    commit = command(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    report = {'format': 1, 'build': BUILD, 'target': TARGET, 'upstream_commit': PIN, 'source_commit': commit,
              'native_input_sha256': native.ORIGINAL_SHA256, 'native_uuid': native.ORIGINAL_UUID,
              'native_entitlements_preserved': True, 'native_entitlement_count': len(original_entitlements),
              'hardware_verified': False, 'files': files, 'inputs': inputs, 'patch': patched.report()}
    (runtime / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print('piano-bt-path1 runtime built: ' + str(runtime))
    print('version=' + BUILD + ' signed_machos=5 hardware_verified=false')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode(errors='replace') if isinstance(error.stderr, bytes) else error.stderr or ''
        raise SystemExit(f'Build command failed ({error.returncode}): {detail.strip()}') from None
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error)) from None
