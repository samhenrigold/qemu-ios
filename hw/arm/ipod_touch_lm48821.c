#include "hw/arm/ipod_touch_lm48821.h"

/*
 * LM48821 speaker amp, I2C 0x76. Register-mapped I2C stub: first written byte is
 * the register pointer, following writes fill the register file, reads echo it.
 * Everything is ACKed. No audio flows through here.
 */

static int lm48821_event(I2CSlave *i2c, enum i2c_event event)
{
    LM48821State *s = LM48821(i2c);
    if (event == I2C_START_SEND) {
        s->have_reg = false;
    }
    return 0;
}

static uint8_t lm48821_recv(I2CSlave *i2c)
{
    LM48821State *s = LM48821(i2c);
    uint8_t reg = s->cmd & 0xff;
    uint8_t val = s->regs[reg];
    s->cmd = (reg + 1) & 0xff;
    return val;
}

static int lm48821_send(I2CSlave *i2c, uint8_t data)
{
    LM48821State *s = LM48821(i2c);

    if (!s->have_reg) {
        s->cmd = data;
        s->have_reg = true;
        return 0;
    }

    s->regs[s->cmd & 0xff] = data;
    s->cmd = (s->cmd + 1) & 0xff;
    return 0;
}

static void lm48821_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = lm48821_event;
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
