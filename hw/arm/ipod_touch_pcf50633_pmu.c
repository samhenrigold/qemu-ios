#include "qemu/osdep.h"
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "system/runstate.h"

/*
 * IT_PMU_TRACE=1 logs every PMU register access in hex together with the guest
 * PC/LR that made it. That caller pair is what identifies which driver routine
 * a register belongs to -- the D1759 kext is unsymbolised, so the register map
 * is only recoverable by correlating writes with the kernel's own serial
 * output. Off by default: the PMU is polled continuously for the battery gauge.
 */
static bool pmu_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_PMU_TRACE") != NULL;
    }
    return on;
}

static void pmu_trace_access(const char *what, uint8_t reg, uint8_t val)
{
    uint32_t pc = 0, lr = 0;

    if (current_cpu && object_dynamic_cast(OBJECT(current_cpu), TYPE_ARM_CPU)) {
        CPUARMState *env = &ARM_CPU(current_cpu)->env;
        pc = env->regs[15];
        lr = env->regs[14];
    }
    fprintf(stderr, "[PMU] %s reg 0x%02x val 0x%02x  pc=0x%08x lr=0x%08x\n",
            what, reg, val, pc, lr);
}

static void pmu_update_irq(Pcf50633State *s)
{
    uint8_t pending = 0;
    for (unsigned i = 0; i < 3; i++) {
        pending |= s->regs[PMU_EVENT_A_REG + i] &
                   ~s->regs[PMU_IRQ_MASK_A + i];
    }
    qemu_set_irq(s->irq, pending != 0);
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
    if (was_charging != pmu_charge_active(s)) {
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
        pmu_latch_event(s, PMU_EVENT_C_REG, 1 << 2);
    }
}

static void pmu_adc_complete(void *opaque)
{
    Pcf50633State *s = opaque;
    s->regs[PMU_ADC_CONTROL] &= ~0x10;
    s->regs[PMU_ADC_RESULT_LO] = (s->regs[PMU_ADC_RESULT_LO] & ~3) |
                               (s->adc_sample & 3);
    s->regs[PMU_ADC_RESULT_HI] = s->adc_sample >> 2;
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
        timer_mod(s->adc_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
}

void pcf50633_set_usb_cable(Pcf50633State *s, bool attached)
{
    pcf50633_update_battery(s);
    if (s->usb_cable != attached) {
        s->usb_cable = attached;
        pmu_latch_event(s, PMU_EVENT_A_REG, PMU_PWRSRC_USB);
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

static uint8_t pcf50633_recv(I2CSlave *i2c)
{
    Pcf50633State *s = PCF50633(i2c);
    pcf50633_update_battery(s);
    uint8_t reg = s->curreg & 0xff;
    if (pmu_trace()) {
        fprintf(stderr, "Reading PMU register %d\n", reg);
    }

    int res = 0;

    switch(reg) {
        case PMU_RTC_COUNTER:
            // Take the snapshot on the low byte, so the four bytes the driver
            // reads back describe one instant even if the host second ticks
            // over mid-transfer. 2.1.1's driver has no ripple retry at all, so
            // without this it can observe a torn counter.
            s->rtc_latch = (uint32_t)time(NULL);
            res = s->rtc_latch & 0xff;
            break;
        case PMU_RTC_COUNTER + 1:
        case PMU_RTC_COUNTER + 2:
        case PMU_RTC_COUNTER + 3:
            if (s->rtc_latch == 0) {
                // Read out of order (nobody does, but do not answer zero).
                s->rtc_latch = (uint32_t)time(NULL);
            }
            res = (s->rtc_latch >> (8 * (reg - PMU_RTC_COUNTER))) & 0xff;
            break;
        case 0x69:
            res = 0; // boot count error/panic
            break;
        case 0x76:
            res = 0; // unknown register
            break;
        case PMU_PWRSRC_STATUS:   // 0x04
            // Power-source live-level status. Bit 3 = USB cable present. Only OR
            // in that one bit -- forcing the whole 0x04-0x06 block hangs boot on
            // the Apple logo. Gated on the machine's usb-attached option so an
            // unplugged device can still be emulated; it defaults on because the
            // emulated device is effectively tethered to the host.
            res = s->regs[PMU_PWRSRC_STATUS] & ~PMU_PWRSRC_USB;
            if (s->usb_cable) {
                res |= PMU_PWRSRC_USB;
            }
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
        case PMU_EVENT_A_REG:     // 0x01
        case PMU_EVENT_A_REG + 1: // 0x02
        case PMU_EVENT_C_REG:     // 0x03
            // EVENT_A/B/C interrupt status. iOS reads the three as one block
            // starting at subaddress 0x01; each is read-to-clear, matching real
            // interrupt-event registers, so a latched wake event is consumed
            // exactly once.
            res = s->regs[reg];
            s->regs[reg] = 0;
            pmu_update_irq(s);
            break;
        default:
            // Falls through to the register file, which is what the RTC offset
            // at PMU_RTC_OFFSET (0x64..0x67) wants: zero until the guest writes
            // an offset of its own. 0x67 used to be forced to 1 here, labelled
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
        fprintf(stderr, "Writing PMU register cmd %d reg %d\n", data, reg);
    }
    if (pmu_trace()) {
        pmu_trace_access("write", reg, data);
    }

    switch(reg) {
        case PMU_IRQ_MASK_A ... PMU_IRQ_MASK_A + 2:
            pmu_update_irq(s);
            break;
        case PMU_ADC_CONTROL:
            pmu_adc_command(s, data);
            break;
        case PMU_DSBL1:
            lcd_changebrightness(data);
	    break;

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

static void pcf50633_reset(DeviceState *dev)
{
    Pcf50633State *s = PCF50633(dev);
    pcf50633_update_battery(s);
    timer_del(s->adc_timer);
    /* The power-on transition consumes the standby command. Leaving 0x90
     * latched makes iBoot re-enter its charging/standby path after Power On. */
    s->regs[PMU_STANDBY_CMD] = 0;
    s->regs[PMU_ADC_CONTROL] = 0;
    s->adc_sample = 0;
    for (unsigned i = 0; i < 3; i++) {
        s->regs[PMU_EVENT_A_REG + i] = 0;
        s->regs[PMU_IRQ_MASK_A + i] = 0xff;
    }
    s->addressing = true;
    s->rtc_latch = 0;
    qatomic_set(&guest_shutdown_confirmed, false);
    pmu_update_irq(s);
}

static int pcf50633_post_load(void *opaque, int version_id)
{
    Pcf50633State *s = opaque;
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
    .version_id = 4,
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
        VMSTATE_END_OF_LIST()
    }
};

static void pcf50633_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_pcf50633;
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
