#include "qemu/osdep.h"
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "system/runstate.h"
#include "trace.h"
#include "hw/trace-printf.h"

/*
 * -trace pcf50633_pmu_log logs every PMU register access in hex together with the guest
 * PC/LR that made it. That caller pair is what identifies which driver routine
 * a register belongs to -- the D1759 kext is unsymbolized, so the register map
 * is only recoverable by correlating writes with the kernel's own serial
 * output. Off by default: the PMU is polled continuously for the battery gauge.
 */
static bool pmu_trace(void)
{
    return trace_event_get_state_backends(TRACE_PCF50633_PMU_LOG);
}

static void pmu_trace_access(const char *what, uint8_t reg, uint8_t val)
{
    uint32_t pc = 0, lr = 0;

    if (current_cpu && object_dynamic_cast(OBJECT(current_cpu), TYPE_ARM_CPU)) {
        CPUARMState *env = &ARM_CPU(current_cpu)->env;
        pc = env->regs[15];
        lr = env->regs[14];
    }
    TRACE_PRINTF(trace_pcf50633_pmu_log, "[PMU] %s reg 0x%02x val 0x%02x  pc=0x%08x lr=0x%08x\n",
            what, reg, val, pc, lr);
}

/* What the panel gets: the WLED level, only while its rail is enabled. With no
 * level register ("backlight-level-reg" 0, the 1G) the light is on/off only. */
static void pmu_update_backlight(Pcf50633State *s)
{
    bool on = s->regs[s->backlight_enable_reg] & s->backlight_enable_bit;
    lcd_changebrightness(!on ? 0 : s->backlight_level_reg ? s->regs[s->backlight_level_reg] : 255);
}

/*
 * The event block. The BCD-calendar part is the N45 PCF50635, not the D1759:
 * INT1..5 = 0x02..06, INT1M..5M = 0x07..0b. The Dialog parts put N event
 * bytes at 0x01, N status bytes after them and N mask bytes after those
 * ("event-count": D1759 3, so masks at 0x07; D1755 4, masks at 0x09).
 */
static unsigned pmu_event_base(Pcf50633State *s)
{
    return s->rtc_bcd ? 2 : PMU_EVENT_A_REG;
}

static unsigned pmu_event_count(Pcf50633State *s)
{
    return s->rtc_bcd ? 5 : s->event_count;
}

static unsigned pmu_mask_base(Pcf50633State *s)
{
    return s->rtc_bcd ? PMU_IRQ_MASK_A : PMU_EVENT_A_REG + 2 * s->event_count;
}

/*
 * The AP's power, which the PMU switches. Hibernate turns it off; the next
 * wake turns it on, and the SoC comes up through its boot ROM and LLB, which
 * find the suspend marker the kernel left (0x6f, and its resume vector in
 * DRAM) and resume the kernel instead of booting. The PMU itself stays
 * powered across that power-on reset: its events tell the kernel why it woke.
 */
static void pmu_set_ap_power(Pcf50633State *s, bool on)
{
    /* Boards that don't wire the AP's rail (the 1G's PCF50633, the D1755
     * boards for now) keep the CPU parked where the kernel left it. */
    if (!s->ap_power || s->ap_off != on) {
        return;
    }
    s->ap_off = !on;
    s->ap_waking = on;
    if (on) {
        /* EVENT_B bit 7 says the AP came up out of hibernate. 7E18's LLB reads
         * EVENT_B first; with the bit set it keeps 0x6f's suspend marker and
         * resumes, without it it clears the marker and boots (LLB 0x22000f32). */
        s->regs[PMU_EVENT_A_REG + 1] |= PMU_EVENT_B_HIB_WAKE;
    }
    qemu_set_irq(s->ap_power, on);
}

