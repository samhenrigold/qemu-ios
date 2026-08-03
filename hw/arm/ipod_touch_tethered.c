#include "hw/arm/ipod_touch_tethered.h"
#include "migration/vmstate.h"

/* IT_TETHERED_TRACE=1 logs every I2C access to the simulated demo card. */
static bool tethered_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_TETHERED_TRACE") != NULL;
    }
    return on;
}

static int ipod_touch_tethered_event(I2CSlave *i2c, enum i2c_event event)
{
    return 0;
}

static uint8_t ipod_touch_tethered_recv(I2CSlave *i2c)
{
    IPodTouchTetheredState *s = IPOD_TOUCH_TETHERED(i2c);
    uint8_t res = IT_TETHERED_MAGIC; /* identification byte the kext requires */

    if (tethered_trace()) {
        fprintf(stderr, "[tethered] read reg 0x%02x -> 0x%02x\n", s->cmd, res);
    }
    s->cmd += 1;
    return res;
}

static int ipod_touch_tethered_send(I2CSlave *i2c, uint8_t data)
{
    IPodTouchTetheredState *s = IPOD_TOUCH_TETHERED(i2c);
    if (tethered_trace()) {
        fprintf(stderr, "[tethered] write 0x%02x (prev reg 0x%02x)\n", data, s->cmd);
    }
    s->cmd = data;
    return 0;
}

static void ipod_touch_tethered_init(Object *obj)
{
}

static const VMStateDescription vmstate_ipod_touch_tethered = {
    .name = "ipod-touch-tethered",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, IPodTouchTetheredState),
        VMSTATE_UINT8(cmd, IPodTouchTetheredState),
        VMSTATE_UINT8_ARRAY(control, IPodTouchTetheredState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_tethered_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_tethered;
    k->event = ipod_touch_tethered_event;
    k->recv = ipod_touch_tethered_recv;
    k->send = ipod_touch_tethered_send;
}

static const TypeInfo ipod_touch_tethered_info = {
    .name          = TYPE_IPOD_TOUCH_TETHERED,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = ipod_touch_tethered_init,
    .instance_size = sizeof(IPodTouchTetheredState),
    .class_init    = ipod_touch_tethered_class_init,
};

static void ipod_touch_tethered_register_types(void)
{
    type_register_static(&ipod_touch_tethered_info);
}

type_init(ipod_touch_tethered_register_types)
