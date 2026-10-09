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
    bool backlight;     /* "backlight": this SWI drives the backlight (the S5L8930's) */
    uint8_t iset;               /* "iset-command": the DT's command-iset */
    uint32_t backlight_word;    /* the last current-set word sent, on either channel */
};

/*
 * The A4's backlight level, for the host. AppleSamsungSWI (11D257 kext at 80a36000) sends a word either
 * synchronously (0x18, started through 0x14: it waits for 0x14's busy bit first) or not (0x20, started
 * through 0x1c), the same encoding on both (80a376d0): bits 0-6 are data 0-6, bit 7 is always 1, bits 8-11
 * are data 7-10 and bits 12-14 the command: the DT's command-iset for the backlight current (the board's
 * "iset-command": 1 on the K48, 3 on the N81 and N90) or command-vsel (6) for the core voltage.
 * AppleARMBacklight sends its enable and disable levels on the synchronous channel and its ramps on the
 * other, so the level is the last current-set word started on either. n90 iBoot's 0x3cc1 decodes to the
 * 0x641 the kernel then logs; 4.x/5.x ramp to 0x7ff, 6.x/7.x step through the DT's backlight-table
 * (0x7b3 at the top on the n90).
 */
static int swi_backlight_level(void *opaque)
{
    IPodSWIState *s = opaque;
    uint32_t word = s->backlight_word;
    return word && ((word >> 12) & 7) == s->iset ? (int)((word & 0x7f) | ((word >> 1) & 0x780)) : -1;
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
        uint32_t word = s->regs[(addr + 4) / 4];
        if (s->backlight && ((word >> 12) & 7) == s->iset) {
            s->backlight_word = word;
        }
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
    s->backlight_word = 0;
}

static const VMStateDescription vmstate_swi = {
    .name = TYPE_IPOD_SWI,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodSWIState, 0x1000 / 4),
        VMSTATE_UINT32_V(backlight_word, IPodSWIState, 2),
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
    DEFINE_PROP_UINT8("iset-command", IPodSWIState, iset, 0),
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