static void pmu_update_irq(Pcf50633State *s)
{
    uint8_t pending = 0;
    unsigned base = pmu_event_base(s), count = pmu_event_count(s);
    for (unsigned i = 0; i < count; i++) {
        pending |= s->regs[base + i] &
                   ~s->regs[pmu_mask_base(s) + i];
    }
    qemu_set_irq(s->irq, pending != 0);
    /* An event the guest left unmasked wakes a hibernating AP. */
    if (pending && s->ap_off) {
        pmu_set_ap_power(s, true);
    }
}

static void pmu_latch_event(Pcf50633State *s, unsigned event, uint8_t bits)
{
    s->regs[event] |= bits;
    pmu_update_irq(s);
}

/* 7E18 IOPMPowerSource battery-data/0003-default, percent and millivolts.
 * The guest applies its own measurement interval and capacity filter. */
static const uint16_t battery_curve[][2] = {
    {0, 3000}, {2, 3450}, {3, 3667}, {5, 3707}, {9, 3742},
    {14, 3765}, {18, 3783}, {23, 3800}, {27, 3800}, {32, 3824},
    {36, 3824}, {41, 3841}, {45, 3853}, {50, 3877}, {55, 3888},
    {59, 3912}, {64, 3941}, {68, 3965}, {73, 3994}, {77, 4023},
    {82, 4047}, {86, 4094}, {91, 4129}, {95, 4150}, {100, 4200},
};

unsigned pcf50633_adc_for_level(unsigned percent)
{
    percent = MIN(percent, 100);
    for (unsigned i = 1; i < ARRAY_SIZE(battery_curve); i++) {
        unsigned lo = battery_curve[i - 1][0], hi = battery_curve[i][0];
        if (percent <= hi) {
            unsigned mv = battery_curve[i - 1][1] +
                ((battery_curve[i][1] - battery_curve[i - 1][1]) *
                 (percent - lo) + (hi - lo) / 2) / (hi - lo);
            return ((mv - 2500) * 1024 + 1000) / 2000;
        }
    }
    return 870;
}

unsigned pcf50633_level_for_adc(unsigned counts)
{
    unsigned mv = 2500 + MIN(counts, 1023) * 2000 / 1024;
    if (mv <= battery_curve[0][1]) {
        return 0;
    }
    for (unsigned i = 1; i < ARRAY_SIZE(battery_curve); i++) {
        unsigned lo = battery_curve[i - 1][1], hi = battery_curve[i][1];
        if (mv <= hi && hi > lo) {
            return battery_curve[i - 1][0] +
                ((battery_curve[i][0] - battery_curve[i - 1][0]) *
                 (mv - lo) + (hi - lo) / 2) / (hi - lo);
        }
    }
    return 100;
}

static bool pmu_charge_active(Pcf50633State *s)
{
    return s->usb_cable && !(s->regs[0x0a] & 0x0c) &&
           s->charging_mode != 2 &&
           (s->charging_mode == 1 || s->adc_values[4] < 870);
}

static void pmu_apply_battery_adc(Pcf50633State *s, unsigned counts)
{
    bool was_charging = pmu_charge_active(s);
    s->adc_values[4] = MIN(counts, 1023);
    if (!s->rtc_bcd && was_charging != pmu_charge_active(s)) {
        pmu_latch_event(s, PMU_EVENT_C_REG, 1 << 2);
    }
}

/* Sample drain lazily on the guest's own ADC/status reads. Virtual time
 * freezes while paused, and no extra periodic interrupt is needed. */
void pcf50633_update_battery(Pcf50633State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->drain_rate > 0 && now > s->drain_updated_ns &&
        (!s->usb_cable || s->charging_mode == 2)) {
        double elapsed = ((double)now - s->drain_updated_ns) / 60000000000.0;
        s->drain_level = MAX(0.0, s->drain_level - s->drain_rate * elapsed);
        pmu_apply_battery_adc(s, pcf50633_adc_for_level((unsigned)(s->drain_level + 0.5)));
    }
    s->drain_updated_ns = now;
}

