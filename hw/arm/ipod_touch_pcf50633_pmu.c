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
    uint8_t reg = s->curreg & 0xff;
    printf("Reading PMU register %d\n", reg);

    int res = 0;

    switch(reg) {
        case PMU_MBCS1:
            res = 1; // battery power source
            break;
        case PMU_ADCC1:
            res = 3; // battery charge voltage
            break;
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
            res = s->regs[PMU_PWRSRC_STATUS];
            if (s->usb_cable) {
                res |= PMU_PWRSRC_USB;
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
    s->regs[PMU_EVENT_C_REG] |= bits;
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
    uint8_t prev = s->regs[reg];
    s->regs[reg] = data;
    s->cmd = data;
    printf("Writing PMU register cmd %d reg %d\n", data, reg);
    if (pmu_trace()) {
        pmu_trace_access("write", reg, data);
    }

    switch(reg) {
        case PMU_DSBL1:
            lcd_changebrightness(data);
	    break;

        case PMU_PWRLATCH_REG:
            // Guest-requested power off (see the note in the header). On real
            // hardware the PMU drops the rails here and the SoC simply stops;
            // with no model for it the guest sat forever in its "pmu waiting
            // for stdby" spin and the machine had to be killed, which is what
            // made a clean shutdown impossible to use.
            if ((prev & PMU_PWRLATCH_ON) && !(data & PMU_PWRLATCH_ON)) {
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            }
            break;
    }

    s->curreg = (s->curreg + 1) & 0xff;
    return 0;
}

static void pcf50633_init(Object *obj)
{

}

/* regs[] holds the whole register file, including the power latch at 0x10 and
 * the pending EVENT_A-C interrupt bits, so a snapshot taken with a button
 * press outstanding restores with it still outstanding. */
static const VMStateDescription vmstate_pcf50633 = {
    .name = "pcf50633",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, Pcf50633State),
        VMSTATE_UINT32(cmd, Pcf50633State),
        VMSTATE_UINT32(ready, Pcf50633State),
        VMSTATE_UINT32(curreg, Pcf50633State),
        VMSTATE_BOOL(addressing, Pcf50633State),
        VMSTATE_UINT8_ARRAY(regs, Pcf50633State, 256),
        VMSTATE_UINT32(rtc_latch, Pcf50633State),
        VMSTATE_BOOL(usb_cable, Pcf50633State),
        VMSTATE_END_OF_LIST()
    }
};

static void pcf50633_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_pcf50633;
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
