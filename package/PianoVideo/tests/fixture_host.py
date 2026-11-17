#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang. Reproduce our own encode, inspect SPS, independently software-decode the committed bytes.
import hashlib
from pathlib import Path
import re
import subprocess


def ffmpeg(args, data):
    return subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', *args], input=data,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout


def sha(data):
    return hashlib.sha256(data).hexdigest()


text = (Path(__file__).parents[1] / 'VideoFixture.h').read_text()
fixture = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', text.split('kAccessUnit[] = {')[1].split('};')[0]))
source = bytes([64])*76800 + bytes([96])*19200 + bytes([160])*19200
encoded = ffmpeg(['-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size', '320x240', '-framerate', '1',
                  '-i', 'pipe:0', '-frames:v', '1', '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '1.3',
                  '-preset', 'medium', '-qp', '20', '-x264-params',
                  'threads=1:keyint=1:min-keyint=1:scenecut=0:ref=1:bframes=0:cabac=0:8x8dct=0:deblock=0,0',
                  '-fflags', '+bitexact', '-flags:v', '+bitexact', '-f', 'h264', 'pipe:1'], source)
without_sei = ffmpeg(['-f', 'h264', '-i', 'pipe:0', '-c', 'copy', '-bsf:v', 'filter_units=remove_types=6',
                      '-f', 'h264', 'pipe:1'], encoded)
assert fixture == without_sei
assert len(fixture) == 286 and sha(fixture) == 'ed63e8ee3deeb819b059f5c8346f66da0592b1922eec0198a47a6efa33f96923'
nals = [n for n in re.split(b'\x00\x00\x00?\x01', fixture) if n]
assert [n[0] & 31 for n in nals] == [7, 8, 5]
# Independent minimal Baseline SPS parser (Exp-Golomb, RBSP unescaping). No decoder/firmware code shared.
sps = re.sub(b'\x00\x00\x03', b'\x00\x00', nals[0][1:])
bits = ''.join(f'{b:08b}' for b in sps)
at = 0


def take(n):
    global at
    value = int(bits[at:at+n], 2)
    at += n
    return value


def ue():
    zeros = 0
    while take(1) == 0:
        zeros += 1
    return (1 << zeros) - 1 + (take(zeros) if zeros else 0)


assert take(8) == 66
constraints = take(8)
assert take(8) == 13
assert ue() == 0  # SPS id
assert ue() == 0  # log2_max_frame_num_minus4
assert ue() == 2  # pic_order_cnt_type
assert ue() == 0  # max_num_ref_frames: does NOT prescribe firmware DPB count
assert take(1) == 0  # gaps
assert ue() == 19 and ue() == 14
assert take(1) == 1  # progressive, frame_mbs_only
_ = take(1)  # direct_8x8_inference
assert take(1) == 0  # no crop
nv12 = ffmpeg(['-f', 'h264', '-i', 'pipe:0', '-pix_fmt', 'nv12', '-f', 'rawvideo', 'pipe:1'], fixture)
assert nv12 == bytes([64])*76800 + bytes([96,160])*19200
assert len(nv12) == 115200 and sha(nv12) == 'bd230ec4a9cd22c274831d1b526c43d3ceb39a089257c3cb4114fbea0a8102dc'
# Wrong padding/UV stride cannot silently equal the active-area digest.
padded = bytearray([0xa5])*147456
for y in range(240): padded[y*384:y*384+320] = nv12[y*320:y*320+320]
for y in range(120): padded[98304+y*384:98304+y*384+320] = nv12[76800+y*320:76800+y*320+320]
assert sha(padded[:115200]) != sha(nv12)
assert sha(padded) != sha(nv12)
print(f'V4 own fixture PASS bytes=286 profile=66 level=13 POC=2 refs=0 geometry=320x240 progressive=1 crop=0 constraints={constraints:#x}')
print(f'V4 independent software NV12 PASS active={len(nv12)} SHA256={sha(nv12)}')
