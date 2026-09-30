#!/usr/bin/env python3
"""The PMU RTC as each iPod's own driver reads it, through the model's real pcf50633_recv.

1.x (rtc-bcd, the 1G's PCF50633): ApplePCF50635PMURTC::getCurrentDateTime (3A101a 0xc047bb04, 4B1 the
same) reads seven BCD bytes from 0x59, re-reads 0x59 and retries until the seconds agree, year = 2000 +
RTCYR; AppleARMRTC::convertDateTimeToSeconds makes Unix seconds of it. The date must be the host's UTC
instant, on every read, whatever the host time is (smoke #22: it jumped days per boot).
2.x/3.x (the D1759 default): a 32-bit LE seconds counter at 0x5c, one coherent snapshot per block."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_pcf50633_pmu.c').read_text()
header = (root / 'include/hw/arm/ipod_touch_pcf50633_pmu.h').read_text()
functions = []
for name in ('pmu_update_irq', 'pmu_latch_event', 'pcf50633_adc_for_level', 'pmu_charge_active',
             'pmu_apply_battery_adc', 'pcf50633_update_battery', 'pmu_bcd', 'pmu_bcd_rtc_read', 'pcf50633_recv'):
    m = re.search(r'^(?:static )?[^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert m, name
    functions.append(m.group())
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct {int unused;} I2CSlave;
typedef struct {int unused;} QEMUTimer;
typedef int *qemu_irq;
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define PCF50633(s) ((Pcf50633State *)(s))
#define QEMU_CLOCK_VIRTUAL 0
static time_t host_now;          /* the host clock the model reads */
static int tick_after = -1;      /* the host second ticks after this many bytes of the next block */
#define time(p) (host_now)
static void qemu_set_irq(int *irq, int value) {}
static int64_t qemu_clock_get_ns(int clock) { return 0; }
static bool pmu_trace(void) { return false; }
static void pmu_trace_access(const char *s, uint8_t r, uint8_t v) {}
''' + '\n'.join(re.findall(r'^#define PMU_.*$', header, re.M)) + '\n' \
    + re.search(r'static const uint16_t battery_curve\[\]\[2\] = \{.*?\n};', source, re.S).group() + '\n' \
    + re.search(r'typedef struct Pcf50633State \{.*?\n} Pcf50633State;', header, re.S).group() + '\n' \
    + '\n'.join(functions) + r'''
static Pcf50633State s;
static uint8_t rd(uint8_t reg) { s.curreg = reg; return pcf50633_recv((I2CSlave *)&s); }
static unsigned bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

/* ApplePCF50635PMURTC::getCurrentDateTime + AppleARMRTC::convertDateTimeToSeconds. */
static time_t driver_1x(void)
{
    uint8_t b[7], again;
    int tries = 0;
    do {
        s.curreg = 0x59;
        for (int i = 0; i < 7; i++) {
            b[i] = pcf50633_recv((I2CSlave *)&s);
            if (i + 1 == tick_after) host_now++, tick_after = -1;
        }
        again = rd(0x59);
        assert(++tries < 4);
    } while (again != b[0]);
    struct tm tm = { .tm_sec = bcd(b[0]), .tm_min = bcd(b[1]), .tm_hour = bcd(b[2]), .tm_mday = bcd(b[4]),
                     .tm_mon = bcd(b[5]) - 1, .tm_year = 2000 + bcd(b[6]) - 1900 };
    time_t t = timegm(&tm);
    struct tm check; gmtime_r(&t, &check);
    assert(b[3] == check.tm_wday);   /* RTCWD, 0 = Sunday, copied raw */
    return t;
}

int main(void)
{
    s.rtc_bcd = true;
    /* 2007-09-14 (3A101a), a leap day, the brief's Sep 28 2026, 2099-06-30 23:59:59 (RTCYR is two digits: 2000-2099). */
    const time_t when[] = { 1189728000, 1709164800 + 3723, 1790553600 + 45296, 4086547199 };
    for (unsigned i = 0; i < ARRAY_SIZE(when); i++) {
        host_now = when[i];
        assert(driver_1x() == when[i]);
        host_now += 86400 * 3 + 61;            /* powered off three days: the next boot reads the host */
        assert(driver_1x() == host_now);
    }
    /* The host second ticks inside the seven-byte block: the block stays one instant (RTCSC latch)... */
    host_now = 1790553659; tick_after = 3;              /* 00:00:59 -> 00:01:00 after RTCHR */
    s.curreg = 0x59;
    uint8_t b[7];
    for (int i = 0; i < 7; i++) {
        b[i] = pcf50633_recv((I2CSlave *)&s);
        if (i + 1 == tick_after) host_now++;
    }
    assert(bcd(b[0]) == 59 && bcd(b[1]) == 0 && bcd(b[2]) == 0);
    /* ...and the driver's RTCSC re-read then disagrees, so it retries and gets the new second. */
    host_now = 1790553659; tick_after = 7;
    assert(driver_1x() == 1790553660);
    /* The OS's own offset (0x6b..0x6e) is plain register file, zero until written. */
    assert(rd(0x6b) == 0 && rd(0x6e) == 0);

    /* D1759 (rtc-bcd off): the LE counter at 0x5c, and 0x59 is not a clock. */
    memset(&s, 0, sizeof(s));
    host_now = 1790553600;
    s.curreg = 0x5c;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)pcf50633_recv((I2CSlave *)&s) << (8 * i);
    assert(v == 1790553600 && rd(0x59) == 0);
    puts("PASS: 1.x reads the host's UTC date from the PCF50633 BCD calendar; the D1759 counter is unchanged");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'; c.write_text(code)
    subprocess.run(['clang', '-std=gnu11', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(c), '-o', str(Path(tmp) / 'check')], check=True)
    subprocess.run([str(Path(tmp) / 'check')], check=True)
