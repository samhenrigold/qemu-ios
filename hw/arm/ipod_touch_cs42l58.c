#include "hw/arm/ipod_touch_cs42l58.h"

static int cs42l58_event(I2CSlave *i2c, enum i2c_event event)
{
    return 0;
}

static uint8_t cs42l58_recv(I2CSlave *i2c)
{
    CS42L58State *s = CS42L58(i2c);
    printf("Reading stereo codec register %d\n", s->cmd);

    switch(s->cmd) {
        default:
            return 0;
    }
}

static int cs42l58_send(I2CSlave *i2c, uint8_t data)
{
    CS42L58State *s = CS42L58(i2c);
    s->cmd = data;
    printf("Writing stereo codec register %d\n", s->cmd);
    return 0;
}

static void cs42l58_init(Object *obj)
{

}

static void cs42l58_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = cs42l58_event;
    k->recv = cs42l58_recv;
    k->send = cs42l58_send;
}

static const TypeInfo cs42l58_info = {
    .name          = TYPE_CS42L58,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = cs42l58_init,
    .instance_size = sizeof(CS42L58State),
    .class_init    = cs42l58_class_init,
};

static void cs42l58_register_types(void)
{
    type_register_static(&cs42l58_info);
}

type_init(cs42l58_register_types)
