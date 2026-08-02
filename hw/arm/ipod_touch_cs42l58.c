#include "hw/arm/ipod_touch_cs42l58.h"

/*
 * Cirrus CS42L58 stereo audio codec, I2C address 0x4A (device tree:
 * /device-tree/arm-io/i2c0/audio0, compatible "audio-control,cs42l58").
 *
 * The device is a plain 7-bit-address register file: the master writes a
 * memory-address pointer (MAP) byte, optionally with the auto-increment bit
 * 0x80 set, then either writes data bytes or issues a repeated START and
 * reads them back. Every control register reads back what was written --
 * that read/write round-trip is what the driver's gain/volume code relies on.
 */
static bool codec_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_CODEC_TRACE") != NULL;
    }
    return on;
}

#define CS42L58_MAP_INCR   0x80
#define CS42L58_REG_CHIPID 0x01

static int cs42l58_event(I2CSlave *i2c, enum i2c_event event)
{
    CS42L58State *s = CS42L58(i2c);

    if (event == I2C_START_SEND) {
        s->have_cmd = false;   /* next byte written is the MAP */
    }
    return 0;
}

static uint8_t cs42l58_recv(I2CSlave *i2c)
{
    CS42L58State *s = CS42L58(i2c);
    uint8_t reg = s->cmd & 0x7f;
    uint8_t res = s->regs[reg];

    if (codec_trace()) {
        fprintf(stderr, "CODEC R %02x -> %02x\n", reg, res);
    }
    if (s->autoinc) {
        s->cmd = (reg + 1) & 0x7f;
    }
    return res;
}

static int cs42l58_send(I2CSlave *i2c, uint8_t data)
{
    CS42L58State *s = CS42L58(i2c);

    if (!s->have_cmd) {
        s->autoinc = !!(data & CS42L58_MAP_INCR);
        s->cmd = data & 0x7f;
        s->have_cmd = true;
        return 0;
    }

    if (codec_trace()) {
        fprintf(stderr, "CODEC W %02x <- %02x\n", (uint8_t)(s->cmd & 0x7f), data);
    }
    if ((s->cmd & 0x7f) != CS42L58_REG_CHIPID) {
        s->regs[s->cmd & 0x7f] = data;
    }
    if (s->autoinc) {
        s->cmd = ((s->cmd & 0x7f) + 1) & 0x7f;
    }
    return 0;
}

static void cs42l58_reset(DeviceState *dev)
{
    CS42L58State *s = CS42L58(dev);
    const char *id = getenv("IT_CODEC_ID");

    memset(s->regs, 0, sizeof(s->regs));
    s->cmd = 0;
    s->have_cmd = false;
    s->autoinc = false;
    /* Chip ID / revision: read-only on the real part. */
    s->regs[CS42L58_REG_CHIPID] = id ? (uint8_t)strtoul(id, NULL, 0) : 0xe0;
}

static void cs42l58_init(Object *obj)
{

}

static void cs42l58_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->event = cs42l58_event;
    k->recv = cs42l58_recv;
    k->send = cs42l58_send;
    dc->reset = cs42l58_reset;
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
