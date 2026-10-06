#!/usr/bin/env python3
"""Display panel= size (issue #21): iBoot's native geometry words (the board's panel) read back as the panel's.

AppleDisplayPipe/AppleCLCD adopt the panel geometry iBoot programmed (size, UI layer source/end,
stride, CLCD timing size). With panel=WxH those words must read as the panel's; on the shipped
panel (panel word 0) every register reads exactly what was written.

Mutation (named, must fail this test): in panel_word's DP_SIZE case return `v` instead of `panel`.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/s5l8930_display.c').read_text()
m = re.search(r'^static uint32_t panel_word\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
assert m, 'panel_word'
defines = '\n'.join(l for l in source.splitlines()
                    if re.match(r'#define (DP_SIZE|DP_UI_BASE|DP_UI_STRIDE|DP_UI_SRC_SIZE|DP_UI_DST_END|'
                                r'CLCD_SIZE|DEFAULT_WIDTH|DEFAULT_HEIGHT)\b', l))
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t hwaddr;
typedef struct { uint32_t panel, native; } DisplayPipe;
''' + defines + '\n' + m.group() + r'''
int main(void)
{
    const uint32_t native = 1024u << 16 | 768, tall = 1280u << 16 | 768;
    DisplayPipe k48 = { 0, native }, k48t = { tall, native };
#define panel_word(pn, a, v) panel_word((pn) ? &k48t : &k48, a, v)
    hwaddr geometry[] = { DP_SIZE, DP_UI_BASE(0) + DP_UI_SRC_SIZE, DP_UI_BASE(0) + DP_UI_DST_END,
                          DP_UI_BASE(1) + DP_UI_SRC_SIZE, DP_UI_BASE(1) + DP_UI_DST_END };
    for (unsigned i = 0; i < sizeof(geometry) / sizeof(geometry[0]); i++) {
        assert(panel_word(0, geometry[i], native) == native);
        assert(panel_word(tall, geometry[i], native) == tall);
        assert(panel_word(tall, geometry[i], 0x02000100) == 0x02000100);   /* a CA layer rect stays */
    }
    assert(panel_word(0, DP_UI_BASE(0) + DP_UI_STRIDE, 4096) == 4096);
    assert(panel_word(tall, DP_UI_BASE(0) + DP_UI_STRIDE, 4096) == 5120);
    assert(panel_word(tall, DP_UI_BASE(0) + DP_UI_STRIDE, 4096 << 4 | 2) == (4096 << 4 | 2));
    assert(panel_word(tall, CLCD_SIZE, 1023u << 16 | 767) == (1279u << 16 | 767));
    assert(panel_word(tall, 0x1038, native) == native);                  /* not a geometry word */
#undef panel_word
    const uint32_t n90 = 640u << 16 | 960, big = 768u << 16 | 1024;
    DisplayPipe p = { big, n90 }, q = { 0, n90 };
    assert(panel_word(&p, DP_SIZE, n90) == big);
    assert(panel_word(&p, DP_SIZE, native) == native);                  /* K48's geometry is not N90's */
    assert(panel_word(&q, DP_SIZE, n90) == n90);
    assert(panel_word(&p, DP_UI_BASE(0) + DP_UI_STRIDE, 640 * 4 | 2) == (768 * 4 | 2));
    assert(panel_word(&p, CLCD_SIZE, 639u << 16 | 959) == (767u << 16 | 1023));
    puts("PASS: panel= geometry words read as the panel's (K48 and a portrait board); unset reads raw");
}
'''
with tempfile.TemporaryDirectory(prefix='display-panel-') as d:
    c, exe = Path(d) / 't.c', Path(d) / 't'
    c.write_text(code)
    subprocess.run(['clang', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(c), '-o', str(exe)],
                   check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=20)
