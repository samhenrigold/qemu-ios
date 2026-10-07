#ifndef HW_ARM_S5L8900_ADM_H
#define HW_ARM_S5L8900_ADM_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/arm/s5l8900_fmc.h"

#define TYPE_S5L8900_ADM "s5l8900.adm"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900ADMState, S5L8900_ADM)

#define ADM_CTRL 0x0
#define ADM_CTRL2 0x4
#define ADM_CODE_SEC_ADDR 0x50
#define ADM_DATA1_SEC_ADDR 0x84
#define ADM_DATA2_SEC_ADDR 0x88
#define ADM_DATA3_SEC_ADDR 0x8C
#define ADM_RESULT 0x30          /* four result words, see adm_run_command */
#define ADM_STATUS_CLEAN 0xFE    /* result[2] of a read that found an erased page */

/*
 * The transfer block ("TF", the firmware's own name) the driver writes into its
 * data2 section, offsets from its start. It begins with the three data section
 * addresses (big-endian, +4/+8/+0xC), which is how the model finds it: 0x1104
 * into data2 for CalmADMFMCFirmware-17 (1.1-1.1.5), 0x824 for -14 (1.0).
 */
#define ADM_TF_V17           0x1104
#define ADM_TF_V14           0x824
#define ADM_CMD_OFFSET       0x24
#define ADM_CMD_NPAGES       0x28   /* uint16, big-endian */
#define ADM_CMD_CE           0x34   /* uint8: chip enable (bank) of an erase */
#define ADM_CMD_BANKS        0x44   /* uint8 per page */
#define ADM_CMD_PAGES        0x244  /* uint32 big-endian per page (firmware-17; see adm_pages_off) */

#define ADM_CMD_READ_SEQ   0x200   /* n pages, same page on every bank */
#define ADM_CMD_READ       0x300   /* one page, or a scattered list */
#define ADM_CMD_WRITE_SEQ  0x400   /* n pages, same page on every bank (READ_SEQ's twin) */
#define ADM_CMD_WRITE      0x500
#define ADM_CMD_ERASE      0x600   /* one block */

typedef struct S5L8900ADMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t code_sec_addr;
    uint32_t data1_sec_addr;
    uint32_t data2_sec_addr;
    uint32_t data3_sec_addr;
    uint32_t result[4];
    uint32_t tf;            /* the transfer block's offset in data2, 0 = not found yet */

    MemoryRegion *downstream;
    AddressSpace downstream_as;

    S5L8900FMCState *fmc;   /* set by the board */
} S5L8900ADMState;

#endif
