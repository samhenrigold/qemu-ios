/* Original IOP 247c/2438/21a4 countdown programming and acknowledgement.
 * SLICE hw/arm/s5l8950.c fn a6_counter_period_ns a6_iop_timer_expired a6_iop_timer_schedule a6_iop_timer_write a6_iop_timer_ack
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define QEMU_CLOCK_VIRTUAL 0
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define trace_a6_iop_timer_log(...) ((void)0)
typedef uint64_t hwaddr;
typedef struct {
    bool armed;
    int64_t deadline;
} QEMUTimer;
typedef struct {
    void *cpu[2];
    QEMUTimer *iop_timer;
    int64_t iop_timer_deadline;
    uint32_t iop_timer_control, iop_timer_reload, iop_timer_status,
        iop_other_mask;
} S5L8950State;
static int64_t now = 1000000;
static bool irq;
static void timer_del(QEMUTimer *t)
{
    t->armed = false;
}
static void timer_mod(QEMUTimer *t, int64_t n)
{
    t->armed = true;
    t->deadline = n;
}
static int64_t qemu_clock_get_ns(unsigned clock)
{
    return now;
}
static void a6_irq_update(S5L8950State *s)
{
    irq = (s->iop_timer_status & 1) && !(s->iop_other_mask & 2);
}
static unsigned gt_cntfrq_period_ns(void *cpu) { return 41; }
#include "slice.h"
int main(void)
{
    QEMUTimer timer = {0};
    S5L8950State s = {.iop_timer = &timer, .iop_other_mask = UINT32_MAX};
    assert(!a6_iop_timer_write(&s, 0x300c, 4));
    assert(a6_iop_timer_write(&s, 0x2010, 0xe) && !timer.armed);
    a6_iop_timer_write(&s, 0x2020, 2);
    assert(!(s.iop_other_mask & 2) && !irq);
    /* Native callback arm: maximum interval, enable, W1C, final interval.
  * The AP timer represents each nominal 24 MHz tick as 41 ns. */
    a6_iop_timer_write(&s, 0x2014, UINT32_MAX);
    a6_iop_timer_write(&s, 0x2010, 0xf);
    a6_iop_timer_write(&s, 0x2018, 1);
    a6_iop_timer_write(&s, 0x2014, 1200000);
    assert(timer.armed && timer.deadline == now + 49200000 && !irq);
    now = timer.deadline;
    timer.armed = false;
    a6_iop_timer_expired(&s);
    assert(irq && s.iop_timer_status == 1);
    assert(a6_iop_timer_ack(&s) == 0x70001 && !irq && s.iop_timer_status == 1);
    assert(!a6_iop_timer_ack(&s));
    /* Native handler disarms, clears status, then rearms the other IRQ. */
    a6_iop_timer_write(&s, 0x2010, 0xe);
    a6_iop_timer_write(&s, 0x2018, 1);
    a6_iop_timer_write(&s, 0x2020, 2);
    assert(!irq && !timer.armed && !s.iop_timer_status);
    /* Expiry while masked must survive until unmasked. */
    a6_iop_timer_write(&s, 0x201c, 2);
    a6_iop_timer_write(&s, 0x2014, 24);
    a6_iop_timer_write(&s, 0x2010, 0xf);
    assert(timer.deadline == now + 984);
    now = timer.deadline;
    timer.armed = false;
    a6_iop_timer_expired(&s);
    assert(!irq && s.iop_timer_status == 1);
    a6_iop_timer_write(&s, 0x2020, 2);
    assert(irq);
    a6_iop_timer_write(&s, 0x2010, 0xe);
    a6_iop_timer_write(&s, 0x2018, 1);
    assert(!irq);
    /* Sub-ns rounding and zero intervals cannot create an early or looping callback. */
    a6_iop_timer_write(&s, 0x2014, 1);
    a6_iop_timer_write(&s, 0x2010, 0xf);
    assert(timer.deadline == now + 41);
    a6_iop_timer_write(&s, 0x2014, 0);
    assert(timer.deadline == now + 1);
    a6_iop_timer_write(&s, 0x2010, 0xe);
    assert(!timer.armed);
    puts(
        "PASS: native 24 MHz IOP timer, 70001 ACK/mask, W1C, cancellation, masked expiry and interval rounding");
}
