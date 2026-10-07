#!/usr/bin/env python3
"""Shared GPIO groups must preserve other pending sources when one is ACKed."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'hw/arm/ipod_touch_sysic.c').read_text()
header=(root/'include/hw/arm/ipod_touch_sysic.h').read_text()
constants='\n'.join(x for x in header.splitlines() if x.startswith(('#define GPIO_', '#define POWER_')))
state=re.search(r'typedef struct IPodTouchSYSICState \{.*?\n} IPodTouchSYSICState;',header,re.S).group()
functions=[]
for name in ('sysic_update_gpio_irq','sysic_gpio_irq_input','ipod_touch_sysic_read','ipod_touch_sysic_write','sysic_post_load'):
    match=re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)
    assert match,name
    functions.append(match.group())
prelude=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef int *qemu_irq;
typedef uint64_t hwaddr;
static bool sysic_gpio_trace(void) { return false; }
static void qemu_set_irq(int *irq,int value) { *irq=!!value; }
'''
tests=r'''
int main(void) {
    int level[GPIO_NUMINTGROUPS]={0};IPodTouchSYSICState s={0};
    for (unsigned i=0;i<GPIO_NUMINTGROUPS;i++) s.gpio_irqs[i]=&level[i];
    s.gpio_int_status[3]=(1u<<1)|(1u<<13); /* PMU and digitizer share group 3. */
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,~0u,4);assert(level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+12,1u<<13,4);
    assert(level[3] && s.gpio_int_status[3]==2);
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,0,4);assert(!level[3]);
    assert(ipod_touch_sysic_read(&s,GPIO_INTSTAT+12,4)==2);
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,2,4);assert(level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+12,2,4);assert(!level[3]);
    s.gpio_int_status[3]=2;sysic_post_load(&s,1);assert(level[3]);
    s.gpio_int_enabled[3]=0;sysic_post_load(&s,1);assert(!level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+28,~0u,4);
    ipod_touch_sysic_write(&s,GPIO_INTEN+28,~0u,4);
    assert(ipod_touch_sysic_read(&s,GPIO_INTSTAT+28,4)==0);
    /* PMU events remain asserted across a GPIO ACK until I2C consumes them. */
    sysic_gpio_irq_input(&s,97,1);assert(!level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,2,4);assert(level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+12,2,4);assert(level[3]);
    /* The guest masks then ACKs before scheduling its I2C worker. */
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,0,4);assert(!level[3]);
    ipod_touch_sysic_write(&s,GPIO_INTSTAT+12,2,4);
    assert(!s.gpio_int_status[3] && s.gpio_level_pending[3]==2);
    ipod_touch_sysic_write(&s,GPIO_INTEN+12,2,4);assert(level[3]);
    sysic_gpio_irq_input(&s,97,0);assert(!level[3] && !s.gpio_int_status[3]);
    puts("PASS: shared GPIO pending ACK, mask/unmask, restore and group bounds");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'check.c';c.write_text(prelude+constants+'\n'+state+'\n'+'\n'.join(functions)+tests)
    binary=str(Path(tmp)/'check')
    subprocess.run(['clang','-std=gnu11','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',binary],check=True)
    subprocess.run([binary],check=True)
