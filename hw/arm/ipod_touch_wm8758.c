/*
 * Wolfson WM8758 audio codec, 2-wire control interface (iPod touch 1G, I2C1
 * address 0x1A: the device tree's audio0@1A, AppleWM8758Audio).
 *
 * The control port is write-only: every transfer is two bytes, B[15:9] the
 * register address and B[8:0] its 9-bit value (WM8758 datasheet, "Control
 * interface"). Writing register 0 resets the register file. There is no
 * read-back, so recv answers 0xff like an undriven bus.
 *
 * Only the control port is modeled. The kernel's driver needs the codec to
 * ACK its setup writes before it publishes the IOAudio device mediaserverd
 * waits for; the analog side (headphone output) is not modeled.
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_WM8758 "wm8758"
OBJECT_DECLARE_SIMPLE_TYPE(WM8758State, WM8758)

#define WM8758_NREGS 64

struct WM8758State {
    I2CSlave i2c;
    uint8_t nbytes;
    uint8_t first;
    uint16_t reg[WM8758_NREGS];
};

static int wm8758_event(I2CSlave *i2c, enum i2c_event event)
{
    WM8758(i2c)->nbytes = 0;
    return 0;
}

static int wm8758_send(I2CSlave *i2c, uint8_t data)
{
    WM8758State *s = WM8758(i2c);

    if (s->nbytes++ == 0) {
        s->first = data;
        return 0;
    }
    if (s->nbytes == 2) {
        unsigned r = s->first >> 1;
        uint16_t v = ((s->first & 1) << 8) | data;
        if (r == 0) {
            memset(s->reg, 0, sizeof(s->reg));
        } else if (r < WM8758_NREGS) {
            s->reg[r] = v;
        }
    }
    return 0;
}

static uint8_t wm8758_recv(I2CSlave *i2c)
{
    return 0xff;
}

static void wm8758_reset(DeviceState *dev)
{
    WM8758State *s = WM8758(dev);

    s->nbytes = 0;
    memset(s->reg, 0, sizeof(s->reg));
}

static const VMStateDescription vmstate_wm8758 = {
    .name = TYPE_WM8758,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, WM8758State),
        VMSTATE_UINT8(nbytes, WM8758State),
        VMSTATE_UINT8(first, WM8758State),
        VMSTATE_UINT16_ARRAY(reg, WM8758State, WM8758_NREGS),
        VMSTATE_END_OF_LIST()
    }
};

static void wm8758_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_wm8758;
    device_class_set_legacy_reset(dc, wm8758_reset);
    k->event = wm8758_event;
    k->send = wm8758_send;
    k->recv = wm8758_recv;
}

static const TypeInfo wm8758_info = {
    .name          = TYPE_WM8758,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(WM8758State),
    .class_init    = wm8758_class_init,
};

static void wm8758_register_types(void)
{
    type_register_static(&wm8758_info);
}

type_init(wm8758_register_types)
