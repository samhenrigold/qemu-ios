#ifndef HW_ARM_S5L8900_NAND_ECC_H
#define HW_ARM_S5L8900_NAND_ECC_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"

#define NANDECC_DATA 0x4
#define NANDECC_ECC 0x8
#define NANDECC_START 0xC
#define NANDECC_STATUS 0x10
#define NANDECC_SETUP 0x14
#define NANDECC_CLEARINT 0x40

#define TYPE_S5L8900_NAND_ECC "s5l8900.nandecc"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900NandECCState, S5L8900_NAND_ECC)

typedef struct S5L8900NandECCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t data_addr;
    uint32_t ecc_addr;
    uint32_t setup;
} S5L8900NandECCState;

#endif
