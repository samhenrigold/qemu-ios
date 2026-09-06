#!/usr/bin/env python3
"""Production timer interval arithmetic and restored-state bounds."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'hw/arm/ipod_touch_timer.c').read_text()
header = (root/'include/hw/arm/ipod_touch_timer.h').read_text()
state = re.search(r'typedef struct IPodTouchTimerState.*?} IPodTouchTimerState;', header, re.S)[0]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t hwaddr;
typedef int SysBusDevice, MemoryRegion, QEMUTimer, Clock, qemu_irq;
#define MIN(a,b) ((a)<(b)?(a):(b))
#define NANOSECONDS_PER_SECOND 1000000000
#define QEMU_CLOCK_VIRTUAL 0
static int64_t now, deadline;
static bool timer_trace(void) { return false; }
static int64_t qemu_clock_get_ns(int clock) { return now; }
static void timer_mod(QEMUTimer *timer, int64_t value) { assert(value>=now);deadline=value; }
static void timer_del(QEMUTimer *timer) {}
static void qemu_irq_lower(qemu_irq irq) {}
static uint64_t muldiv64(uint64_t a,uint64_t b,uint64_t c) { return (unsigned __int128)a*b/c; }
'''
code += '\n'.join(re.findall(r'^#define (?:TIMER_|IT_TIMER_).*$',header,re.M))+'\n'+state+'\n'
for name in ['s5l8900_st_update','s5l8900_st_set_timer','s5l8900_timer1_write','ipod_touch_timer_post_load']:
    code += re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)[0]+'\n'
code += r'''
int main(void) {
 IPodTouchTimerState s={.dilation=1};
 for(unsigned repeat=0;repeat<2;repeat++) {
   for(unsigned addr=8;addr<0x80;addr+=0x20)s5l8900_timer1_write(&s,addr,1,4);
   s5l8900_timer1_write(&s,0x88,1,4); /* 64-bit control is not timer D. */
 }
 s5l8900_st_update(&s);assert(s.tick_interval==100000);
 s.bcount1=10000;s.dilation=2;s5l8900_st_update(&s);assert(s.tick_interval==2000000);
 now=123;s5l8900_st_set_timer(&s);assert(deadline==2000000);
 /* A restored interval survives a same-value guest reprogram. */
 uint64_t restored_interval=s.tick_interval;
 s5l8900_timer1_write(&s,TIMER_4+TIMER_CONFIG,0,4);
 assert(s.tick_interval==restored_interval);
 s.bcount1=UINT32_MAX;s.dilation=IT_TIMER_MAX_DILATION;s5l8900_st_update(&s);
 assert(s.tick_interval==UINT64_C(429496729500000000) && s.tick_interval<INT64_MAX);
 now=INT64_MAX-10;s5l8900_st_set_timer(&s);assert(deadline==INT64_MAX);
 assert(!ipod_touch_timer_post_load(&s,1));
 s.base_time=(uint64_t)now+1;assert(ipod_touch_timer_post_load(&s,1)==-EINVAL);
 s.base_time=0;s.status=TIMER_STATE_START;s.tick_interval=0;
 assert(ipod_touch_timer_post_load(&s,1)==-EINVAL);
 s.tick_interval=UINT64_MAX;assert(ipod_touch_timer_post_load(&s,1)==-EINVAL);
 s.tick_interval=1;s.next_planned_tick=UINT64_MAX;assert(ipod_touch_timer_post_load(&s,1)==-EINVAL);
 s.next_planned_tick=0;assert(!ipod_touch_timer_post_load(&s,1));
 puts("PASS: timer dilation, maximum guest count, deadline saturation and snapshot validation");
}
'''
with tempfile.TemporaryDirectory(prefix='it-timer-check-') as tmp:
    tmp=Path(tmp);(tmp/'check.c').write_text(code)
    subprocess.run(['cc','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check')],check=True)
    result=subprocess.run([str(tmp/'check')],check=True,capture_output=True,text=True)
    assert len(result.stderr.splitlines())==4, result.stderr
    for n in range(4):
        assert f"UNMODELLED timer {n} (reg 0x{8+n*32:03x}" in result.stderr, result.stderr
    print(result.stdout,end="")
