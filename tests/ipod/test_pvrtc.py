#!/usr/bin/env python3
"""PVRTC1 reference vectors and tiny-mip/RGB policy under ASan and UBSan.

Golden RGBA hashes were generated with PowerVR Native SDK's
framework/PVRCore/texture/PVRTDecompress.cpp at revision
34f68e4c028e06704ff2f69f292a61f9ba0c53c4 (no runtime dependency):
https://github.com/powervr-graphics/Native_SDK/tree/34f68e4c028e06704ff2f69f292a61f9ba0c53c4
The deterministic compressed words below mix opaque/translucent endpoints.
"""
from pathlib import Path
import hashlib
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'hw/arm/gles-host.c').read_text()
source = source[source.index('static void pvrtc_endpoints('):
                source.index('/*\n * The paletted formats:')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t ldl_le_p(const uint8_t *p) {
    return p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24;
}
''' + source + r'''
int main(int argc, char **argv) {
    assert(argc == 5);
    unsigned w = atoi(argv[1]), h = atoi(argv[2]);
    int bpp = atoi(argv[3]);
    assert(w && w<=4096 && h && h<=4096 && (bpp==2 || bpp==4));
    assert(!pvrtc_size(0, 4, bpp) && !pvrtc_size(3, 4, bpp));
    assert(!pvrtc_size(4, 0, bpp) && !pvrtc_size(4, 3, bpp));
    size_t length = pvrtc_size(w, h, bpp);
    uint8_t *src = malloc(length), *dst = malloc((size_t)w*h*4);
    assert(src && dst && fread(src,1,length,stdin)==length && getchar()==EOF);
    pvrtc_decode(src, w, h, bpp, atoi(argv[4]), dst);
    assert(fwrite(dst,1,(size_t)w*h*4,stdout)==(size_t)w*h*4);
    free(src);
    free(dst);
}
'''

# bpp, width, height: one hash per uniform modulation mode, in encoding order.
GOLDEN = {
    (2, 16, 8): [
        '72c90e12d14c9374885a07b395af1b3f16f4879c9bb95654982e943ba3ab5307',
        'fba11314f25670c4c34354fc9322a77134e68c7e184942c7382b085faa65cec1',
        'ef7355f60b9b8f80f64831e1285641aef062ef146980acb768fb9867fddcbac5',
        '262a7d66828859b7998e47d183e2aa47f52cbe6af2397e47bca2dda37aa16651'],
    (2, 32, 8): [
        'd42d791f714fb72ec3a5fe425c02bbefe98d514c18db3f4b590cc929d2a28c8f',
        'c2f6559b9cf4f6cdadcc1342aa22114c8bc6874925b0116007dd25258a19169b',
        'aa0b0e85bd60d88e7fc23d3433c3d4d3628b9064a8e040292eafba8bda464bd1',
        '4ba42dd3625bcc87c41a5676dec616ed2502608f48f486053a9a92a69c6dd3ca'],
    (2, 16, 16): [
        '03ac7b9bc39028790a92e970848d0619a82d9295eda94c2825afeba1d36d7430',
        'a9d3dbacc32c60e8b18da4b084fe1c57a79eb56d21038179c3508924bce7fee0',
        'e0a943ce9cbe6cf9a1fd3cebb347b3e5b7e0fe100da2462ae302581af3331e36',
        '658ded23d78109bc4380e898ec128f33dffaa6f9bfd938db3c0edb5fd94e36fc'],
    (4, 8, 8): [
        '463a67be5143569de1a28c58539936a0a51f822441c1f132271882461fd38438',
        'db9f25cf6b7a09b425387a9bbe1f71b9bffdc23a0669ab8fde5f6648e19103ba'],
    (4, 32, 8): [
        '2a357e88ce96cce4cecec521ae49d22bf4b014bc980b46548d2d02b17751e4b3',
        '62b7cfee543c03b76954775cac5c592c465a9fce0e305d896b3b4d59aa9cbe59'],
    (4, 8, 32): [
        '76c0d7d11e3a8ced873020f2bfe8e6c089b41dca2dd84663d06abfc8f8df9732',
        '31a323b4c8ecd1b4af066cbafe1703160963fa48948c1328d7c64437fbbf5888'],
}


def compressed(w, h, bpp, mode):
    state = 0x12345678
    data = bytearray()
    for i in range(w * h * bpp // 64):
        words = []
        for _ in range(2):
            state ^= state << 13 & 0xffffffff
            state ^= state >> 17
            state ^= state << 5 & 0xffffffff
            words.append(state)
        modulation, color = words
        current = i % (4 if bpp == 2 else 2) if mode is None else mode
        color = (color & ~1) | (current != 0)
        if bpp == 2:
            if current:
                modulation = (modulation & ~1) | (current >= 2)
            if current >= 2:
                modulation = (modulation & ~(1 << 20)) | ((current == 3) << 20)
        data += struct.pack('<II', modulation, color)
    return bytes(data)


with tempfile.TemporaryDirectory(prefix='pvrtc-check-') as directory:
    c = Path(directory) / 'check.c'
    exe = Path(directory) / 'check'
    c.write_text(harness)
    subprocess.run(['clang', '-O1', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(c), '-o', str(exe)], check=True)

    def decode(data, w, h, bpp, alpha=True):
        return subprocess.run([str(exe), str(w), str(h), str(bpp), str(int(alpha))],
                              input=data, stdout=subprocess.PIPE, check=True).stdout

    for (bpp, w, h), hashes in GOLDEN.items():
        for mode, expected in enumerate(hashes):
            data = compressed(w, h, bpp, mode)
            rgba = decode(data, w, h, bpp)
            assert hashlib.sha256(rgba).hexdigest() == expected, (bpp, w, h, mode)
            rgb = decode(data, w, h, bpp, False)
            assert rgb[3::4] == b'\xff' * (w*h)
            for channel in range(3):
                assert rgb[channel::4] == rgba[channel::4]
    for bpp, w, h, expected in [
        (2, 32, 16, '2119ee5d991f927d67b789bfaf1faca7a1c0578853df33e4ac576fd544dec4d9'),
        (4, 16, 16, '511484ca0bf5ebb59ed6bf7ae25a9d181356f26dfb3ada030d40b43d31b745e3'),
    ]:
        assert hashlib.sha256(decode(compressed(w, h, bpp, None), w, h, bpp)).hexdigest() == expected

    # Preserve the guest driver's compact one-word mip tails. Heap redzones
    # detect accidental reads of spec-padding blocks that the guest did not send.
    for bpp in (2, 4):
        for w in (1, 2, 4):
            for h in (1, 2, 4):
                data = struct.pack('<II', 0, 0xfffffffe)
                assert decode(data, w, h, bpp) == b'\xff' * (w*h*4)
    punch = struct.pack('<II', 0xaaaaaaaa, 0xffffffff)
    assert decode(punch, 4, 4, 4) == b'\xff\xff\xff\0' * 16
    assert decode(punch, 4, 4, 4, False) == b'\xff' * 64

print('PASS: 20 vendor vectors, all 2/4bpp modes, mixed neighbours, RGB alpha, tiny mips; ASan/UBSan')
