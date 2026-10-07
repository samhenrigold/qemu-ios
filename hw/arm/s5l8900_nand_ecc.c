/*
 * S5L8900 NAND ECC block (iPod touch 1G), 0x38F00000. The guest starts a
 * check, waits for the interrupt and reads a status that is always "no
 * error": the page store holds the real bytes, so there is nothing to correct.
 * Stub (fidelity class S); a real model would compute the Hamming/RS syndrome
 * over the DATA/ECC buffers named by +0x4/+0x8.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/arm/s5l8900_nand_ecc.h"

static uint64_t s5l8900_nand_ecc_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8900NandECCState *s = opaque;

    switch (addr) {
    case NANDECC_DATA:  return s->data_addr;
    case NANDECC_ECC:   return s->ecc_addr;
    case NANDECC_SETUP: return s->setup;
    case NANDECC_STATUS:
    default:
        return 0;
    }
}

static void s5l8900_nand_ecc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8900NandECCState *s = opaque;

    switch (addr) {
    case NANDECC_DATA:  s->data_addr = val; break;
    case NANDECC_ECC:   s->ecc_addr = val; break;
    case NANDECC_SETUP: s->setup = val; break;
    case NANDECC_START:
        qemu_irq_raise(s->irq);
        break;
    case NANDECC_CLEARINT:
        qemu_irq_lower(s->irq);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps s5l8900_nand_ecc_ops = {
    .read = s5l8900_nand_ecc_read,
    .write = s5l8900_nand_ecc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_nand_ecc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    S5L8900NandECCState *s = S5L8900_NAND_ECC(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_nand_ecc_ops, s, "s5l8900.nandecc", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void s5l8900_nand_ecc_reset(DeviceState *d)
{
    S5L8900NandECCState *s = S5L8900_NAND_ECC(d);

    s->data_addr = 0;
    s->ecc_addr = 0;
    s->setup = 0;
    qemu_irq_lower(s->irq);
}

static void s5l8900_nand_ecc_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), s5l8900_nand_ecc_reset);
}

static const TypeInfo s5l8900_nand_ecc_info = {
    .name          = TYPE_S5L8900_NAND_ECC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900NandECCState),
    .instance_init = s5l8900_nand_ecc_init,
    .class_init    = s5l8900_nand_ecc_class_init,
};

static void s5l8900_nand_ecc_register_types(void)
{
    type_register_static(&s5l8900_nand_ecc_info);
}

type_init(s5l8900_nand_ecc_register_types)
