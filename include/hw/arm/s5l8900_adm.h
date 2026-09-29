#ifndef HW_ARM_S5L8900_ADM_H
#define HW_ARM_S5L8900_ADM_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/arm/s5l8900_fmc.h"

#define TYPE_S5L8900_ADM "s5l8900.adm"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900ADMState, S5L8900_ADM)

#define ADM_CTRL 0x0
#define ADM_CTRL2 0x4
#define ADM_CODE_SEC_ADDR 0x50
#define ADM_DATA1_SEC_ADDR 0x84
#define ADM_DATA2_SEC_ADDR 0x88
#define ADM_DATA3_SEC_ADDR 0x8C

/* Command block the driver writes into its data2 section. */
#define ADM_CMD_OFFSET       (0x1104 + 0x24)
#define ADM_CMD_NPAGES       (0x1104 + 0x28)   /* uint16, big-endian */
#define ADM_CMD_BANKS        (0x1104 + 0x44)   /* uint8 per page */
#define ADM_CMD_PAGES        (0x1104 + 0x244)  /* uint32 big-endian per page */

#define ADM_CMD_READ_SEQ   0x200   /* n pages, same page on every bank */
#define ADM_CMD_READ       0x300   /* one page, or a scattered list */
#define ADM_CMD_WRITE      0x500

typedef struct S5L8900ADMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t code_sec_addr;
    uint32_t data1_sec_addr;
    uint32_t data2_sec_addr;
    uint32_t data3_sec_addr;

    MemoryRegion *downstream;
    AddressSpace downstream_as;

    S5L8900FMCState *fmc;   /* set by the board */
} S5L8900ADMState;

#endif
