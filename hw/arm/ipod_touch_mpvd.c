#include "hw/arm/ipod_touch_mpvd.h"

/*
 * MPVD (video decode) register window, backing only.
 *
 * The device tree has mpvd@1600000 (arm-io base + 0x01600000 = 0x39600000,
 * 0x1000 bytes) but nothing was mapped there, so AppleMPVDDriver's power-state
 * path took a synchronous external abort the first time it touched the block --
 * the "Memory access exception" panic with fsr=0x808, far=0xec3fd01c (the
 * driver's iomap of this window, offset 0x1c). That happens right after
 * "enabling idle sleep", so every boot died once the device went idle.
 *
 * We do not emulate video decode; there is no decoding to do. All this needs to
 * do is exist and behave like plain registers so the power-state write lands
 * somewhere instead of faulting.
 */

static uint64_t ipod_touch_mpvd_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMPVDState *s = (IPodTouchMPVDState *)opaque;

    if (addr + 4 > MPVD_REG_SIZE) {
        return 0;
    }
    return s->regs[addr / 4];
}

static void ipod_touch_mpvd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMPVDState *s = (IPodTouchMPVDState *)opaque;

    if (addr + 4 > MPVD_REG_SIZE) {
        return;
    }
    s->regs[addr / 4] = (uint32_t)val;
}

static const MemoryRegionOps ipod_touch_mpvd_ops = {
    .read = ipod_touch_mpvd_read,
    .write = ipod_touch_mpvd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_mpvd_init(Object *obj)
{
    IPodTouchMPVDState *s = IPOD_TOUCH_MPVD(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_mpvd_ops, s,
                          TYPE_IPOD_TOUCH_MPVD, MPVD_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void ipod_touch_mpvd_class_init(ObjectClass *klass, void *data)
{
}

static const TypeInfo ipod_touch_mpvd_type_info = {
    .name = TYPE_IPOD_TOUCH_MPVD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMPVDState),
    .instance_init = ipod_touch_mpvd_init,
    .class_init = ipod_touch_mpvd_class_init,
};

static void ipod_touch_mpvd_register_types(void)
{
    type_register_static(&ipod_touch_mpvd_type_info);
}

type_init(ipod_touch_mpvd_register_types)
