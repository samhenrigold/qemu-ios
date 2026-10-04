/*
 * S5L8930 DRAM controller, early boot register interface.
 *
 * iBSS trains two DLLs via +0x140/+0x180 and polls bits 0/1 in
 * their status registers (+4), then uses the 10-bit delay at bits 16..25.
 * Emulated RAM has no analogue timing: a started DLL locks immediately
 * with a stable midpoint delay. Timing/configuration registers read back.
 * This models initialization, not DRAM power or timing behaviour.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"

#define TYPE_S5L8930_DMC "s5l8930.dmc"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8930DMCState, S5L8930_DMC)

struct S5L8930DMCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x1000 / 4];
};

static uint64_t dmc_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8930DMCState *s = opaque;
    return s->regs[off / 4];
}

static void dmc_write(void *opaque, hwaddr off, uint64_t value,
                      unsigned size)
{
    S5L8930DMCState *s = opaque;
    if (off == 0x144 || off == 0x184) {
        return; /* DLL status is read-only. */
    }
    s->regs[off / 4] = value;
    if (off == 0x140 || off == 0x180) {
        s->regs[(off + 4) / 4] = (value & 1) ? (0x100u << 16) | 3 : 0;
    }
}

static const MemoryRegionOps dmc_ops = {
    .read = dmc_read,
    .write = dmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dmc_reset(DeviceState *dev)
{
    memset(S5L8930_DMC(dev)->regs, 0, sizeof(S5L8930_DMC(dev)->regs));
}

static void dmc_init(Object *obj)
{
    S5L8930DMCState *s = S5L8930_DMC(obj);
    memory_region_init_io(&s->iomem, obj, &dmc_ops, s,
                          TYPE_S5L8930_DMC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_dmc = {
    .name = TYPE_S5L8930_DMC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8930DMCState, 0x1000 / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void dmc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_dmc;
    device_class_set_legacy_reset(dc, dmc_reset);
}

static const TypeInfo dmc_info = {
    .name = TYPE_S5L8930_DMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930DMCState),
    .instance_init = dmc_init,
    .class_init = dmc_class_init,
};

static void dmc_register_types(void)
{
    type_register_static(&dmc_info);
}
type_init(dmc_register_types)
