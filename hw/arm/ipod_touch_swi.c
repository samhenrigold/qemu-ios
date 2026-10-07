/* S5L8720 single-wire core-voltage interface, observed in 7E18.
 * AppleS5L8720XSWI writes commands at 0x18/0x20 and starts the corresponding
 * channel through 0x14/0x1c. Bit 0 is busy (c05f47b8, c05f47e0, c05f48a4);
 * retaining it as RAM deadlocks performance-state changes after video decode.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/ipod_touch_lcd.h"

#define TYPE_IPOD_SWI "ipodtouch.swi"
OBJECT_DECLARE_SIMPLE_TYPE(IPodSWIState, IPOD_SWI)

struct IPodSWIState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x1000 / 4];
    bool backlight;     /* "backlight": channel 0x20 drives the backlight (the S5L8930's) */
};

/*
 * The A4's backlight level, for the host: the last word sent on channel 0x20 (started through 0x1c) is 0b111
 * followed by the 11-bit level code. 4.x/5.x ramp it to 0x7ff, 6.x/7.x step it through the DT's
 * backlight-table (0x7b3 at the top on the n90); channel 0x18 is the core voltage.
 * ponytail: the level as last sent. The driver's state across the D1815's WLED enable (0x50 bit 6) is not
 * modeled: 7.1.2 can enable it at a new level (its kernel logs one) without a word on this channel.
 */
static int swi_backlight_level(void *opaque)
{
    uint32_t word = ((IPodSWIState *)opaque)->regs[0x20 / 4];
    return (word >> 11) == 7 ? (int)(word & 0x7ff) : -1;
}

static uint64_t swi_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodSWIState *s = opaque;
    return s->regs[addr / 4];
}

static void swi_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    IPodSWIState *s = opaque;
    s->regs[addr / 4] = value;
    if ((addr == 0x14 || addr == 0x1c) && (value == 1 || value == 3)) {
        /* ponytail: synchronous voltage-command completion. Model bus timing
         * if software needs pulse timing; host CPU voltage is never changed. */
        s->regs[addr / 4] &= ~1u;
    }
}

static const MemoryRegionOps swi_ops = {
    .read = swi_read,
    .write = swi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, .unaligned = false },
};

static void swi_reset(DeviceState *dev)
{
    IPodSWIState *s = IPOD_SWI(dev);
    if (s->backlight) {
        ios_backlight_register(swi_backlight_level, s);
    }
    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_swi = {
    .name = TYPE_IPOD_SWI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodSWIState, 0x1000 / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void swi_init(Object *obj)
{
    IPodSWIState *s = IPOD_SWI(obj);
    memory_region_init_io(&s->iomem, obj, &swi_ops, s, TYPE_IPOD_SWI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property swi_properties[] = {
    DEFINE_PROP_BOOL("backlight", IPodSWIState, backlight, false),
};

static void swi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, swi_properties);
    device_class_set_legacy_reset(dc, swi_reset);
    dc->vmsd = &vmstate_swi;
}

static const TypeInfo swi_info = {
    .name = TYPE_IPOD_SWI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodSWIState),
    .instance_init = swi_init,
    .class_init = swi_class_init,
};

static void swi_register_types(void) { type_register_static(&swi_info); }
type_init(swi_register_types)
