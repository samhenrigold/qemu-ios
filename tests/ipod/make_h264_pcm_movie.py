#!/usr/bin/env python3
"""Generate a firmware-free PCM video fixture; requires ffmpeg on PATH.

Usage: python3 tests/ipod/make_h264_pcm_movie.py output.mov [--audio source.mov]
The adjacent output.nv12 contains independently constructed reference pixels.
"""
from pathlib import Path
import subprocess
import argparse
import tempfile

class Bits:

    def __init__(self):
        self.bits = []

    def put(self, v, n):
        self.bits += [v >> i & 1 for i in range(n - 1, -1, -1)]

    def ue(self, v):
        n = (v + 1).bit_length()
        self.put(0, n - 1)
        self.put(v + 1, n)

    def align(self):
        while len(self.bits) % 8:
            self.put(0, 1)

    def nal(self):
        self.put(1, 1)
        self.align()
        raw = bytes((sum((self.bits[i + j] << 7 - j for j in range(8))) for i in range(0, len(self.bits), 8)))
        out = bytearray()
        zeros = 0
        for b in raw:
            if zeros == 2 and b <= 3:
                out.append(3)
                zeros = 0
            out.append(b)
            zeros = zeros + 1 if b == 0 else 0
        return b'\x00\x00\x00\x01' + out
s = Bits()
s.put(103, 8)
s.put(66, 8)
s.put(192, 8)
s.put(30, 8)
s.ue(0)
s.ue(0)
s.ue(2)
s.ue(1)
s.put(0, 1)
s.ue(3)
s.ue(3)
s.put(12, 4)
p = Bits()
p.put(104, 8)
p.ue(0)
p.ue(0)
p.put(0, 2)
p.ue(0)
p.ue(0)
p.ue(0)
p.put(0, 3)
p.ue(0)
p.ue(0)
p.ue(0)
p.put(4, 3)
stream = bytearray(s.nal() + p.nal())
expected = bytearray()
for frame in range(180):
    idr = frame % 16 == 0
    Y = bytes((16 + (x * 2 + y + frame) % 220 for y in range(64) for x in range(64)))
    U = bytes([80 + frame % 80]) * 1024
    V = bytes([160 - frame % 80]) * 1024
    expected += Y + b''.join((bytes([u, v]) for u, v in zip(U, V)))
    for first in (0, 8):
        b = Bits()
        b.put(101 if idr else 65, 8)
        b.ue(first)
        b.ue(2 if idr else 0)
        b.ue(0)
        b.put(frame % 16, 4)
        if idr:
            b.ue(0)
        else:
            b.put(0, 2)
        b.put(0, 2 if idr else 1)
        b.ue(0)
        b.ue(1)
        for mb in range(first, first + 8):
            if not idr:
                b.ue(0)
            b.ue(25 if idr else 30)
            b.align()
            x = mb % 4 * 16
            y = mb // 4 * 16
            for row in range(16):
                for value in Y[(y + row) * 64 + x:(y + row) * 64 + x + 16]:
                    b.put(value, 8)
            for plane in (U, V):
                for row in range(8):
                    for value in plane[(y // 2 + row) * 32 + x // 2:(y // 2 + row) * 32 + x // 2 + 8]:
                        b.put(value, 8)
        stream += b.nal()
parser = argparse.ArgumentParser(description='Generate a Baseline PCM movie and independently known NV12 pixels.')
parser.add_argument('output', type=Path)
parser.add_argument('--audio', type=Path, help='Optional movie supplying the audio track')
args = parser.parse_args()
output = args.output.resolve()
with tempfile.TemporaryDirectory(prefix='h264-pcm-') as tmp:
    raw = Path(tmp) / 'input.h264'
    decoded = Path(tmp) / 'decoded.nv12'
    raw.write_bytes(stream)
    command = ['ffmpeg', '-v', 'error', '-n', '-r', '30', '-i', str(raw)]
    if args.audio:
        command += ['-i', str(args.audio.resolve()), '-map', '0:v', '-map', '1:a:0']
    command += ['-c', 'copy', str(output)]
    subprocess.run(command, check=True)
    subprocess.run(['ffmpeg', '-v', 'error', '-i', str(output), '-pix_fmt', 'nv12', '-f', 'rawvideo', str(decoded)], check=True)
    assert decoded.read_bytes() == expected
output.with_suffix('.nv12').write_bytes(expected)
print('PASS: 180 two-slice I_PCM/P_PCM pictures match independent raw pixels')
