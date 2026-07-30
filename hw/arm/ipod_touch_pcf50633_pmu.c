#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "hw/arm/ipod_touch_lcd.h"

static int pcf50633_event(I2CSlave *i2c, enum i2c_event event)
{
    Pcf50633State *s = PCF50633(i2c);
    // printf("%s Event %d\n", __func__, s->cmd);

    if (event == I2C_START_SEND)
    {
        // A write transaction always begins with the register-address byte.
        s->addressing = true;
    }
    else if (event == I2C_FINISH)
    {
	s->ready = 1;
	// printf("%s end send %d\n", __func__, s->cmd);
    }

    return 0;
}

/*
 * Phase 1 experiment harness (temporary, inert unless IT_PMU_FORCE is set).
 *
 * iOS parks the whole USB stack on AppleD1759PMUPowerSource reporting
 * "AppleUSBCableDetect 0"; we do not know which register/bit carries it, and
 * the register the stub special-cases (MBCS1, 0x4B) is never read. This lets
 * one binary drive many parallel boots that each force a different candidate
 * register range to a value, so the bit can be bisected empirically.
 *
 * Spec: IT_PMU_FORCE="lo-hi=val,reg=val"  (all values hex), e.g. "4-16=ff".
 * Only applies to registers that fall through to the default read path, so
 * RTC/boot-count/wake-status behaviour is untouched.
 */
static uint8_t pmu_force_val[256];
static bool pmu_force_set[256];

static void pmu_force_init(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;

    const char *spec = getenv("IT_PMU_FORCE");
    if (!spec || !*spec) {
        return;
    }

    char *dup = g_strdup(spec);
    for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
        unsigned lo, hi, val;
        if (sscanf(tok, "%x-%x=%x", &lo, &hi, &val) == 3) {
            /* range form */
        } else if (sscanf(tok, "%x=%x", &lo, &val) == 2) {
            hi = lo;
        } else {
            fprintf(stderr, "[PMUFORCE] bad spec '%s'\n", tok);
            continue;
        }
        if (lo > 0xff || hi > 0xff || lo > hi) {
            fprintf(stderr, "[PMUFORCE] out-of-range spec '%s'\n", tok);
            continue;
        }
        for (unsigned r = lo; r <= hi; r++) {
            pmu_force_val[r] = val & 0xff;
            pmu_force_set[r] = true;
        }
        fprintf(stderr, "[PMUFORCE] regs 0x%02x-0x%02x -> 0x%02x\n", lo, hi, val & 0xff);
    }
    g_free(dup);
}

static int int_to_bcd(int value) {
    int shift = 0;
    int res = 0;
    while (value > 0) {
      res |= (value % 10) << (shift++ << 2);
      value /= 10;
   }
   return res;
}

static uint8_t pcf50633_recv(I2CSlave *i2c)
{
    Pcf50633State *s = PCF50633(i2c);
    uint8_t reg = s->curreg & 0xff;
    printf("Reading PMU register %d\n", reg);

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    int res = 0;

    switch(reg) {
        case PMU_MBCS1:
            res = 1; // battery power source
            break;
        case PMU_ADCC1:
            res = 3; // battery charge voltage
            break;
        case PMU_RTCSC:  // seconds
            res = int_to_bcd(tm.tm_sec);
            break;
        case PMU_RTCMN:  // minutes
            res = int_to_bcd(tm.tm_min);
            break;
        case PMU_RTCHR:  // hours
            res = int_to_bcd(tm.tm_hour);
            break;
        case PMU_RTCDT:  // days
            res = int_to_bcd(tm.tm_mday);
            break;
        case PMU_RTCMT:  // month
            res = int_to_bcd(tm.tm_mon + 1);
            break;
        case PMU_RTCYR:  // year
            res = int_to_bcd(tm.tm_year - 100); // the year counts from 1900
            break;
        case 0x67:
            res = 1; // whether we should enable debug UARTS
            break;
        case 0x69:
            res = 0; // boot count error/panic
            break;
        case 0x76:
            res = 0; // unknown register
            break;
        default:
            // Return whatever the guest last wrote to this register. A stateless
            // stub that always returned 0 here caused iOS's sleep sequence to
            // never observe the power-state transition it had just requested,
            // making it fall through into a reset instead of suspending.
            pmu_force_init();
            /*
             * IT_PMU_CABLE=<seconds>: assert USB cable presence (reg 0x04 bit 3)
             * only after the guest has been running this long. Asserting it from
             * the first read races the driver, which reports "cable connected,
             * but don't have device configuration yet" and then never retries.
             */
            if (reg == 0x04) {
                /* IT_PMU_CABLE_AFTER=<n>: assert from the nth read of reg 4 on. */
                const char *n = getenv("IT_PMU_CABLE_AFTER");
                if (n && *n) {
                    static int seen;
                    seen++;
                    printf("[PMUCABLE] reg4 read #%d\n", seen);
                    if (seen >= atoi(n)) {
                        s->curreg = (s->curreg + 1) & 0xff;
                        return s->regs[0x04] | 0x08;
                    }
                }
                const char *d = getenv("IT_PMU_CABLE");
                if (d && *d) {
                    static time_t t0;
                    if (!t0) {
                        t0 = time(NULL);
                    }
                    if (time(NULL) - t0 >= atoi(d)) {
                        static bool announced;
                        if (!announced) {
                            announced = true;
                            printf("[PMUCABLE] asserting USB cable present\n");
                        }
                        s->curreg = (s->curreg + 1) & 0xff;
                        return s->regs[0x04] | 0x08;
                    }
                }
            }
            if (pmu_force_set[reg]) {
                res = pmu_force_val[reg];
                printf("[PMUFORCE] reg 0x%02x -> 0x%02x\n", reg, res);
            } else {
                res = s->regs[reg];
            }
    }

    // Auto-increment for sequential multi-byte reads.
    s->curreg = (s->curreg + 1) & 0xff;
    return res;
}

static int pcf50633_send(I2CSlave *i2c, uint8_t data)
{
    Pcf50633State *s = PCF50633(i2c);

    if (s->addressing)
    {
        // First byte of a write transaction selects the register.
        s->curreg = data;
        s->addressing = false;
        s->cmd = data;
        return 0;
    }

    // Subsequent bytes are data written to the selected register, which
    // auto-increments for multi-byte writes.
    uint8_t reg = s->curreg & 0xff;
    s->regs[reg] = data;
    s->cmd = data;
    printf("Writing PMU register cmd %d reg %d\n", data, reg);

    switch(reg) {
        case PMU_DSBL1:
            lcd_changebrightness(data);
	    break;
    }

    s->curreg = (s->curreg + 1) & 0xff;
    return 0;
}

static void pcf50633_init(Object *obj)
{

}

static void pcf50633_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pcf50633_event;
    k->recv = pcf50633_recv;
    k->send = pcf50633_send;
}

static const TypeInfo pcf50633_info = {
    .name          = TYPE_PCF50633,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = pcf50633_init,
    .instance_size = sizeof(Pcf50633State),
    .class_init    = pcf50633_class_init,
};

static void pcf50633_register_types(void)
{
    type_register_static(&pcf50633_info);
}

type_init(pcf50633_register_types)
