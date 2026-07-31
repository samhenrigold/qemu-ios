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
            // Return whatever the guest last wrote to this register. A stateless
            // stub that always returned 0 here caused iOS's sleep sequence to
            // never observe the power-state transition it had just requested,
            // making it fall through into a reset instead of suspending.
            res = s->regs[reg];
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
