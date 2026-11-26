#!/usr/bin/env python3
"""Verify synthetic Android v2 containers with an in-memory FAT file listing."""
import contextlib
import hashlib
import importlib.util
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


vr = load('verify-ram-images')
builder = load('build-ram-images')
PAGE = 2048


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pages(data):
    return data + bytes(-len(data) % PAGE)


def node(name, properties=(), children=()):
    props = [('name', name.encode() + b'\0'), *properties]
    return (struct.pack('<II', len(props), len(children)) +
            b''.join(key.encode().ljust(32, b'\0') + struct.pack('<I', len(data)) + data + bytes(-len(data) % 4)
                     for key, data in props) + b''.join(children))


class VerifyRamImages(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.image = self.root / 'boot.img'
        self.record = self.root / 'build.json'
        self.registry = self.root / 'warm.json'
        self.fd, self.dtb, self.payload = b'fixture FD', b'fixture DTB', b'fixture FAT'
        (self.root / 'fd').write_bytes(self.fd)
        (self.root / 'dtb').write_bytes(self.dtb)
        shim = bytearray(64)
        struct.pack_into('<QQQ', shim, 8, 0, PAGE, 0xa)
        shim[56:60] = b'ARM\x64'
        kernel = bytes(shim) + self.fd + self.dtb
        header = vr.HEADER.pack(b'ANDROID!', len(kernel), 0x8000, len(self.payload), 0x1000000, 0, 0, 0x100,
                                PAGE, 2, 0, b'', b'', vr.mkbootimg_id(kernel, self.payload, self.dtb), b'',
                                0, 0, vr.HEADER.size, len(self.dtb), 0x1f00000)
        self.data = b''.join(map(pages, (header, kernel, self.payload, self.dtb)))
        self.image.write_bytes(self.data)
        (self.root / 'reference.img').write_bytes(self.data)
        self.info = {'shim_bytes': len(shim), 'payload_sha256': digest(self.payload)}
        self.build = {'fd': {'file': 'fd', 'sha256': digest(self.fd)}, 'dtb_sha256': digest(self.dtb),
                      'images': {self.image.name: self.info}}
        self.files = {'EFI/UniXNU/unixnu.toml': b'version = 1\n[[entry]]\ntype = "action"\n'}

    def verify(self, *args, write_record=True):
        if write_record:
            self.record.write_text(json.dumps(self.build))
        files = {name: (len(data), digest(data), data.decode() if name.endswith('.toml') else None)
                 for name, data in self.files.items()}
        output = io.StringIO()
        with contextlib.redirect_stdout(output), patch.object(vr, 'WARM_SEEDS', self.registry), \
                patch.object(vr, 'fat_files', return_value=(True, files)), \
                patch.object(vr, 'fat_file', side_effect=lambda _, name: self.files[name]):
            code = vr.main([str(self.record), '--reference', str(self.root / 'reference.img'),
                            '--fd', str(self.root / 'fd'), '--dtb', str(self.root / 'dtb'), *map(str, args)])
        return code, output.getvalue()

    def check_result(self, status, item, *args, **kwargs):
        code, output = self.verify(*args, **kwargs)
        self.assertEqual(code, status, output)
        self.assertIn(item, output)

    def macos(self, args, guard=False, tail=None):
        self.files['EFI/UniXNU/unixnu.toml'] = f'[[entry]]\ntype = "xnu"\nargs = "{args}"\n'.encode()
        props = [('test-root', vr.ROOT_27_3 + b'\0')]
        if tail:
            props.append(('piano-boot-args-ram', tail.encode() + b'\0'))
        kc, adt, trust, seed = vr.PAYLOADS
        self.files.update({
            kc: b'fixture KC' + (b'\0data-store-replace\0' if guard else b''),
            adt: node('device-tree', children=[node('chosen', props, [node('memory-map')])]),
            trust: b'fixture trust',
            seed: b'fixture seed',
        })
        self.info.update({key: digest(self.files[name]) for name, key in vr.PAYLOADS.items()})

    def test_unsigned_and_explicit_path(self):
        self.check_result(0, 'PASS: 0 failure(s)')
        signed = self.root / 'boot-signed.img'
        signed.write_bytes(self.data)
        self.check_result(0, 'PASS: 0 failure(s)', signed)
        unrecorded = self.root / 'other.img'
        unrecorded.write_bytes(self.data)
        self.check_result(1, 'image record for other.img', unrecorded)

    def test_bad_record(self):
        self.record.write_text('{')
        self.check_result(1, 'FAIL: 1 failure(s)', write_record=False)
        with patch.dict(self.build, {'images': {}}):
            self.check_result(1, 'build record has no images')
        del self.info['payload_sha256']
        self.check_result(1, 'missing or invalid payload_sha256')

    def test_container_bounds(self):
        with patch.object(vr, 'IMAGE_MAX_BYTES', len(self.data)):
            self.check_result(0, 'PASS: 0 failure(s)')
            self.image.write_bytes(self.data + b'\0')
            self.check_result(1, 'exceeds')
        self.image.write_bytes(self.data[:-1])
        self.check_result(1, 'sections extend beyond the file')
        invalid = bytearray(self.data)
        struct.pack_into('<I', invalid, 36, 0)
        self.image.write_bytes(invalid)
        self.check_result(1, 'invalid boot image page size')

    def test_missing_macos_payload_hash(self):
        self.macos('data-persist=0')
        del self.info['trust_sha256']
        self.check_result(1, 'missing or invalid trust_sha256')

    def test_effective_store_guard_and_warm_seed(self):
        self.macos('piano-boot-args=ram', tail='data-persist=2')
        self.info['data_store'] = {'data_persist': 2, 'same_seed': True, 'kc_has_store_guard': True}
        self.check_result(1, 'uses the installed seed or has the store guard')
        self.macos('piano-boot-args=ram', guard=True, tail='data-persist=2')
        self.check_result(0, 'PASS: 0 failure(s)')
        self.info['data_store']['data_persist'] = 0
        self.check_result(1, 'boot data-persist=2 matches the build record')
        del self.info['data_store']
        self.registry.write_text(json.dumps({self.info['seed_sha256']: {}}))
        self.check_result(1, 'a warm test seed runs data-persist=0')
        self.macos('data-persist=0')
        self.check_result(0, 'PASS: 0 failure(s)')

    def test_exact_args_cannot_bypass_store_options(self):
        for arg in ('data-store-replace=1', 'data-store-flatten=1', 'data-persist=0 data-persist=2', 'data-persist'):
            with self.subTest(arg=arg), self.assertRaises(SystemExit):
                builder.exact_store_mode(arg.split())
        self.assertEqual(builder.exact_store_mode(['data-persist=2']), '2')

    def test_replacement_must_be_recorded(self):
        self.macos('data-persist=2 data-store-replace=1', guard=True)
        self.info['data_store'] = {'data_persist': 2}
        self.check_result(1, 'replacement or flattening requires the recorded guarded mode')
        self.info['data_store']['replace'] = '1'
        self.check_result(0, 'PASS: 0 failure(s)')


if __name__ == '__main__':
    unittest.main()
