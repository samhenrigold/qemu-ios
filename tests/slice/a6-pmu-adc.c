/* Native D1972 ADC completion must survive masking until the event is read.
 * Reading a result must preserve its paired byte and must not acknowledge
 * the event; cancelling or resetting a conversion must stop its timer.
 *
 * SLICE hw/arm/s5l8950.c fn a6_pmu_cancel_resume a6_pmu_set_ap_power a6_pmu_irq_update a6_pmu_take_suspend a6_pmu_button a6_pmu_adc_complete a6_pmu_event a6_pmu_send a6_pmu_recv a6_pmu_reset
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct QEMUTimer {
    bool armed;
    int64_t deadline;
} QEMUTimer;
typedef struct A6PMU {
    uint8_t regs[256], reg;
    bool address, adc_probe, thermal_sensors, battery_swi, usb_power;
    uint16_t adc_input;
    uint32_t rtc_latch;
    int irq, ap_power, gpio[12];
    unsigned log_count;
    bool ap_off, ap_waking;
    QEMUTimer *adc_timer;
} A6PMU;
typedef A6PMU I2CSlave, DeviceState;
enum i2c_event { I2C_START_SEND };
#define QEMU_CLOCK_VIRTUAL 0
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 1
static unsigned shutdown_requests;
#define SHUTDOWN_CAUSE_GUEST_RESET 2
static unsigned reset_requests;
static void qemu_system_reset_request(int cause)
{
    assert(cause == SHUTDOWN_CAUSE_GUEST_RESET);
    reset_requests++;
}

static void qemu_system_shutdown_request(int cause)
{
    assert(cause == SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    shutdown_requests++;
}
#define qemu_log_mask(...) ((void)0)
#define trace_a6_pmu_register(...) ((void)0)
#define trace_a6_pmu_gpio(...) ((void)0)
static int irq_level, rail_level, rail_events, gpio_levels[12];
static void qemu_set_irq(int irq, int value)
{
    if (irq >= 100 && irq < 112) {
        gpio_levels[irq - 100] = value;
    } else if (irq == 2) {
        rail_level = value;
        rail_events++;
    } else {
        irq_level = value;
    }
}
static int64_t qemu_clock_get_ns(int clock)
{
    (void)clock;
    return 123;
}
static void timer_mod(QEMUTimer *t, int64_t deadline)
{
    t->armed = true;
    t->deadline = deadline;
}
static void timer_del(QEMUTimer *t)
{
    t->armed = false;
}
static uint32_t rtc_seconds;
static uint32_t a6_pmu_rtc_clock(void)
{
    return rtc_seconds;
}
#include "slice.h"
static void address(A6PMU *s, uint8_t reg)
{
    a6_pmu_event(s, I2C_START_SEND);
    a6_pmu_send(s, reg);
}
static void write_reg(A6PMU *s, uint8_t reg, uint8_t value)
{
    address(s, reg);
    a6_pmu_send(s, value);
}
int main(void)
{
    QEMUTimer timer = {0};
    A6PMU s = {.adc_probe = true, .adc_input = 0xabc, .adc_timer = &timer};
    a6_pmu_reset(&s);
    assert(irq_level == 1);
    for (unsigned i = 0; i < 12; i++)
        s.gpio[i] = 100 + i;
    write_reg(&s, 0x72, 0);
    assert(!gpio_levels[10]);
    write_reg(&s, 0x72, 2);
    assert(gpio_levels[10]);
    write_reg(&s, 0x72, 0x60);
    assert(gpio_levels[10]);
    write_reg(&s, 0x72, 0);
    assert(!gpio_levels[10]);
    write_reg(&s, 0x48, 0x2e);
    assert(timer.armed && timer.deadline > 123);
    timer.armed = false;
    a6_pmu_adc_complete(&s);
    assert(irq_level == 1); /* completion retained while masked */
    address(&s, 0x49);
    unsigned low = a6_pmu_recv(&s), high = a6_pmu_recv(&s);
    assert(((high << 4) | low) == 0xabc);
    assert(s.regs[2] == 0x20 && !(s.regs[0x48] & 0x20));
    write_reg(&s, 0x13, 0xdf);
    assert(irq_level == 0);
    address(&s, 2);
    assert(a6_pmu_recv(&s) == 0x20 && irq_level == 1);
    address(&s, 2);
    assert(a6_pmu_recv(&s) == 0);
    write_reg(&s, 0x48, 0x22);
    assert(timer.armed);
    write_reg(&s, 0x48, 2);
    assert(!timer.armed);
    write_reg(&s, 0x48, 0x22);
    a6_pmu_reset(&s);
    assert(!timer.armed && irq_level == 1 && !s.regs[2]);
    /* Button latches and masks are PMU functionality, not an ADC probe. */
    s.adc_probe = false;
    a6_pmu_button(&s, false);
    a6_pmu_button(&s, true);
    assert(s.regs[1] == 3 && irq_level == 1);
    write_reg(&s, 0x12, 0xfd); /* Hold unmasked, Home remains masked */
    assert(irq_level == 0);
    address(&s, 1);
    assert(a6_pmu_recv(&s) == 3 && irq_level == 1);
    a6_pmu_button(&s, false);
    assert(irq_level == 1);
    write_reg(&s, 0x12, 0xfc);
    assert(irq_level == 0);
    address(&s, 1);
    assert(a6_pmu_recv(&s) == 1 && irq_level == 1);
    s.ap_power = 2;
    write_reg(&s, 0x12, 0xff); /* masks do not prevent physical button wake */
    write_reg(&s, 0xb0, 0x80);
    write_reg(&s, 0x1b, 2);
    assert(s.ap_off && !s.ap_waking && rail_level == 0 && rail_events == 1 &&
           !(s.regs[0x1b] & 2));
    a6_pmu_button(&s, true);
    assert(!s.ap_off && s.ap_waking && rail_level == 1 && rail_events == 2);
    a6_pmu_reset(&s);
    assert(!s.ap_waking && s.regs[1] == 2 && s.regs[0xb0] == 0x80);
    assert(a6_pmu_take_suspend(&s) && s.regs[0xb0] == 0x40);
    assert(!a6_pmu_take_suspend(&s));
    a6_pmu_reset(&s);
    assert(s.regs[0xb0] == 0 && !s.regs[1]);
    /* An unmasked PMU interrupt can also restore AP power. */
    write_reg(&s, 0x12, 0xfe);
    write_reg(&s, 0x1b, 2);
    assert(s.ap_off);
    s.regs[1] = 1;
    a6_pmu_irq_update(&s);
    assert(s.ap_waking && !s.ap_off && rail_level == 1);
    /* Advancing the RTC between byte reads must not tear a word. */
    rtc_seconds = 0x12ffffff;
    address(&s, 0x64);
    unsigned word = a6_pmu_recv(&s);
    rtc_seconds++;
    for (unsigned i = 1; i < 4; i++)
        word |= (unsigned)a6_pmu_recv(&s) << (i * 8);
    assert(word == 0x12ffffff);
    address(&s, 0x64);
    word = 0;
    for (unsigned i = 0; i < 4; i++)
        word |= (unsigned)a6_pmu_recv(&s) << (i * 8);
    assert(word == 0x13000000);
    write_reg(&s, 0xa5, 0x42);
    write_reg(&s, 0x1b, 2);
    a6_pmu_button(&s, false);
    a6_pmu_reset(&s);
    address(&s, 0xa5);
    assert(a6_pmu_recv(&s) == 0x42);
    /* Hibernate above retains the machine. Standby instead requests normal
     * guest shutdown after flash sync; it must consume the command once. */
    a6_pmu_cancel_resume(&s);
    a6_pmu_reset(&s);
    assert(shutdown_requests == 0);
    write_reg(&s, 0x1b, 1);
    assert(shutdown_requests == 1 && !s.ap_off && !(s.regs[0x1b] & 3) &&
           s.regs[0xb0] == 0);
    assert(!a6_pmu_take_suspend(&s));
    a6_pmu_cancel_resume(&s);
    a6_pmu_reset(&s);
    assert(!s.ap_waking && !s.ap_off && !s.regs[1]);
    /* Original N42 GPIO5 contract: bank0C bit5, control6D input mode60,
     * driven value bit1. A fake always-high answer fails the reset case. */
    s.battery_swi = true;
    a6_pmu_reset(&s);
    assert(s.regs[0x6d] == 0x60);
    s.regs[0x0c] = 0x81;
    address(&s, 0x0c);
    assert(a6_pmu_recv(&s) == 0xa1);
    write_reg(&s, 0x6d, 0);
    address(&s, 0x0c);
    assert(a6_pmu_recv(&s) == 0x81);
    write_reg(&s, 0x6d, 2);
    address(&s, 0x0c);
    assert(a6_pmu_recv(&s) == 0xa1);
    write_reg(&s, 0x6d, 0x60);
    address(&s, 0x0c);
    assert(a6_pmu_recv(&s) == 0xa1);
    assert(s.regs[0x0c] == 0x81); /* GPIO input reads preserve stored bits. */
    s.battery_swi = false;
    address(&s, 0x0c);
    assert(a6_pmu_recv(&s) == 0x81);
    /* Native D1972 STATUS A/B: attached VBUS, active charge phase,
     * valid source selected. Reads retain unrelated status bits. */
    s.usb_power = true;
    s.regs[0x0a] = 0x80;
    s.regs[0x0b] = 0x80;
    s.reg = 0x0a;
    assert(a6_pmu_recv(&s) == 0xc8);
    assert(a6_pmu_recv(&s) == 0x82);
    s.usb_power = false;
    s.reg = 0x0a;
    assert(a6_pmu_recv(&s) == 0x80);
    assert(a6_pmu_recv(&s) == 0x80);
    unsigned before_shutdown = shutdown_requests;
    s.regs[0xb0] = 0x90;
    s.reg = 0x99;
    s.address = false;
    a6_pmu_send(&s, 0x2b);
    assert(shutdown_requests == before_shutdown + 1);
    assert(!reset_requests);
    s.regs[0xb0] = 0;
    s.reg = 0x99;
    a6_pmu_send(&s, 0x2b);
    assert(reset_requests == 1);
    assert(shutdown_requests == before_shutdown + 1);
    /* Calibration and NTC inputs must be independent. Nonthermal voltage
     * channels retain their analog fixture, including when thermal sensing
     * is disabled for an explicitly configured analog experiment. */
    s.thermal_sensors = true;
    for (unsigned channel = 0; channel < 32; channel++) {
        s.regs[0x48] = 0x20 | channel;
        a6_pmu_adc_complete(&s);
        unsigned raw = s.regs[0x49] | (s.regs[0x4a] << 4);
        unsigned expected = channel == 10                    ? 256
                            : channel >= 11 && channel <= 14 ? 800
                                                             : 0xabc;
        assert(raw == expected);
        assert(!(s.regs[0x48] & 0x20));
        assert(s.regs[2] & 0x20);
    }
    s.thermal_sensors = false;
    s.regs[0x48] = 0x2a;
    a6_pmu_adc_complete(&s);
    assert((s.regs[0x49] | (s.regs[0x4a] << 4)) == 0xabc);
    puts(
        "PASS: D1972 ADC, independent thermal inputs, event, power and RTC contracts; battery SWI isolation");
}
