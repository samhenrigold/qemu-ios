#!/usr/bin/env python3
"""Paletted-texture entries decode as little-endian shorts (LightTouchMac issue 15)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'hw/arm/gles-host.c').read_text()
source = source[source.index('static void gles_palette_entry('):
                source.index('/*\n * Decode one mip level')]
harness = r'''
#include <assert.h>
#include <stdint.h>
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
''' + source + r'''
int main(void) {
    uint8_t o[4];
    const uint8_t red5551[] = { 0x01, 0xf8 };   /* 0xf801: opaque red */
    const uint8_t blue565[] = { 0x1f, 0x00 };   /* 0x001f: blue */
    const uint8_t g4444[] = { 0xf0, 0x0f };     /* 0x0ff0: green+blue, alpha 0 */
    gles_palette_entry(red5551, GL_UNSIGNED_SHORT_5_5_5_1, o);
    assert(o[0] == 255 && o[1] == 0 && o[2] == 0 && o[3] == 255);
    gles_palette_entry(blue565, GL_UNSIGNED_SHORT_5_6_5, o);
    assert(o[0] == 0 && o[1] == 0 && o[2] == 255 && o[3] == 255);
    gles_palette_entry(g4444, GL_UNSIGNED_SHORT_4_4_4_4, o);
    assert(o[0] == 0 && o[1] == 255 && o[2] == 255 && o[3] == 0);
    return 0;
}
'''

with tempfile.TemporaryDirectory() as d:
    c, exe = Path(d, 't.c'), Path(d, 't')
    c.write_text(harness)
    subprocess.run(['cc', '-Wall', '-Werror', '-o', exe, c], check=True)
    subprocess.run([exe], check=True)
print('ok')
