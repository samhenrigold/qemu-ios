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
typedef int SysBusDevice, MemoryRegion, QEMUTimer, Clock, qemu_irq;
#define MIN(a,b) ((a)<(b)?(a):(b))
#define NANOSECONDS_PER_SECOND 1000000000
#define QEMU_CLOCK_VIRTUAL 0
static int64_t now, deadline;
static bool timer_trace(void) { return false; }
static int64_t qemu_clock_get_ns(int clock) { return now; }
static void timer_mod(QEMUTimer *timer, int64_t value) { assert(value>=now);deadline=value; }
static uint64_t muldiv64(uint64_t a,uint64_t b,uint64_t c) { return (unsigned __int128)a*b/c; }
'''
code += '\n'.join(re.findall(r'^#define (?:TIMER_|IT_TIMER_).*$',header,re.M))+'\n'+state+'\n'
for name in ['s5l8900_st_update','s5l8900_st_set_timer','ipod_touch_timer_post_load']:
    code += re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)[0]+'\n'
code += r'''
int main(void) {
 IPodTouchTimerState s={.dilation=1};
 s5l8900_st_update(&s);assert(s.tick_interval==100000);
 s.bcount1=10000;s.dilation=2;s5l8900_st_update(&s);assert(s.tick_interval==2000000);
 now=123;s5l8900_st_set_timer(&s);assert(deadline==2000000);
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
    subprocess.run([str(tmp/'check')],check=True)
