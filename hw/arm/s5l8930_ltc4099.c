/*
 * Linear LTC4099 USB power manager / charger, I2C0 address 0x09 on the iPad 1.
 *
 * 7B500 AppleLTC4099Charger (kext at c065c000). Its I2C helpers are readReg
 * c065d024 (subaddress 0, 1 or 2 bytes, returns the first) and writeReg
 * c065d060 (one byte at subaddress 0, 1 or 2): start() and the power-state
 * path write reg 2 = 0x80, _setCharger writes reg 1 = 0xc4/0xd4 and reg 0 =
 * the input-current/charge code; nothing written is ever read back. The
 * 'STAT' platform function (c065d210) returns (status & mask) != 0 where the
 * mask comes from the caller's DT entry: function-usb_det on the USB
 * arbitrator uses 0x80 (USB present), secondary_charge_status uses 3 (charge
 * state 1/2 = charging). The interrupt/timer path c065d79c re-reads the same
 * byte and tests 0x80.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930LTC4099State, S5L8930_LTC4099)

#define LTC_STATUS_USB      0x80

struct S5L8930LTC4099State {
    I2CSlave i2c;
    uint8_t regs[4];
    uint8_t reg;            /* current register, auto-incrementing */
    bool addressing;        /* next byte received is the register number */
    bool usb_present;       /* the cable: STAT bit 0x80 */
};

static int ltc4099_event(I2CSlave *i2c, enum i2c_event event)
{
    S5L8930LTC4099State *s = S5L8930_LTC4099(i2c);

    if (event == I2C_START_SEND) {
        s->addressing = true;
    }
    return 0;
}

static uint8_t ltc4099_recv(I2CSlave *i2c)
{
    S5L8930LTC4099State *s = S5L8930_LTC4099(i2c);
    uint8_t reg = s->reg++ & 3;

    if (reg == 0) {
        return s->usb_present ? LTC_STATUS_USB : 0;
    }
    return s->regs[reg];
}

static int ltc4099_send(I2CSlave *i2c, uint8_t data)
{
    S5L8930LTC4099State *s = S5L8930_LTC4099(i2c);

    if (s->addressing) {
        s->reg = data;
        s->addressing = false;
    } else {
        s->regs[s->reg++ & 3] = data;
    }
    return 0;
}

void s5l8930_ltc4099_set_usb(DeviceState *dev, bool present)
{
    S5L8930_LTC4099(dev)->usb_present = present;
}

static void ltc4099_reset(DeviceState *dev)
{
    S5L8930LTC4099State *s = S5L8930_LTC4099(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->reg = 0;
    s->addressing = true;
}

static const VMStateDescription vmstate_s5l8930_ltc4099 = {
    .name = TYPE_S5L8930_LTC4099,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, S5L8930LTC4099State),
        VMSTATE_UINT8_ARRAY(regs, S5L8930LTC4099State, 4),
        VMSTATE_UINT8(reg, S5L8930LTC4099State),
        VMSTATE_BOOL(addressing, S5L8930LTC4099State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ltc4099_properties[] = {
    DEFINE_PROP_BOOL("usb-present", S5L8930LTC4099State, usb_present, true),
};

static void ltc4099_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8930_ltc4099;
    device_class_set_legacy_reset(dc, ltc4099_reset);
    device_class_set_props(dc, ltc4099_properties);
    k->event = ltc4099_event;
    k->recv = ltc4099_recv;
    k->send = ltc4099_send;
}

static const TypeInfo s5l8930_ltc4099_info = {
    .name          = TYPE_S5L8930_LTC4099,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(S5L8930LTC4099State),
    .class_init    = ltc4099_class_init,
};

static void s5l8930_ltc4099_register_types(void)
{
    type_register_static(&s5l8930_ltc4099_info);
}

type_init(s5l8930_ltc4099_register_types)
