#!/usr/bin/env python3
"""Run the actual core-voltage command handlers with sanitizer bounds checks."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[2]/'hw/arm/ipod_touch_swi.c').read_text()
s=s[s.index('static uint64_t swi_read('):s.index('static const MemoryRegionOps')]
c=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t hwaddr;
typedef struct { uint32_t regs[0x1000/4]; } IPodSWIState;
'''+s+r'''
int main(void)
{
    IPodSWIState s={0};
    /* Both channels are reused by voltage-up/down transitions. */
    for(unsigned n=0;n<100;n++) {
        unsigned ctl=(n&1)?0x1c:0x14, command=0x880+(n&127);
        swi_write(&s,ctl+4,command,4);swi_write(&s,ctl,3,4);
        assert(swi_read(&s,ctl,4)==2 && swi_read(&s,ctl+4,4)==command);
    }
    swi_write(&s,0x18,0x5200,4);swi_write(&s,0x14,1,4);
    assert(swi_read(&s,0x14,4)==0 && swi_read(&s,0x18,4)==0x5200);
    swi_write(&s,0,0x303,4);swi_write(&s,0x24,42,4);
    assert(swi_read(&s,0,4)==0x303 && swi_read(&s,0x24,4)==42);
    swi_write(&s,0xffc,123,4);assert(swi_read(&s,0xffc,4)==123);
    puts("PASS: both SWI channels complete repeated voltage commands; configuration preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='it-swi-') as tmp:
    p=Path(tmp)/'check.c';exe=Path(tmp)/'check';p.write_text(c)
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
