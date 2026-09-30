#!/usr/bin/env python3
"""The D1815's scratch bank (0x80-0x9F: the OS's boot reason at 0x8F, PMURTC's offset at 0x84) and its RTC
survive a restart; only the PMU's own power-on clears them. 5.x's halt with the cable attached writes 0x8F = 0x90
and restarts, and iBoot-1219 reads the reason back to run its power-off simulation instead of autobooting iOS
(LightTouchMac smoke #28). 4.x's halt makes the same choice from STATUS A bit 3, VBUS (8L1 809f47b8: STATUS A-E
read at 809f4384, bit 3 set -> 0x8F = 0x90 and restart, clear -> "pmu go stdby"; ledger #55a), so STATUS A must follow
the cable. Compiles the model's register file and reset from hw/arm/s5l8930_i2c.c."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
src = (root / 'hw/arm/s5l8930_i2c.c').read_text()
body = src[src.index('OBJECT_DECLARE_SIMPLE_TYPE(S5L8930D1815State'):src.index('static void d1815_init(')]
body = body.replace('OBJECT_DECLARE_SIMPLE_TYPE(S5L8930D1815State, S5L8930_D1815)', 'typedef struct S5L8930D1815State S5L8930D1815State;')
pre = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
typedef struct { int address; } I2CSlave;
typedef void DeviceState;
typedef int *qemu_irq;
typedef int QEMUTimer;
enum i2c_event { I2C_START_SEND, I2C_START_RECV, I2C_FINISH };
#define S5L8930_D1815(o) ((S5L8930D1815State *)(o))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SCALE_MS 1000000
#define QEMU_CLOCK_VIRTUAL 0
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 1
#define SHUTDOWN_CAUSE_GUEST_RESET 2
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
static int shutdowns, resets;
static void qemu_system_shutdown_request(int c) { shutdowns++; }
static void qemu_system_reset_request(int c) { resets++; }
static void qemu_set_irq(qemu_irq i, int l) {}
static void timer_del(QEMUTimer *t) {}
static void timer_mod(QEMUTimer *t, long long ns) {}
static long long qemu_clock_get_ns(int c) { return 0; }
static uint32_t ldl_le_p(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
'''
test = r'''
static void wr(S5L8930D1815State *s, uint8_t reg, uint8_t v)
{ d1815_event(&s->i2c, I2C_START_SEND); d1815_send(&s->i2c, reg); d1815_send(&s->i2c, v); }
static uint8_t rd(S5L8930D1815State *s, uint8_t reg)
{ d1815_event(&s->i2c, I2C_START_SEND); d1815_send(&s->i2c, reg); return d1815_recv(&s->i2c); }
int main(void)
{
    static S5L8930D1815State s;
    memset(s.regs, 0x5a, sizeof(s.regs));
    s.rtc_base = 12345;
    d1815_reset(&s);                                  /* the PMU's own power-on */
    assert(rd(&s, 0x8f) == 0 && rd(&s, 0x84) == 0 && s.rtc_base == 0);
    /* 8L1's halt decision: VBUS from STATUS A bit 3, read with STATUS B-E in one 5-byte burst (809f4384). */
    d1815_event(&s.i2c, I2C_START_SEND); d1815_send(&s.i2c, 0x07);
    assert(!(d1815_recv(&s.i2c) & 8) && rd(&s, 0x07) == 0); /* no cable: standby */
    s5l8930_d1815_set_usb_host(&s, true);
    d1815_event(&s.i2c, I2C_START_SEND); d1815_send(&s.i2c, 0x07);
    assert(d1815_recv(&s.i2c) & 8);                   /* cable: restart into iBoot's wait */
    assert(rd(&s, 0x07) == 8 && rd(&s, 0x09) == 0x20); /* only VBUS; STATUS C keeps the battery SWI */
    d1815_reset(&s); assert(rd(&s, 0x07) & 8);        /* a level, not a latch: survives reset */
    s5l8930_d1815_set_usb_host(&s, false);
    assert(rd(&s, 0x07) == 0);
    wr(&s, 0x84, 0x77);                               /* PMURTC's offset */
    wr(&s, 0x46, 0x10); wr(&s, 0x47, 0); wr(&s, 0x48, 0); wr(&s, 0x49, 0); wr(&s, 0x4a, 0x41);
    int64_t base = s.rtc_base;
    wr(&s, 0x0c, 0x00); wr(&s, 0x30, 0x04);           /* ordinary registers */
    wr(&s, 0x8f, 0x90);                               /* 9B206 halt: the reason, then the restart */
    wr(&s, 0x7b, 0x0b);
    /* The OS has halted (volume unmounted): confirmed, and QEMU restarts rather than exits. */
    assert(resets == 1 && shutdowns == 0 && s5l8930_d1815_guest_shutdown_confirmed());
    d1815_reset(&s);                                  /* the restart the guest asked for */
    assert(rd(&s, 0x8f) == 0x90);                     /* what iBoot-1219 reads at 5ff07fd0 */
    assert(s5l8930_d1815_guest_shutdown_confirmed()); /* off, in the power-off simulation */
    assert(rd(&s, 0x84) == 0x77 && s.rtc_base == base);
    assert(rd(&s, 0x0c) == 0xff && rd(&s, 0x30) == 0);/* outside the bank: power-on values */
    wr(&s, 0x8f, 0x00);                               /* iBoot consumes it (5ff073b4, 5ff07f92) */
    assert(!s5l8930_d1815_guest_shutdown_confirmed()); /* the power button: iOS boots */
    wr(&s, 0x7b, 0x0b); d1815_reset(&s);              /* a plain restart confirms nothing */
    assert(resets == 2 && !s5l8930_d1815_guest_shutdown_confirmed());
    wr(&s, 0x12, 0x01);                               /* standby (5ff075e0 after an unplug) */
    assert(shutdowns == 1 && s5l8930_d1815_guest_shutdown_confirmed());
    /* Halted again, then the host resets (the app's Power On, QMP system_reset): the PMU's power-on. The
     * app waits for the latch to drop before resuming, and iBoot must boot iOS, not wait again. */
    wr(&s, 0x8f, 0x90); wr(&s, 0x7b, 0x0b); d1815_reset(&s);
    assert(s5l8930_d1815_guest_shutdown_confirmed());
    d1815_reset(&s);
    assert(!s5l8930_d1815_guest_shutdown_confirmed() && rd(&s, 0x8f) == 0 && rd(&s, 0x84) == 0);
    puts("ok");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c, exe = Path(d) / 't.c', Path(d) / 't'
    c.write_text(pre + body + test)
    subprocess.run(['cc', '-w', '-o', exe, c], check=True)
    out = subprocess.run([exe], capture_output=True, text=True)
    assert out.returncode == 0 and out.stdout.strip() == 'ok', out.stdout + out.stderr
print('PASS test_pmu_scratch')
