/*
 * S5L8920 DART ('dart,s5l8920x', AppleH2PDART-27): the IOMMU the M2 CLCD,
 * scaler and TV-out use (dart0) and JPEG/venc (dart1). One 64 MiB IOVA window
 * at 0x3c000000, 16 segments of 4 MiB; segment n's page table is a 4 KiB
 * array of PTEs for 4 KiB pages. Both hold DRAM offsets, not addresses: the
 * kext masks them with 0x0ffff000 (STE 0x0ffffffe), the status dump ORs
 * 0x40000000 back in, so PA = DRAM base + (entry & 0x0ffff000); bit 0 valid.
 *
 * Registers, from the N18 8C148 kext (activation 8065d124, status print
 * 8065d3c0, PTE writes 8065e544):
 *   +0x00 command: 0x702 in the low bits = flush; seg << 22 | 1 selects an
 *         STE for reading at +0x04
 *   +0x04 data
 *   +0x08 STE port: bits 8-11 segment, 12-31 page table PA, bit 0 valid
 *   +0x0c config: bit 31 enable
 *   +0x10 error status (W1C), +0x14 miss counter
 * Addresses outside the window, or with the DART disabled, pass through.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"

#define TYPE_S5L8920_DART "s5l8920.dart"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8920DartState, S5L8920_DART)

#define DART_CMD            0x00
#define DART_DATA           0x04
#define DART_STE            0x08
#define DART_CONFIG         0x0c
#define DART_ERROR          0x10
#define DART_SIZE           0x1000
#define DART_CONFIG_ENABLE  (1u << 31)
#define DART_WINDOW         0x3c000000u
#define DART_SEGS           16
#define DART_SEG_SHIFT      22
#define DART_DRAM           0x40000000u
#define DART_PA_MASK        0x0ffff000u

struct S5L8920DartState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[DART_SIZE / 4];
    uint32_t ste[DART_SEGS];
};

/* va -> PA, or va itself when it is not a DART address; -1 for an invalid PTE. */
hwaddr s5l8920_dart_xlate(void *opaque, uint32_t va, unsigned sid)
{
    S5L8920DartState *s = opaque;
    uint32_t off = va - DART_WINDOW, ste, pte;

    if (!(s->regs[DART_CONFIG / 4] & DART_CONFIG_ENABLE) || va < DART_WINDOW) {
        return va;
    }
    ste = s->ste[off >> DART_SEG_SHIFT];
    if (!(ste & 1)) {
        return -1;
    }
    pte = address_space_ldl_le(&address_space_memory,
                               DART_DRAM + (ste & DART_PA_MASK) + ((off >> 12) & 0x3ff) * 4,
                               MEMTXATTRS_UNSPECIFIED, NULL);
    return (pte & 1) ? DART_DRAM + (pte & DART_PA_MASK) + (va & 0xfff) : -1;
}

static uint64_t dart_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8920DartState *s = opaque;

    return s->regs[addr / 4];
}

static void dart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8920DartState *s = opaque;

    switch (addr) {
    case DART_CMD:
        if ((val & 0xf) == 1) {             /* select an STE for reading */
            s->regs[DART_DATA / 4] = s->ste[(val >> DART_SEG_SHIFT) & (DART_SEGS - 1)];
        }
        s->regs[addr / 4] = val;
        return;
    case DART_STE:
        s->ste[(val >> 8) & (DART_SEGS - 1)] = val;
        s->regs[addr / 4] = val;
        return;
    case DART_ERROR:
        s->regs[addr / 4] &= ~val;
        return;
    default:
        s->regs[addr / 4] = val;
    }
}

static const MemoryRegionOps dart_ops = {
    .read = dart_read,
    .write = dart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8920_dart_reset(DeviceState *dev)
{
    S5L8920DartState *s = S5L8920_DART(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ste, 0, sizeof(s->ste));
}

static void s5l8920_dart_init(Object *obj)
{
    S5L8920DartState *s = S5L8920_DART(obj);

    memory_region_init_io(&s->iomem, obj, &dart_ops, s, TYPE_S5L8920_DART, DART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_s5l8920_dart = {
    .name = TYPE_S5L8920_DART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8920DartState, DART_SIZE / 4),
        VMSTATE_UINT32_ARRAY(ste, S5L8920DartState, DART_SEGS),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8920_dart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8920_dart;
    device_class_set_legacy_reset(dc, s5l8920_dart_reset);
}

static const TypeInfo s5l8920_dart_info = {
    .name          = TYPE_S5L8920_DART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8920DartState),
    .instance_init = s5l8920_dart_init,
    .class_init    = s5l8920_dart_class_init,
};

static void s5l8920_dart_register_types(void)
{
    type_register_static(&s5l8920_dart_info);
}

type_init(s5l8920_dart_register_types)
