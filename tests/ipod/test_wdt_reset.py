#!/usr/bin/env python3
"""Normal watchdog kicks must not be mistaken for immediate resets."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_wdt.c').read_text()
header = (root / 'include/hw/arm/ipod_touch_wdt.h').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t hwaddr;
typedef struct { uint32_t ctrl, cnt; } IPodTouchWDTState;
typedef struct { struct { uint32_t regs[16]; } env; } ARMCPU;
#define ARM_CPU(p) ((ARMCPU *)(p))
#define SHUTDOWN_CAUSE_GUEST_RESET 1
static void *current_cpu;
static int resets;
static bool inhibited;
static bool wdt_trace(void) { return false; }
static bool wdt_noreset(void) { return inhibited; }
static void qemu_system_reset_request(int cause) {
    assert(cause == SHUTDOWN_CAUSE_GUEST_RESET);
    resets++;
}
'''
code += '\n'.join(re.findall(r'^#define WDT_.*$', header, re.M)) + '\n'
code += re.search(r'^static void ipod_touch_wdt_write\(.*?^}', source,
                  re.M | re.S).group() + '\n'
code += r'''
int main(void) {
    IPodTouchWDTState s = {0};
    ipod_touch_wdt_write(&s, WDT_CTRL, 0x001f4a00, 4);
    assert(s.ctrl == 0x001f4a00 && !resets);
    ipod_touch_wdt_write(&s, WDT_CTRL, 0, 4);
    assert(!s.ctrl && !resets);
    ipod_touch_wdt_write(&s, WDT_CNT, 0x100000, 4);
    assert(s.cnt == 0x100000 && !resets);
    ipod_touch_wdt_write(&s, WDT_CTRL, 0x100000, 4);
    assert(resets == 1);
    inhibited = true;
    ipod_touch_wdt_write(&s, WDT_CTRL, 0x100000, 4);
    assert(resets == 1);
    puts("Watchdog reset command checks passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wdt-check-') as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', str(c), '-o', str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
