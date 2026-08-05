#include "hw/arm/ipod_touch_chipid.h"
#include "qemu/log.h"

static uint64_t ipod_touch_chipid_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case CHIPID_UNKNOWN1:
            /*
             * Bit 5 is the production-mode fuse: the bootrom reads it at
             * 0x3d100004 and shifts it out (bootrom_240_4 +0x3d44). With it
             * set, every img3 must carry a signature that verifies against the
             * chain in its CERT tag. Clearing it demotes the part to a
             * development unit, which is the documented way to run images
             * whose SHSH the device cannot validate.
             *
             * IT_DEV_MODE=1 clears it. Off by default so the stock NOR keeps
             * booting through the real verification path.
             */
            if (getenv("IT_DEV_MODE")) {
                return 0;
            }
            return (1 << 5); // ind5 = production mode
        case CHIPID_INFO:
            /*
             * Bit 2 is the security-domain (secure-mode) fuse. Clearing the
             * production bit alone (IT_DEV_MODE, above) demotes to CPFM 0x01
             * "secure development"; that was tried and the bootrom still
             * rejected an unsigned LLB. IT_INSECURE_MODE=1 *also* clears this
             * bit, i.e. CPFM 0x00 "insecure development" - the fully permissive
             * fuse state - to test whether the signature/personalisation checks
             * key on secure rather than production.
             */
            if (getenv("IT_INSECURE_MODE")) {
                return (0x8720 << 16);
            }
            return (0x8720 << 16) | (1 << 2); // ind16 = chipid, ind2 = security domain,
        case CHIPID_UNKNOWN2:
            return 0;
        case CHIPID_UNKNOWN3:
            return 0;
        default:
            /*
             * Offset 0 is inside the 0x14 region but has no named register, and
             * a guest read of it used to hw_error() the whole process. Read as
             * 0, like every other unhandled ChipID offset.
             */
            qemu_log_mask(LOG_UNIMP,
                          "%s: read from unknown chip ID register 0x%08x\n",
                          __func__, (uint32_t)addr);
    }

    return 0;
}

static void ipod_touch_chipid_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    /* ChipID is read-only fuses; there is no .write path on hardware. Swallow
     * stores (the ops table had no .write at all, so any store dispatched
     * through a NULL pointer). */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: write 0x%08x to read-only chip ID register 0x%08x\n",
                  __func__, (uint32_t)val, (uint32_t)addr);
}

static const MemoryRegionOps ipod_touch_chipid_ops = {
    .read = ipod_touch_chipid_read,
    .write = ipod_touch_chipid_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_chipid_init(Object *obj)
{
    IPodTouchChipIDState *s = IPOD_TOUCH_CHIPID(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_chipid_ops, s, TYPE_IPOD_TOUCH_CHIPID, 0x14);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void ipod_touch_chipid_class_init(ObjectClass *klass, void *data)
{
    
}

static const TypeInfo ipod_touch_chipid_type_info = {
    .name = TYPE_IPOD_TOUCH_CHIPID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchChipIDState),
    .instance_init = ipod_touch_chipid_init,
    .class_init = ipod_touch_chipid_class_init,
};

static void ipod_touch_chipid_register_types(void)
{
    type_register_static(&ipod_touch_chipid_type_info);
}

type_init(ipod_touch_chipid_register_types)