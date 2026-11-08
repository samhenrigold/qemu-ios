/* A shared-wire BREAK resets the transaction and echoes before command bits.
 * The standard gauge replies retain the configured full, healthy battery.
 *
 * SLICE hw/arm/s5l8930_hdq.c define HDQ_|BQ_|CNTL_
 * SLICE hw/arm/s5l8930_hdq.c range struct HdqGaugeChardev { | DECLARE_INSTANCE_CHECKER
 * SLICE hw/arm/s5l8930_hdq.c fn hdq_queue hdq_read_reg hdq_write_reg hdq_chr_write hdq_chr_ioctl hdq_apply_battery s5l8930_hdq_set_battery hdq_reset s5l8930_hdq_set_capacity
 * CFLAGS -Wall -Werror -Wno-unused-function
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define HDQ_GAUGE_CHARDEV(p) ((HdqGaugeChardev *)(p))
#define QEMU_CLOCK_VIRTUAL 0
#define SCALE_MS 1000000
#define CHR_IOCTL_SERIAL_SET_BREAK 2
#define trace_event_get_state_backends(...) false
#define TRACE_PRINTF(...) ((void)0)
typedef struct {
    int64_t deadline;
    bool armed;
} QEMUTimer;
typedef struct {
    int unused;
} Chardev;
static int64_t qemu_clock_get_ns(int clock)
{
    return 123000000;
}
static void timer_mod(QEMUTimer *t, int64_t n)
{
    t->deadline = n;
    t->armed = true;
}
static void timer_del(QEMUTimer *t)
{
    t->armed = false;
}
static uint16_t lduw_le_p(const void *v)
{
    const uint8_t *p = v;
    return p[0] | p[1] << 8;
}
static void stw_le_p(void *v, uint16_t n)
{
    uint8_t *p = v;
    p[0] = n;
    p[1] = n >> 8;
}
#include "slice.h"
static void read_byte(HdqGaugeChardev *g, uint8_t reg, uint8_t expected)
{
    uint8_t command[8];
    for (unsigned b = 0; b < 8; b++)
        command[b] = reg & (1 << b) ? HDQ_SYM_1 : HDQ_SYM_0;
    g->resp_head = g->resp_tail = 0;
    assert(hdq_chr_write((Chardev *)g, command, 8) == 8);
    assert(g->resp_tail == 16 && !g->nbits);
    assert(!memcmp(g->resp, command, 8));
    uint8_t actual = 0;
    for (unsigned b = 0; b < 8; b++)
        actual |= (g->resp[8 + b] > 0xf8) << b;
    assert(actual == expected);
}
int main(void)
{
    QEMUTimer timer = {0};
    HdqGaugeChardev g = {.timer = &timer};
    s5l8930_hdq_set_capacity((Chardev *)&g, 1440);
    s5l8930_hdq_set_battery((Chardev *)&g, 100, true);
    g.bits = 0x73;
    g.nbits = 7;
    g.resp_tail = 9;
    int on = 1, off = 0;
    assert(hdq_chr_ioctl((Chardev *)&g, CHR_IOCTL_SERIAL_SET_BREAK, &on) == 0);
    assert(!g.bits && !g.nbits && g.resp_tail == 1 && g.resp[0] == 0);
    assert(timer.armed && timer.deadline == qemu_clock_get_ns(0));
    assert(hdq_chr_ioctl((Chardev *)&g, CHR_IOCTL_SERIAL_SET_BREAK, &off) == 0);
    assert(g.resp_tail == 1); /* No duplicate echo on release. */
    assert(hdq_chr_ioctl((Chardev *)&g, 999, &on) == -ENOTSUP);
    read_byte(&g, BQ_SOC, 100);
    assert(lduw_le_p(g.regs + BQ_RM) == 1440 &&
           lduw_le_p(g.regs + BQ_FCC) == 1440);
    assert(lduw_le_p(g.regs + BQ_SOH) == 100 &&
           lduw_le_p(g.regs + BQ_VOLT) == 4200);
    assert(lduw_le_p(g.regs + BQ_FLAGS) ==
           (BQ_FLAG_BAT_DET | BQ_FLAG_CHG | BQ_FLAG_FC));
    hdq_reset(&g);
    read_byte(&g, BQ_SOC, 100); /* Reset retains machine settings. */
    puts(
        "PASS: BREAK reset/echo, symbol framing, full charge, health, voltage, reset retention");
    return 0;
}
