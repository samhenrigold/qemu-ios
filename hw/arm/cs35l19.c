/* CS35L19 speaker-amplifier register port. Uses the same Cirrus I2C MAP /
 * autoincrement framing as qemu-ios's CS42L58 port. The original N42 driver
 * reads registers 1..3 and checks 35 a1 9x (11A465, 80a23690).
 * Analog, clock, power and interrupt side effects remain unqualified. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_CS35L19 "cs35l19"
OBJECT_DECLARE_SIMPLE_TYPE(CS35L19State, CS35L19)
struct CS35L19State {
    I2CSlave parent_obj;
    uint8_t regs[128], address;
    bool have_map, autoincrement, trace_regs;
    unsigned traced;
};
static int cs35l19_event(I2CSlave *dev, enum i2c_event event)
{
    if (event == I2C_START_SEND) {
        CS35L19(dev)->have_map = false;
    }
    return 0;
}
static uint8_t cs35l19_recv(I2CSlave *dev)
{
    CS35L19State *s = CS35L19(dev);
    uint8_t result = s->regs[s->address];
    if (s->trace_regs && s->traced++ < 512) {
        qemu_log("cs35l19 R %02x -> %02x\n", s->address, result);
    }
    if (s->autoincrement) {
        s->address = (s->address + 1) & 0x7f;
    }
    return result;
}
static int cs35l19_send(I2CSlave *dev, uint8_t value)
{
    CS35L19State *s = CS35L19(dev);
    if (!s->have_map) {
        s->address = value & 0x7f;
        s->autoincrement = value & 0x80;
        s->have_map = true;
        return 0;
    }
    if (s->trace_regs && s->traced++ < 512) {
        qemu_log("cs35l19 W %02x <- %02x\n", s->address, value);
    }
    if (s->address < 1 || s->address > 3) {
        s->regs[s->address] = value;
    }
    if (s->autoincrement) {
        s->address = (s->address + 1) & 0x7f;
    }
    return 0;
}
static void cs35l19_reset(DeviceState *dev)
{
    CS35L19State *s = CS35L19(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[1] = 0x35;
    s->regs[2] = 0xa1;
    s->regs[3] = 0x90;
    s->address = 0;
    s->have_map = s->autoincrement = false;
    s->traced = 0;
}
static const VMStateDescription vmstate_cs35l19 = {
    .name = TYPE_CS35L19,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){
        VMSTATE_I2C_SLAVE(parent_obj, CS35L19State),
        VMSTATE_UINT8_ARRAY(regs, CS35L19State, 128),
        VMSTATE_UINT8(address, CS35L19State),
        VMSTATE_BOOL(have_map, CS35L19State),
        VMSTATE_BOOL(autoincrement, CS35L19State), VMSTATE_END_OF_LIST()}};
static const Property cs35l19_properties[] = {
    DEFINE_PROP_BOOL("trace-regs", CS35L19State, trace_regs, false),
};
static void cs35l19_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->event = cs35l19_event;
    ic->recv = cs35l19_recv;
    ic->send = cs35l19_send;
    device_class_set_props(dc, cs35l19_properties);
    device_class_set_legacy_reset(dc, cs35l19_reset);
    dc->vmsd = &vmstate_cs35l19;
}
static const TypeInfo cs35l19_info = {
    .name = TYPE_CS35L19,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(CS35L19State),
    .class_init = cs35l19_class_init,
};
static void cs35l19_register_types(void)
{
    type_register_static(&cs35l19_info);
}
type_init(cs35l19_register_types)
