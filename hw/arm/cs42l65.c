/*
 * CS42L65 SPI register port, qualified against the original N42 driver.
 *
 * AppleARMSPIAudio's read/write methods (11A465, 809ee348/809ee3f8):
 *   6d|6c, register>>8, register&7f | autoincrement<<7, length-1, data...
 * AppleCS42L65Audio's identity probe (80b78d18) reads registers 1..3 and
 * checks 42 a6 5x. Other registers currently retain writes. Clocking, analog
 * controls and interrupt/status side effects remain to be qualified; this
 * register-port model does not claim a working codec or audio stream.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"

#define TYPE_CS42L65 "cs42l65"
OBJECT_DECLARE_SIMPLE_TYPE(CS42L65State, CS42L65)
#define CS42L65_REGS 65536

struct CS42L65State {
    SSIPeripheral parent_obj;
    uint8_t regs[CS42L65_REGS];
    uint8_t command, header_pos, high;
    bool autoincrement;
    uint16_t address, remaining;
    unsigned bad_commands;
};

static uint32_t cs42l65_transfer(SSIPeripheral *dev, uint32_t value)
{
    CS42L65State *s = CS42L65(dev);
    uint8_t byte = value;
    if (s->remaining) {
        uint8_t result = 0;
        if (s->command == 0x6d) {
            result = s->regs[s->address];
        } else if (s->address < 1 || s->address > 3) {
            s->regs[s->address] = byte;
        }
        if (s->autoincrement) {
            s->address = (s->address + 1) & (CS42L65_REGS - 1);
        }
        if (!--s->remaining) {
            s->header_pos = 0;
        }
        return result;
    }
    switch (s->header_pos) {
    case 0:
        if (byte != 0x6c && byte != 0x6d) {
            if (s->bad_commands++ < 8) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "cs42l65: unknown command 0x%02x\n", byte);
            }
            return 0;
        }
        s->command = byte;
        break;
    case 1:
        s->high = byte;
        break;
    case 2:
        s->address = ((uint16_t)s->high << 8) | (byte & 0x7f);
        s->autoincrement = byte & 0x80;
        break;
    case 3:
        s->remaining = (uint16_t)byte + 1;
        break;
    default:
        g_assert_not_reached();
    }
    s->header_pos++;
    return 0;
}

static void cs42l65_reset(DeviceState *dev)
{
    CS42L65State *s = CS42L65(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[1] = 0x42;
    s->regs[2] = 0xa6;
    s->regs[3] = 0x50;
    s->command = s->header_pos = s->high = 0;
    s->autoincrement = false;
    s->address = s->remaining = 0;
    s->bad_commands = 0;
}

static const VMStateDescription vmstate_cs42l65 = {
    .name = TYPE_CS42L65,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){
        VMSTATE_UINT8_ARRAY(regs, CS42L65State, CS42L65_REGS),
        VMSTATE_UINT8(command, CS42L65State),
        VMSTATE_UINT8(header_pos, CS42L65State),
        VMSTATE_UINT8(high, CS42L65State),
        VMSTATE_BOOL(autoincrement, CS42L65State),
        VMSTATE_UINT16(address, CS42L65State),
        VMSTATE_UINT16(remaining, CS42L65State), VMSTATE_END_OF_LIST()}};

static void cs42l65_realize(SSIPeripheral *dev, Error **errp)
{
    cs42l65_reset(DEVICE(dev));
}

static void cs42l65_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);
    sc->realize = cs42l65_realize;
    sc->transfer = cs42l65_transfer;
    sc->cs_polarity = SSI_CS_NONE; /* packets carry their own byte count */
    device_class_set_legacy_reset(dc, cs42l65_reset);
    dc->vmsd = &vmstate_cs42l65;
}

static const TypeInfo cs42l65_info = {
    .name = TYPE_CS42L65,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(CS42L65State),
    .class_init = cs42l65_class_init,
};
static void cs42l65_register_types(void)
{
    type_register_static(&cs42l65_info);
}
type_init(cs42l65_register_types)