void pcf50633_set_battery_adc(Pcf50633State *s, unsigned counts)
{
    pmu_apply_battery_adc(s, counts);
    s->drain_level = pcf50633_level_for_adc(s->adc_values[4]);
    s->drain_updated_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

void pcf50633_set_battery_level(Pcf50633State *s, unsigned level)
{
    pcf50633_set_battery_adc(s, pcf50633_adc_for_level(level));
    s->drain_level = MIN(level, 100);
}

void pcf50633_set_battery_drain(Pcf50633State *s, double rate)
{
    pcf50633_update_battery(s);
    s->drain_rate = rate;
}

void pcf50633_set_charging_mode(Pcf50633State *s, unsigned mode)
{
    pcf50633_update_battery(s);
    if (s->charging_mode != mode) {
        s->charging_mode = mode;
        if (!s->rtc_bcd) {
            pmu_latch_event(s, PMU_EVENT_C_REG, 1 << 2);
        }
    }
}

static void pmu_adc_complete(void *opaque)
{
    Pcf50633State *s = opaque;
    /* control at adc-reg, the 10-bit result in the next two (D1759 0x40, D1755 0x30) */
    s->regs[s->adc_reg] &= ~0x10;
    s->regs[s->adc_reg + 1] = (s->regs[s->adc_reg + 1] & ~3) | (s->adc_sample & 3);
    s->regs[s->adc_reg + 2] = s->adc_sample >> 2;
    pmu_latch_event(s, PMU_EVENT_A_REG + 1, PMU_ADC_DONE);
}

static void pmu_adc_command(Pcf50633State *s, uint8_t command)
{
    timer_del(s->adc_timer);
    /* Channel 3's bit-5 command is an 80 ms settling phase implemented by
     * the driver's own timer. Signaling ADC completion there deadlocks its
     * wait for the timeout state. Only bit 4 starts the actual conversion. */
    if (command & 0x10) {
        pcf50633_update_battery(s);
        s->adc_sample = s->adc_values[command & 15] & 1023;
        /* The dock's data lines (brick id): a USB host's pull-downs read 0 V; a
         * charger biases them. 5.x's D1755 power source reads them (twice) to
         * tell the two apart, and mid-scale reads as a charger: "Detached",
         * no USB device stack. */
        if ((command & 15) == s->brick_mux && s->usb_cable) {
            s->adc_sample = 0;
        }
        timer_mod(s->adc_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
}

void pcf50633_set_usb_cable(Pcf50633State *s, bool attached)
{
    pcf50633_update_battery(s);
    if (s->usb_cable != attached) {
        s->usb_cable = attached;
        pmu_latch_event(s, s->rtc_bcd ? 2 : PMU_EVENT_A_REG,
                        s->rtc_bcd ? (attached ? 0x04 : 0x08) : PMU_PWRSRC_USB);
    }
}

/* N45 DT function-button_wake uses EXTON1; its GPIO button is a separate
 * input. Both edges latch even while masked, until the driver reads INT2. */
void pcf50633_set_exton1(Pcf50633State *s, bool high)
{
    if (s->rtc_bcd && s->exton1 != high) {
        s->exton1 = high;
        pmu_latch_event(s, 3, high ? 0x04 : 0x08);
    }
}

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

static uint8_t pmu_bcd(unsigned v)
{
    return (v / 10) << 4 | v % 10;
}

/*
 * The PCF50633's own RTC (1.x, "rtc-bcd"): RTCSC..RTCYR at 0x59..0x5f, BCD
 * seconds, minutes, hours, weekday (0 = Sunday), day, month, two-digit year.
 * ApplePCF50635PMURTC::getCurrentDateTime (3A101a 0xc047bb04, the same code in
 * 4B1) reads the seven bytes from 0x59, reads 0x59 once more and retries
 * until the seconds agree, then takes year = 2000 + RTCYR; AppleARMRTC turns
 * that into Unix seconds. Host UTC, snapshotted on the RTCSC read so a block
 * describes one instant. The offset the OS keeps is its own (0x6b..0x6e, the
 * register file). Before this the D1759's counter bytes answered at 0x5c..0x5f
 * (weekday/day/month/year here) and 0x59..0x5b read zero: a date that moved
 * with bits 8..31 of the counter, days per boot.
 */
static uint8_t pmu_bcd_rtc_read(Pcf50633State *s, uint8_t reg)
{
    if (reg == PMU_BCD_RTC || s->rtc_latch == 0) {
        s->rtc_latch = (uint32_t)time(NULL);
    }
    time_t t = s->rtc_latch;
    struct tm tm;
    gmtime_r(&t, &tm);
    const unsigned field[7] = { tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_wday,
                                tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100 };
    return pmu_bcd(field[reg - PMU_BCD_RTC]);
}

static uint8_t pcf50633_recv(I2CSlave *i2c)
{
    Pcf50633State *s = PCF50633(i2c);
    pcf50633_update_battery(s);
    uint8_t reg = s->curreg & 0xff;
    if (pmu_trace()) {
        TRACE_PRINTF(trace_pcf50633_pmu_log, "Reading PMU register %d\n", reg);
    }

    int res = 0;

    if (s->rtc_bcd && reg >= PMU_BCD_RTC && reg < PMU_BCD_RTC + 7) {
        res = pmu_bcd_rtc_read(s, reg);
        goto done;
    }
    if (!s->rtc_bcd && reg >= s->rtc_reg && reg < s->rtc_reg + 4) {
        // Take the snapshot on the low byte, so the four bytes the driver
        // reads back describe one instant even if the host second ticks
        // over mid-transfer. 2.1.1's driver has no ripple retry at all, so
        // without this it can observe a torn counter. (Read out of order,
        // which nobody does, still answers the time rather than zero.)
        if (reg == s->rtc_reg || s->rtc_latch == 0) {
            s->rtc_latch = (uint32_t)time(NULL);
        }
        res = (s->rtc_latch >> (8 * (reg - s->rtc_reg))) & 0xff;
        goto done;
    }
    unsigned event_base = pmu_event_base(s);
    unsigned event_count = pmu_event_count(s);
    if (reg >= event_base && reg < event_base + event_count) {
        res = s->regs[reg];
        s->regs[reg] = 0;
        pmu_update_irq(s);
        goto done;
    }
    switch (reg) {
        case 0x69:
            res = 0; // boot count error/panic
            break;
        case 0x76:
            res = 0; // unknown register
            break;
        case PMU_PWRSRC_STATUS + 1:
            /* 7E18 c05ff4a0 tests status byte 1 bits 1/2 for charging.
             * Report an active charging phase while external USB power is
             * available and the guest has not disabled charging (0x0a[3:2]). */
            res = s->regs[reg] & ~6;
            if (pmu_charge_active(s)) {
                res |= 2;
            }
            break;
        default:
            // Falls through to the register file, which is what the RTC offset
            // at PMU_RTC_OFFSET (0x64..0x67) wants: zero until the guest writes
            // an offset of its own. 0x67 used to be forced to 1 here, labeled
            // "whether we should enable debug UARTS" -- nothing reads it for
            // that (traced over a whole 2.1.1 and a whole 3.1.3 boot: the only
            // reader of 0x67 is the RTC driver's four-byte offset read). All it
            // did was add 0x01000000 to the offset, i.e. 194 days.
            //
            // Return whatever the guest last wrote to this register. A stateless
            // stub that always returned 0 here caused iOS's sleep sequence to
            // never observe the power-state transition it had just requested,
            // making it fall through into a reset instead of suspending.
            res = s->regs[reg];
    }
done:
    if (reg == s->usb_status_reg) {
        // The live cable level ("usb-status-reg"/"-bits"): the D1759's power-source
        // status 0x04 bit 3 (2.x+), the PCF50633's MBCS1 0x4b USBPRES|USBOK (1.x,
        // ApplePCF50635PMUPowerSource's "ext"). Only those bits -- forcing the whole
        // D1759 0x04-0x06 block hangs boot on the Apple logo. Gated on the machine's
        // cable (usb-attached on the 2G, a usb-tcp-addr on the 1G).
        res = (res & ~s->usb_status_bits) | (s->usb_cable ? s->usb_status_bits : 0);
    }

    if (pmu_trace()) {
        pmu_trace_access("read ", reg, res & 0xff);
    }

    // Auto-increment for sequential multi-byte reads.
    s->curreg = (s->curreg + 1) & 0xff;
    return res;
}

void pcf50633_latch_wake_event(Pcf50633State *s, uint8_t bits)
{
    // Latch the wake-button interrupt in EVENT_C (reg 0x03). It stays set until
    // iOS reads the event block (read-to-clear above), so it survives a quick
    // press/release until the guest's PMU interrupt handler consumes it.
    pmu_latch_event(s, PMU_EVENT_C_REG, bits);
    /* The wake buttons power the AP on whatever the masks say: 7E18 goes to
     * sleep with both masked in EVENT_C (0x09 = 0xab). */
    if (s->ap_off) {
        pmu_set_ap_power(s, true);
    }
}

static bool guest_shutdown_confirmed;

bool pcf50633_guest_shutdown_confirmed(void)
{
    return qatomic_read(&guest_shutdown_confirmed);
}

static void pcf50633_guest_shutdown(void)
{
    qatomic_set(&guest_shutdown_confirmed, true);
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
}

void pcf50633_set_stat(Pcf50633State *s, uint8_t bits, bool on)
{
    if (on) {
        s->regs[PMU_STAT_REG] |= bits;
    } else {
        s->regs[PMU_STAT_REG] &= ~bits;
    }
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
    if (pmu_trace()) {
        TRACE_PRINTF(trace_pcf50633_pmu_log, "Writing PMU register cmd %d reg %d\n", data, reg);
    }
    if (pmu_trace()) {
        pmu_trace_access("write", reg, data);
    }

    if (reg == s->backlight_enable_reg || (reg && reg == s->backlight_level_reg)) {
        pmu_update_backlight(s);
    }
    if (reg == s->shutdown_reg) {
        /* Native 7E18 without USB power sets bit 0, then waits forever
         * in AppleD1759PMU's "pmu go stdby" path (c05fba80-c05fbacc). */
        if (data & PMU_SHUTDOWN_GO) {
            s->shutdown_armed = false;
            pcf50633_guest_shutdown();
        } else if ((data & PMU_HIBERNATE_GO) && s->ap_power) {
            /* The power transition consumes the command, as it does standby's
             * (see pcf50633_reset): LLB rewrites this register as it wakes. */
            s->regs[reg] &= ~PMU_HIBERNATE_GO;
            pmu_set_ap_power(s, false);
        }
    } else if (reg >= pmu_mask_base(s) && reg < pmu_mask_base(s) + pmu_event_count(s)) {
        pmu_update_irq(s);
    } else if (reg == s->adc_reg) {
        pmu_adc_command(s, data);
    } else switch (reg) {

        case PMU_STANDBY_CMD:
            /*
             * The end of 3.1.3's shutdown: rails sequenced down, interrupts
             * masked, root volume already unmounted, and this is the last thing
             * it says before waiting for the power to go. Native launchd
             * shutdown reaches this without any host powerdown notification;
             * hardware must not require a host-only arming flag.
             */
            if (data == PMU_STANDBY_GO) {
                s->shutdown_armed = false;
                pcf50633_guest_shutdown();
            }
            break;
    }

    s->curreg = (s->curreg + 1) & 0xff;
    return 0;
}

static void pcf50633_init(Object *obj)
{
    Pcf50633State *s = PCF50633(obj);
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->ap_power, "ap-power", 1);
    s->adc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_adc_complete, s);
    /* 7E18: channel 2 thermistor (about 10 kohm), channel 4 battery voltage
     * (2500 + counts * 2000 / 1024 mV), channel 6 USB charger identification.
     * Battery percentage calibration is a separate machine control. */
    /* Dock function-read_acc selects channel 3. An open accessory-ID input
     * reads full scale; zero falsely identifies a dock with line-out audio. */
    s->adc_values[3] = 1023;
    s->adc_values[2] = 205;
    pcf50633_set_battery_adc(s, 850);
    s->adc_values[6] = 512;
}

/* For the host (qemu_ios_ui_backlight_level): the LED level register, 0 while the rail is off. */
static int pmu_backlight_level(void *opaque)
{
    Pcf50633State *s = opaque;
    return s->regs[s->backlight_enable_reg] & s->backlight_enable_bit ? s->regs[s->backlight_led_reg] : 0;
}

static void pcf50633_reset(DeviceState *dev)
{
    Pcf50633State *s = PCF50633(dev);
    pcf50633_update_battery(s);
    if (s->ap_waking) {
        /* The AP's power-on reset; the PMU was up all along. */
        s->ap_waking = false;
        return;
    }
    s->ap_off = false;
    timer_del(s->adc_timer);
    /* The power-on transition consumes the standby command. Leaving 0x90
     * latched makes iBoot re-enter its charging/standby path after Power On. */
    s->regs[PMU_STANDBY_CMD] = 0;
    s->regs[s->shutdown_reg] &= ~PMU_SHUTDOWN_GO;
    s->regs[s->adc_reg] = 0;
    /* The backlight rail is on out of reset (iBoot lights its logo without
     * touching 0x10); the level is whatever the guest programs. */
    s->regs[s->backlight_enable_reg] |= s->backlight_enable_bit;
    if (s->backlight_led_reg) {
        ios_backlight_register(pmu_backlight_level, s);
    }
    s->adc_sample = 0;
    unsigned base = pmu_event_base(s), count = pmu_event_count(s);
    for (unsigned i = 0; i < count; i++) {
        s->regs[base + i] = 0;
        s->regs[pmu_mask_base(s) + i] = 0xff;
    }
    s->addressing = true;
    s->rtc_latch = 0;
    qatomic_set(&guest_shutdown_confirmed, false);
    pmu_update_irq(s);
}

static int pcf50633_post_load(void *opaque, int version_id)
{
    Pcf50633State *s = opaque;
    if (s->rtc_bcd && version_id < 5) {
        /* Older N45 states used the D1759 interrupt map. */
        return -EINVAL;
    }
    if (version_id < 4) {
        s->drain_rate = 0;
        s->drain_level = pcf50633_level_for_adc(s->adc_values[4]);
        s->drain_updated_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    } else if (!isfinite(s->drain_rate) || s->drain_rate < 0 || s->drain_rate > 100 ||
               !isfinite(s->drain_level) || s->drain_level < 0 || s->drain_level > 100) {
        return -EINVAL;
    }
    pmu_update_irq(opaque);
    return 0;
}

static void pcf50633_finalize(Object *obj)
{
    timer_free(PCF50633(obj)->adc_timer);
}

/* regs[] holds the whole register file, including the power latch at 0x10 and
 * the pending EVENT_A-C interrupt bits, so a snapshot taken with a button
 * press outstanding restores with it still outstanding. */
static const VMStateDescription vmstate_pcf50633 = {
    .name = "pcf50633",
    .version_id = 6,
    .minimum_version_id = 1,
    .post_load = pcf50633_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, Pcf50633State),
        VMSTATE_UINT32(cmd, Pcf50633State),
        VMSTATE_UINT32(ready, Pcf50633State),
        VMSTATE_UINT32(curreg, Pcf50633State),
        VMSTATE_BOOL(addressing, Pcf50633State),
        VMSTATE_UINT8_ARRAY(regs, Pcf50633State, 256),
        VMSTATE_UINT32(rtc_latch, Pcf50633State),
        VMSTATE_BOOL(usb_cable, Pcf50633State),
        VMSTATE_BOOL(shutdown_armed, Pcf50633State),
        VMSTATE_UINT16_ARRAY_V(adc_values, Pcf50633State, 16, 2),
        VMSTATE_UINT16_V(adc_sample, Pcf50633State, 2),
        VMSTATE_TIMER_PTR_V(adc_timer, Pcf50633State, 2),
        VMSTATE_UINT8_V(charging_mode, Pcf50633State, 3),
        VMSTATE_UINT64_V(drain_rate_bits, Pcf50633State, 4),
        VMSTATE_UINT64_V(drain_level_bits, Pcf50633State, 4),
        VMSTATE_INT64_V(drain_updated_ns, Pcf50633State, 4),
        VMSTATE_BOOL_V(exton1, Pcf50633State, 5),
        VMSTATE_BOOL_V(ap_off, Pcf50633State, 6),
        VMSTATE_END_OF_LIST()
    }
};

