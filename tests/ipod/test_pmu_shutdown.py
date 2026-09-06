#!/usr/bin/env python3
"""Exercise the real PMU write handler without a guest or QEMU build."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "hw/arm/ipod_touch_pcf50633_pmu.c").read_text()
header = (root / "include/hw/arm/ipod_touch_pcf50633_pmu.h").read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    uint32_t curreg, cmd;
    uint8_t regs[256];
    bool addressing, shutdown_armed;
} Pcf50633State;
typedef Pcf50633State I2CSlave;
#define PCF50633(s) (s)
#define qatomic_read(p) (*(p))
#define qatomic_set(p, value) (*(p) = (value))
#define SHUTDOWN_CAUSE_GUEST_SHUTDOWN 1
static bool guest_shutdown_confirmed;
static int shutdowns;
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
'''
code += "\n".join(re.findall(r"^#define PMU_.*$", header, re.M)) + "\n"
for name in ("pcf50633_guest_shutdown_confirmed", "pcf50633_guest_shutdown",
             "pcf50633_send"):
    match = re.search(r"^(?:static )?[^\n]*\b" + name + r"\([^)]*\)[^{]*\{.*?^}",
                      source, re.M | re.S)
    assert match, name
    code += match.group() + "\n"
code += r'''
static void write_reg(Pcf50633State *s, uint8_t reg, uint8_t value) {
    s->addressing = true;
    pcf50633_send(s, reg);
    pcf50633_send(s, value);
}
int main(void) {
    Pcf50633State s = {0};
    /* Native guest standby must work without a host-side arming flag. */
    write_reg(&s, PMU_STANDBY_CMD, 0);
    assert(!shutdowns);
    write_reg(&s, PMU_STANDBY_CMD, PMU_STANDBY_GO);
    assert(shutdowns == 1 && pcf50633_guest_shutdown_confirmed());
    assert(!s.shutdown_armed);
    guest_shutdown_confirmed = false;
    memset(&s, 0, sizeof(s));
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
'''
with tempfile.TemporaryDirectory(prefix="pmu-check-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", str(tmp / "test.c"),
                    "-o", str(tmp / "test")], check=True)
    subprocess.run([str(tmp / "test")], check=True)
