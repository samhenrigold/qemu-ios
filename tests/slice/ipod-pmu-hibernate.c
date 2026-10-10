/* The D1759 hibernate: the power command's bit 1 turns the AP off, a wake button or an unmasked event turns it
 * on with EVENT_B bit 7 latched (7E18's LLB resumes only with it), and the PMU keeps its registers across the AP's
 * power-on reset; the D1755 the same with its own registers. A board that does not wire "ap-power" keeps the old behavior: the command is only stored.
 *
 * SLICE include/hw/arm/ipod_touch_pcf50633_pmu.h define PMU_
 * SLICE include/hw/arm/ipod_touch_pcf50633_pmu.h typedef Pcf50633State
 * SLICE hw/arm/ipod_touch_pcf50633_pmu.c fn pmu_event_base pmu_event_count pmu_mask_base pmu_set_ap_power pmu_update_irq pmu_latch_event pcf50633_latch_wake_event pmu_update_backlight pcf50633_guest_shutdown_confirmed pcf50633_guest_shutdown pcf50633_send pcf50633_update_battery pcf50633_reset
 */
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { int unused; } I2CSlave;
typedef struct { int unused; } QEMUTimer;
typedef int *qemu_irq;
typedef struct Pcf50633State DeviceState;
typedef struct Pcf50633State Pcf50633State;
#define PCF50633(s) ((Pcf50633State *)(s))
#define qatomic_read(p) (*(p))
#define qatomic_set(p, value) (*(p) = (value))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define QEMU_CLOCK_VIRTUAL 0
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 1
static bool guest_shutdown_confirmed;
static int shutdowns;
static void qemu_set_irq(int *irq, int level) { if (irq) *irq = level; }
static int64_t qemu_clock_get_ns(int clock) { return 0; }
static void timer_del(QEMUTimer *t) {}
static bool pmu_trace(void) { return false; }
static void pmu_trace_access(const char *what, uint8_t reg, uint8_t val) {}
static void pmu_adc_command(Pcf50633State *s, uint8_t value) {}
static void pmu_apply_battery_adc(Pcf50633State *s, unsigned counts) {}
static unsigned pcf50633_adc_for_level(unsigned percent) { return 0; }
static void lcd_changebrightness(uint8_t val) {}
static void ios_backlight_register(int (*level)(void *), void *opaque) {}
static int pmu_backlight_level(void *opaque) { return 0; }
static void qemu_system_shutdown_request(int cause) { shutdowns++; }
#include "slice.h"

static void wr(Pcf50633State *s, uint8_t reg, uint8_t value) {
    s->addressing = true;
    pcf50633_send((I2CSlave *)s, reg);
    pcf50633_send((I2CSlave *)s, value);
}

/* 7E18's sleep entry: wake masks, the marker, then 0x0a = 0x0a. */
static void hibernate(Pcf50633State *s) {
    wr(s, 0x07, 0x95); wr(s, 0x08, 0xdf); wr(s, 0x09, 0xab);
    wr(s, PMU_STANDBY_CMD, 0x80);
    wr(s, PMU_SHUTDOWN_REG, 0x0a);
}

int main(void) {
    int irq = 0, ap = 1;
    Pcf50633State s = { .irq = &irq, .ap_power = &ap, .shutdown_reg = PMU_SHUTDOWN_REG, .event_count = 3,
                        .wake_event_reg = PMU_EVENT_C_REG,
                        .backlight_enable_reg = PMU_LDO_ENABLE, .adc_reg = PMU_ADC_CONTROL };
    pcf50633_reset(&s);

    hibernate(&s);
    assert(!ap && s.ap_off && !shutdowns);
    assert(s.regs[PMU_SHUTDOWN_REG] == 0x08);          /* the command is consumed, the rest kept */
    /* A masked event (EVENT_A bit 0) does not wake; Home does, masked as it is (0x09 = 0xab). */
    s.regs[PMU_EVENT_A_REG] |= 0x01; pmu_update_irq(&s);
    assert(!ap);
    pcf50633_latch_wake_event(&s, PMU_STAT_MENU);
    assert(ap && !s.ap_off && s.ap_waking);
    assert(s.regs[PMU_EVENT_A_REG + 1] & PMU_EVENT_B_HIB_WAKE);
    /* The AP's power-on reset leaves the PMU alone: LLB reads the marker and the events. */
    pcf50633_reset(&s);
    assert(!s.ap_waking && s.regs[PMU_STANDBY_CMD] == 0x80);
    assert(s.regs[PMU_EVENT_C_REG] & PMU_STAT_MENU && s.regs[0x09] == 0xab);
    /* A cold reset afterwards is the PMU's own power-on. */
    pcf50633_reset(&s);
    assert(!s.regs[PMU_STANDBY_CMD] && !s.regs[PMU_EVENT_C_REG] && s.regs[0x09] == 0xff);

    /* An event the guest left unmasked wakes too (EVENT_A bit 3, the cable). */
    hibernate(&s);
    assert(!ap);
    s.regs[PMU_EVENT_A_REG] |= 0x08; pmu_update_irq(&s);
    assert(ap && s.ap_waking);
    pcf50633_reset(&s);

    /* The D1755 (S5L8920 boards): 4.x's "pmu go hib" sets 0x26 in 0x0d after 0x6f = 0x80; Hold latches
     * event byte 0x01 bit 1 (DT wake_button_hold 'STAT' 0x181), not the D1759's EVENT_C. */
    Pcf50633State d = { .irq = &irq, .ap_power = &ap, .shutdown_reg = 0x0d, .event_count = 4,
                        .wake_event_reg = 0x01, .backlight_enable_reg = 0xfe, .adc_reg = 0x30 };
    pcf50633_reset(&d);
    wr(&d, 0x09, 0xff); wr(&d, 0x0a, 0xff); wr(&d, 0x0b, 0xff); wr(&d, 0x0c, 0xff);
    wr(&d, PMU_STANDBY_CMD, 0x80);
    wr(&d, 0x0d, 0x26);
    assert(!ap && d.ap_off && !shutdowns && d.regs[0x0d] == 0x24);
    pcf50633_latch_wake_event(&d, 0x02);
    assert(ap && d.ap_waking && d.regs[0x01] == 0x02 && !(d.regs[PMU_EVENT_C_REG] & 0x02));
    pcf50633_reset(&d);
    assert(d.regs[PMU_STANDBY_CMD] == 0x80 && d.regs[0x01] == 0x02);

    /* Unwired (the 1G): the command is stored and nothing switches. */
    Pcf50633State u = { .irq = &irq, .shutdown_reg = PMU_SHUTDOWN_REG, .event_count = 3,
                        .backlight_enable_reg = PMU_LDO_ENABLE, .adc_reg = PMU_ADC_CONTROL };
    pcf50633_reset(&u);
    hibernate(&u);
    assert(!u.ap_off && u.regs[PMU_SHUTDOWN_REG] == 0x0a);
    pcf50633_latch_wake_event(&u, PMU_STAT_MENU);
    assert(!u.ap_waking && !(u.regs[PMU_EVENT_A_REG + 1] & PMU_EVENT_B_HIB_WAKE));
    puts("PMU hibernate checks passed");
}
