/* Exercise the real PMU write handler without a guest or QEMU build.
 *
 * SLICE:pmu include/hw/arm/ipod_touch_pcf50633_pmu.h define PMU_
 * SLICE:pmu include/hw/arm/ipod_touch_pcf50633_pmu.h typedef Pcf50633State
 * SLICE hw/arm/ipod_touch_pcf50633_pmu.c fn pmu_event_count pmu_mask_base pmu_update_backlight pcf50633_guest_shutdown_confirmed pcf50633_guest_shutdown pcf50633_send
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
#define PCF50633(s) ((Pcf50633State *)(s))
#define qatomic_read(p) (*(p))
#define qatomic_set(p, value) (*(p) = (value))
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 1
static bool guest_shutdown_confirmed;
static int shutdowns;
#include "pmu.h"
static bool pmu_trace(void) { return false; }
static void pmu_update_irq(Pcf50633State *s) {}
static void pmu_adc_command(Pcf50633State *s, uint8_t value) {}
static void pmu_trace_access(const char *what, uint8_t reg, uint8_t val) {}
static void lcd_changebrightness(uint8_t val) {}
static void qemu_system_shutdown_request(int cause) {
    assert(cause == SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    assert(guest_shutdown_confirmed);
    shutdowns++;
}
#include "slice.h"

static void write_reg(Pcf50633State *s, uint8_t reg, uint8_t value) {
    s->addressing = true;
    pcf50633_send((I2CSlave *)s, reg);
    pcf50633_send((I2CSlave *)s, value);
}
int main(void) {
    Pcf50633State s = {0};
    s.shutdown_reg = PMU_SHUTDOWN_REG;
    /* Native guest standby must work without a host-side arming flag. */
    write_reg(&s, PMU_STANDBY_CMD, 0);
    assert(!shutdowns);
    write_reg(&s, PMU_STANDBY_CMD, PMU_STANDBY_GO);
    assert(shutdowns == 1 && pcf50633_guest_shutdown_confirmed());
    assert(!s.shutdown_armed);
    guest_shutdown_confirmed = false;
    memset(&s, 0, sizeof(s));
    s.shutdown_reg = PMU_SHUTDOWN_REG;
    /* 5F138 clears bit 6 during idle sleep too; wait for the final command. */
    write_reg(&s, 0x10, 0x7f);
    write_reg(&s, 0x10, 0x5f);
    assert(shutdowns == 1 && !pcf50633_guest_shutdown_confirmed());
    write_reg(&s, 0x10, 0x3f);
    assert(shutdowns == 1 && !pcf50633_guest_shutdown_confirmed());
    write_reg(&s, PMU_STANDBY_CMD, 0x80);
    assert(shutdowns == 1 && !pcf50633_guest_shutdown_confirmed());
    write_reg(&s, PMU_STANDBY_CMD, PMU_STANDBY_GO);
    assert(shutdowns == 2 && pcf50633_guest_shutdown_confirmed());
    guest_shutdown_confirmed = false;
    write_reg(&s, PMU_SHUTDOWN_REG, 0x10);
    assert(shutdowns == 2 && !pcf50633_guest_shutdown_confirmed());
    write_reg(&s, PMU_SHUTDOWN_REG, 0x11);
    assert(shutdowns == 3 && pcf50633_guest_shutdown_confirmed());
    puts("PMU guest shutdown checks passed");
}
