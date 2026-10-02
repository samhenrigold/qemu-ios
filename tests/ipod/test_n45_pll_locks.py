#!/usr/bin/env python3
"""Actual clock helpers/read handler: N45 PLL enables and ten-bit multiplier.
The production-board companion is qtest/ipod-n45-rom; no firmware inputs.
"""
from pathlib import Path
import re, shutil, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'hw/arm/ipod_touch_clock.c').read_text()
header = (ROOT / 'include/hw/arm/ipod_touch_clock.h').read_text()
def function(name):
    match = re.search(r'static uint(?:32|64)_t ' + name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    start = match.start(); pos = match.end(); depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}'); pos += 1
    return source[start:pos]
macros = '\n'.join(line for line in header.splitlines() if line.startswith('#define CLOCK_'))
fields = re.findall(r'uint32_t\s+(\w+);', header)
prefix = '''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
typedef uint64_t hwaddr;
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define LOG_UNIMP 0
#define qemu_log_mask(...) ((void)0)
'''
body = prefix + macros + '\ntypedef struct IPodTouchClockState {\n' + '\n'.join('uint32_t ' + f + ';' for f in fields) + '\nbool s5l8900, s5l8900_pll, s5l8720;\n} IPodTouchClockState;\n'
body += '\n'.join(function(n) for n in ['s5l8720_pll_locks', 's5l8900_pll_locks', 's5l8900_clock_read_reg'])
body += '''
static uint32_t readlock(IPodTouchClockState *s) {
 return s5l8900_clock_read_reg(s, CLOCK_PLLLOCK, 4);
}
int main(void) {
 IPodTouchClockState s = {.s5l8900=true,.s5l8900_pll=true,.pllmode=0x111,.pll0con=0x08005000};
 assert(readlock(&s)==1); /* real ROM expects exactly one */
 s.pllmode=0x110; assert(readlock(&s)==0);
 s.pllmode=0x111; s.pll0con=0x00005000; assert(readlock(&s)==0);
 s.pll0con=0x08000000; assert(readlock(&s)==0);
 s.pll0con=0x08010000; assert(readlock(&s)==1); /* bit16 belongs to MDIV */
 s.pll3con=0x08004801; s.pllmode=0x119; assert(readlock(&s)==9);
 s.pllmode=0x118; assert(readlock(&s)==8);
 s.pll3con=0; assert(readlock(&s)==0);
 s.s5l8900=false; s.s5l8900_pll=false; s.s5l8720=true; s.pllmode=1;
 assert(readlock(&s)==0); /* N72 multiplier remains eight bits */
 s.pll0con=0x08005000; assert(readlock(&s)==1);
 s.s5l8720=false; s.s5l8900=true; assert(readlock(&s)==15); /* unchanged secondary fallback */
 return 0;
}
'''
compiler = shutil.which('clang') or shutil.which('cc')
assert compiler, 'C compiler required'
with tempfile.TemporaryDirectory(prefix='n45-pll-') as directory:
    c = Path(directory) / 'locks.c'; binary = Path(directory) / 'locks'
    c.write_text(body)
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-fsanitize=address,undefined', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS actual N45 PLL/read handler, invalid settings, four enables, N72 separation')
