#include "hw/arm/ipod_touch_lm48821.h"
#include "migration/vmstate.h"
#include <math.h>

/*
 * LM48821, I2C 0x76. Every byte is a complete control word, not an address.
 * TI SNAS354A tables 2/3: bits 7:3 gain, bit 2 mute, bits 1/0 left/right enable.
 * https://www.ti.com/lit/ds/symlink/lm48821.pdf
 */
double lm48821_gain(uint8_t control, unsigned channel)
{
    static const int8_t db[32] = {
        -76, -62, -52, -44, -38, -34, -30, -27,
        -24, -21, -18, -16, -14, -12, -10, -8,
        -6, -4, -2, 0, 2, 4, 6, 8, 10, 12, 13, 14, 15, 16, 17, 18
    };
    if (channel > 1 || !(control & (2 >> channel))) {
        return 0;
    }
    int gain = (control & 4) ? -76 : db[control >> 3];
    return pow(10.0, gain / 20.0);
}

static uint8_t lm48821_recv(I2CSlave *i2c)
{
    return LM48821(i2c)->control;
}

static int lm48821_send(I2CSlave *i2c, uint8_t data)
{
    LM48821(i2c)->control = data;
    return 0;
}

static void lm48821_reset(DeviceState *dev)
{
    LM48821(dev)->control = 0;
}

static const VMStateDescription vmstate_lm48821 = {
    .name = "lm48821",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, LM48821State),
        VMSTATE_UINT8(control, LM48821State),
        VMSTATE_END_OF_LIST()
    }
};

static void lm48821_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_lm48821;
    device_class_set_legacy_reset(dc, lm48821_reset);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->recv = lm48821_recv;
    k->send = lm48821_send;
}

static const TypeInfo lm48821_info = {
    .name          = TYPE_LM48821,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(LM48821State),
    .class_init    = lm48821_class_init,
};

static void lm48821_register_types(void)
{
    type_register_static(&lm48821_info);
}

type_init(lm48821_register_types)