static const Property pcf50633_properties[] = {
    /* Register whose bit 0 is "go to standby". 0x0a is where 2.x/3.x's
     * AppleD1759PMU writes it; iPhone OS 1.x's ApplePCF50635PMU uses 0x0a as
     * its fourth interrupt mask (writes 0xff there at start) and the
     * datasheet's OOCSHDWN at 0x0c for standby. */
    DEFINE_PROP_UINT8("shutdown-reg", Pcf50633State, shutdown_reg, PMU_SHUTDOWN_REG),
    DEFINE_PROP_UINT8("usb-status-reg", Pcf50633State, usb_status_reg, PMU_PWRSRC_STATUS),
    DEFINE_PROP_UINT8("usb-status-bits", Pcf50633State, usb_status_bits, PMU_PWRSRC_USB),
    /* The backlight: its enable register/bit and level register (0 = on/off only). The
     * defaults are the D1759's (0x10 bit 6, 0x30); 1.x's PCF50633 drives LEDENA 0x29 bit 0
     * (cleared when the display sleeps) and LEDOUT 0x28 (a 6-bit LED current, not rendered). */
    DEFINE_PROP_UINT8("backlight-enable-reg", Pcf50633State, backlight_enable_reg, PMU_LDO_ENABLE),
    DEFINE_PROP_UINT8("backlight-enable-bit", Pcf50633State, backlight_enable_bit, PMU_LDO_BACKLIGHT),
    DEFINE_PROP_UINT8("backlight-level-reg", Pcf50633State, backlight_level_reg, PMU_DSBL1),
    /* The LED level register reported to the host as the programmed backlight level (0: none decoded):
     * the D1759's 0x30, the PCF50633's LEDOUT 0x28. */
    DEFINE_PROP_UINT8("backlight-led-reg", Pcf50633State, backlight_led_reg, PMU_DSBL1),
    /* The PCF50633's BCD calendar at 0x59 (1.x) instead of the D1759's counter at 0x5c. */
    DEFINE_PROP_BOOL("rtc-bcd", Pcf50633State, rtc_bcd, false),
    DEFINE_PROP_UINT8("event-count", Pcf50633State, event_count, 3),
    DEFINE_PROP_UINT8("adc-reg", Pcf50633State, adc_reg, PMU_ADC_CONTROL),
    DEFINE_PROP_UINT8("brick-mux", Pcf50633State, brick_mux, 0xff),
    DEFINE_PROP_UINT8("rtc-reg", Pcf50633State, rtc_reg, PMU_RTC_COUNTER),
};

static void pcf50633_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_pcf50633;
    device_class_set_props(dc, pcf50633_properties);
    device_class_set_legacy_reset(dc, pcf50633_reset);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pcf50633_event;
    k->recv = pcf50633_recv;
    k->send = pcf50633_send;
}

static const TypeInfo pcf50633_info = {
    .name          = TYPE_PCF50633,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = pcf50633_init,
    .instance_finalize = pcf50633_finalize,
    .instance_size = sizeof(Pcf50633State),
    .class_init    = pcf50633_class_init,
};

static void pcf50633_register_types(void)
{
    type_register_static(&pcf50633_info);
}

type_init(pcf50633_register_types)
