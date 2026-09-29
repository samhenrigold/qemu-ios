#!/usr/bin/env python3
"""Dialog D1815 PMU (the iPad's PMU, hw/arm/s5l8930_i2c.c): the restart/power-off
split, the ADC start-bit and mux-6 cable classification.

Recent iPad fixes this pins:
  - 0x7b = 0x0b is AppleD1815PMU's restart. With iBoot's boot-flags (0x8F)
    stay-off pattern set (& 0xd0 == 0x90 or 0x10) it is a power-off (shutdown +
    guest_shutdown_confirmed), otherwise a reboot (reset request). (smoke #28/#39)
  - An ADC conversion clears its start bit (0x30 bit4) when done; iBoot polls it.
  - mux 6 is the dock D+/D- the charger biased: with a host's pull-downs it
    reads 0 mV (USBHost), otherwise mid-scale 0x800 (a brick, "Detached").
    mV = adc * 5000 / 4096. (4.3.x cable detection, smoke #35)

Mutation (named, must fail this test): in d1815_send's PMU_SYS_CTRL case, drop
the boot-flags guard so restart always reboots, i.e. replace
    if (data == PMU_SYS_RESTART && PMU_BOOT_FLAGS_OFF(s->regs[PMU_BOOT_FLAGS])) {
with
    if (0) {
so a stay-off restart reboots instead of powering off.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/s5l8930_i2c.c').read_text()
constants = '\n'.join(re.findall(r'^#define (?:PMU_\w+|D1815_ADDR).*$', source, re.M))
state = re.search(r'struct S5L8930D1815State \{.*?\n\};', source, re.S).group()

funcs = []
for name in ('d1815_update_irq', 'd1815_adc_done', 's5l8930_d1815_button',
             's5l8930_d1815_set_usb_host', 's5l8930_d1815_set_vbat',
             's5l8930_d1815_usb_cable_event', 'd1815_rtc_count', 'd1815_recv',
             'd1815_send', 'd1815_reset', 's5l8930_d1815_guest_shutdown_confirmed',
             'd1815_post_load'):
    m = re.search(r'^(?:static |bool |void |int )[^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert m, name
    funcs.append(m.group())

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct { int unused; } I2CSlave;
typedef struct { bool pending; int64_t deadline; } QEMUTimer;
typedef int *qemu_irq;
typedef struct S5L8930D1815State S5L8930D1815State;
typedef S5L8930D1815State DeviceState;
#define S5L8930_D1815(s) ((S5L8930D1815State *)(s))
#define QEMU_CLOCK_VIRTUAL 0
#define SCALE_MS 1000000LL
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 1
#define SHUTDOWN_CAUSE_GUEST_RESET 2
#define qatomic_read(p) (*(p))
#define qatomic_set(p,v) (*(p)=(v))
static int64_t now;
static int shutdowns, resets;
static int d1815_shutdown_confirmed;
static int64_t qemu_clock_get_ns(int c) { return now; }
static void qemu_set_irq(int *irq, int v) { if (irq) *irq = !!v; }
static void timer_mod(QEMUTimer *t, int64_t at) { t->pending = true; t->deadline = at; }
static void timer_del(QEMUTimer *t) { t->pending = false; }
static void qemu_system_shutdown_request(int cause) { shutdowns++; }
static void qemu_system_reset_request(int cause) { resets++; }
static uint32_t ldl_le_p(const void *p) {
    const uint8_t *b = p;
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}
'''

tests = r'''
static void wr(S5L8930D1815State *s, uint8_t reg, uint8_t val) {
    s->addressing = true;
    d1815_send((I2CSlave *)s, reg);
    d1815_send((I2CSlave *)s, val);
}
static unsigned rd(S5L8930D1815State *s, uint8_t reg) {
    s->reg = reg;
    return d1815_recv((I2CSlave *)s);
}
static void advance(S5L8930D1815State *s) {
    assert(s->adc_timer->pending);
    now = s->adc_timer->deadline;
    s->adc_timer->pending = false;
    d1815_adc_done(s);
}
int main(void) {
    int irq = 0;
    QEMUTimer timer = {0};
    S5L8930D1815State s = { .irq = &irq, .adc_timer = &timer };
    d1815_reset((DeviceState *)&s);

    /* Reset leaves everything masked, the battery SWI line high, no events. */
    assert(s.regs[PMU_STATUS_C] == PMU_GPIO6_BATT_SWI && !irq);

    /* --- ADC: start bit clears on completion, mux 4 = battery voltage --- */
    wr(&s, PMU_ADC_CTRL, PMU_ADC_MUX_VBAT | PMU_ADC_START);  /* 0x14 */
    assert(timer.pending);
    assert(s.regs[PMU_ADC_CTRL] & PMU_ADC_START);            /* still converting */
    advance(&s);
    assert(!(s.regs[PMU_ADC_CTRL] & PMU_ADC_START));         /* start bit cleared */
    /* default vbat 3900 mV -> adc = (3900-2500)*4096/2000 = 2867 = 0xB33 */
    unsigned adc = (s.regs[PMU_ADC_RES] & 0xf) | (s.regs[PMU_ADC_RES + 1] << 4);
    assert(adc == 2867);
    assert(s.regs[PMU_EVENT + 1] & PMU_EVENT_B_ADC);         /* completion is an event */

    /* --- mux 6 cable: host pull-downs read 0 mV (USBHost); else mid-scale --- */
    s5l8930_d1815_set_usb_host((DeviceState *)&s, true);
    wr(&s, PMU_ADC_CTRL, PMU_ADC_MUX_BRICK | PMU_ADC_START); /* 0x16 */
    advance(&s);
    unsigned brick = (s.regs[PMU_ADC_RES] & 0xf) | (s.regs[PMU_ADC_RES + 1] << 4);
    assert(brick == 0);                                      /* host: 0 mV both lines */

    s5l8930_d1815_set_usb_host((DeviceState *)&s, false);
    wr(&s, PMU_ADC_CTRL, PMU_ADC_MUX_BRICK | PMU_ADC_START);
    advance(&s);
    brick = (s.regs[PMU_ADC_RES] & 0xf) | (s.regs[PMU_ADC_RES + 1] << 4);
    assert(brick == 0x800);                                  /* no host: mid-scale (brick / Detached) */

    /* --- restart with no stay-off flag is a reboot --- */
    assert(!resets && !shutdowns);
    s.regs[PMU_BOOT_FLAGS] = 0x00;
    wr(&s, PMU_SYS_CTRL, PMU_SYS_RESTART);                   /* 0x7b <- 0x0b */
    assert(resets == 1 && shutdowns == 0 && !s5l8930_d1815_guest_shutdown_confirmed());

    /* --- restart with iBoot's stay-off flag is a power-off, not a reboot --- */
    s.regs[PMU_BOOT_FLAGS] = 0x90;                           /* & 0xd0 == 0x90 */
    wr(&s, PMU_SYS_CTRL, PMU_SYS_RESTART);
    assert(shutdowns == 1 && resets == 1);                   /* no new reboot */
    assert(s5l8930_d1815_guest_shutdown_confirmed());

    /* A non-restart write to 0x7b just stores, no reboot/shutdown. */
    wr(&s, PMU_SYS_CTRL, 0x0f);
    assert(resets == 1 && shutdowns == 1);

    puts("PASS: D1815 restart/power-off split on boot flags, ADC start-bit clear, mux-6 cable classification");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + constants + '\n' + state + '\n' + '\n'.join(funcs) + tests)
    binary = str(Path(tmp) / 'check')
    subprocess.run(['clang', '-std=gnu11', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
