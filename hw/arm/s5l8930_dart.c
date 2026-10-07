/*
 * S5L8930 DART ('dart,s5l8930x'): the IOMMU register block, one per client
 * group. dart2 (display: CLCD, RGBOUT, scaler) is part of the display model;
 * dart1 (0x88d00000: ISP, JPEG, VENC) is TYPE_S5L8930_DART here. Same driver,
 * same programming: AppleS5L8930XDART clears every stream's segment table at
 * start (DATA = 0, then TLB_OP = seg << 22 | sid << 8 | 5, then reads TLB_OP
 * for busy), flushes (op 2), sets +0x0c/+0x18/+0x1000 and W1Cs +0x10.
 *
 * STE = page-table PA, PTE = PA | 1. A segment with no table passes the IOVA
 * through untranslated (iBoot's framebuffer, the kernel's identity
 * "transition mapping").
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"

#define DART_TLB_OP         0x00      /* bit3 busy; op nibble 4 = read STE, 5 = write STE */
#define DART_DATA           0x08
#define DART_ERROR_STATUS   0x10      /* W1C */

static uint64_t dart_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8930Dart *d = opaque;

    return addr == DART_TLB_OP ? 0 : d->regs[addr / 4];   /* never busy */
}

static void dart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8930Dart *d = opaque;
    unsigned sid = (val >> 8) & 0xf, seg = (val >> 22) & 0x3f;

    switch (addr) {
    case DART_TLB_OP:
        if (sid >= S5L8930_DART_SIDS) {
            break;
        }
        if ((val & 0xf) == 5) {
            d->ste[sid][seg] = d->regs[DART_DATA / 4];
        } else if ((val & 0xf) == 4) {
            d->regs[DART_DATA / 4] = d->ste[sid][seg];
        }
        break;
    case DART_ERROR_STATUS:
        d->regs[addr / 4] &= ~val;
        break;
    default:
        d->regs[addr / 4] = val;
    }
}

const MemoryRegionOps s5l8930_dart_ops = {
    .read = dart_read,
    .write = dart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

hwaddr s5l8930_dart_xlate(S5L8930Dart *d, unsigned sid, uint32_t va)
{
    uint32_t ste = d->ste[sid][(va >> 22) & 0x3f] & ~0xfffu;
    uint32_t pte;

    if (!ste) {
        return va;
    }
    pte = address_space_ldl_le(&address_space_memory, ste + ((va >> 12) & 0x3ff) * 4, MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(pte & 1)) {
        qemu_log_mask(LOG_GUEST_ERROR, "dart: invalid PTE 0x%08x for sid %u iova 0x%08x\n", pte, sid, va);
    }
    return (pte & 1) ? ((pte & ~0xfffu) | (va & 0xfff)) : (hwaddr)-1;
}

/* ---- a standalone DART (dart1) ------------------------------------------ */

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930DartState, S5L8930_DART)

struct S5L8930DartState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    S5L8930Dart dart;
};

static void s5l8930_dart_reset(DeviceState *dev)
{
    S5L8930DartState *s = S5L8930_DART(dev);

    memset(&s->dart, 0, sizeof(s->dart));
}

static void s5l8930_dart_init(Object *obj)
{
    S5L8930DartState *s = S5L8930_DART(obj);

    memory_region_init_io(&s->mr, obj, &s5l8930_dart_ops, &s->dart, "s5l8930.dart", S5L8930_DART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
}

static const VMStateDescription vmstate_s5l8930_dart = {
    .name = "s5l8930.dart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(dart.regs, S5L8930DartState, S5L8930_DART_SIZE / 4),
        VMSTATE_UINT32_2DARRAY(dart.ste, S5L8930DartState, S5L8930_DART_SIDS, S5L8930_DART_SEGS),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8930_dart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8930_dart_reset);
    dc->vmsd = &vmstate_s5l8930_dart;
}

static const TypeInfo s5l8930_dart_info = {
    .name = TYPE_S5L8930_DART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930DartState),
    .instance_init = s5l8930_dart_init,
    .class_init = s5l8930_dart_class_init,
};

static void s5l8930_dart_register_types(void)
{
    type_register_static(&s5l8930_dart_info);
}

type_init(s5l8930_dart_register_types)
