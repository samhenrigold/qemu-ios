#!/usr/bin/env python3
"""A discarded partial frame must not swap stereo channels after reset."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_i2s.c').read_text()
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {
    struct { unsigned nchannels; } as;
    unsigned ring_head;
    uint64_t total_bytes;
} IPodTouchI2SState;
static unsigned padded;
static void it_i2s_push(IPodTouchI2SState *s, const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; i++) assert(p[i] == 0);
    padded += n;
    s->ring_head = (s->ring_head + n) % 16;
    s->total_bytes += n;
}
'''
code += re.search(r'^static void it_i2s_align_frame\(.*?^}', source,
                  re.M | re.S).group() + '\n'
code += r'''
int main(void) {
    IPodTouchI2SState s = { .as.nchannels = 2 };
    /* A reset discards two pending bytes but retains the lifetime tap count. */
    s.total_bytes = 6;
    it_i2s_align_frame(&s);
    assert(padded == 0 && s.ring_head == 0);
    /* The next stereo frame must remain untouched. */
    s.ring_head = 4; s.total_bytes += 4;
    it_i2s_align_frame(&s);
    assert(padded == 0 && s.ring_head == 4);
    /* A real partial frame still needs padding, including at ring wrap. */
    s.ring_head = 14; s.total_bytes = 20;
    it_i2s_align_frame(&s);
    assert(padded == 2 && s.ring_head == 0);
    it_i2s_align_frame(&s);
    assert(padded == 2);
    puts("PASS: I2S stereo alignment across discarded data and ring wrap");
}
'''
with tempfile.TemporaryDirectory(prefix='i2s-alignment-') as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', str(c), '-o', str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
