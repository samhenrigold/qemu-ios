#!/usr/bin/env python3
"""Exercise production N72 clock derivation; no duplicated frequency function."""
from pathlib import Path
import re, shutil, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'hw/arm/ipod_touch_clock.c').read_text()
header = (ROOT / 'include/hw/arm/ipod_touch_clock.h').read_text()
def function(name):
    match = re.search(r'static (?:uint32_t|void) ' + name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    start = match.start(); pos = match.end(); depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}'); pos += 1
    return source[start:pos]
fields = re.findall(r'uint32_t\s+(\w+);', header)
body = '''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
typedef struct { uint32_t word2; } IPodTouchChipIDState;
typedef struct { uint64_t hz; } Clock;
static void clock_update_hz(Clock *c, uint64_t hz) { c->hz = hz; }
typedef struct IPodTouchClockState {
''' + '\n'.join('uint32_t ' + f + ';' for f in fields) + '''
bool s5l8720; IPodTouchChipIDState *chipid; Clock *pclk;
} IPodTouchClockState;
'''
body += function('s5l8720_pll_locks') + function('ipod_touch_clock_update')
body += '''
static uint64_t frequency(IPodTouchClockState *s) {
 ipod_touch_clock_update(s); return s->pclk->hz;
}
int main(void) {
 Clock c={0}; IPodTouchChipIDState chip={.word2=0x87200004};
 IPodTouchClockState s={.s5l8720=true,.chipid=&chip,.pclk=&c};
 assert(frequency(&s)==12000000);
 s.config1=0x4200; assert(frequency(&s)==3000000);
 chip.word2|=1; assert(frequency(&s)==6000000);
 s.config0=0x1000; s.pll0con=0x03008500; s.pllmode=1;
 assert(frequency(&s)==266000000);
 for(unsigned shift=0;shift<8;shift++) {
  s.pll0con=0x03008500|shift;
  assert(frequency(&s)==266000000/(1U<<shift));
 }
 s.pll0con=0x03008500; chip.word2&=~1;
 assert(frequency(&s)==133000000);
 s.pllmode=0x11; assert(frequency(&s)==299250000);
 chip.word2|=1; assert(frequency(&s)==299250000);
 s.config0=0x2000; s.pll1con=0x06005101; s.pllmode=3;
 assert(frequency(&s)==20250000);
 s.config0=0x3000; s.pll2con=0x02004002; s.pllmode=7;
 assert(frequency(&s)==24000000);
 s.pllmode=3; assert(frequency(&s)==0);
 s.pllmode=7; s.pll2con=0x00004002; assert(frequency(&s)==0);
 s.config0=0; s.pllmode=0; assert(frequency(&s)==6000000);
 s.chipid=0; assert(frequency(&s)==0);
 s.chipid=&chip; s.s5l8720=false; assert(frequency(&s)==0);
 return 0;
}
'''
compiler = shutil.which('clang') or shutil.which('cc'); assert compiler
with tempfile.TemporaryDirectory(prefix='n72-clock-') as directory:
    c = Path(directory) / 'clock.c'; binary = Path(directory) / 'clock'
    c.write_text(body)
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS actual N72 reference/bypass/SDIV/invalid-selection derivation')
