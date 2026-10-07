#ifndef HW_ARM_S5L8900_FMC_H
#define HW_ARM_S5L8900_FMC_H

/*
 * S5L8900 FMC: the iPod touch 1G's raw NAND controller (eight 2048+64-byte
 * page banks behind one FIFO), fed page lists by the ADM. Ported from
 * devos50's ipod_touch_nand.c; the page store is a base directory of
 * bank<N>/<page>.page files plus an optional page-level copy-on-write overlay
 * directory that guest programs and block erases land in (see fmc.c).
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"

#define FMC_NUM_BANKS 8
#define FMC_BYTES_PER_PAGE 2048
#define FMC_BYTES_PER_SPARE 64
#define FMC_META_BYTES 12          /* FTL metadata the ADM reports/takes in data3 */
#define FMC_PAGES_PER_BLOCK 128    /* 256 KiB erase blocks (ID byte 4 = 0xA5) */
#define FMC_PROGRAM_PAD 16         /* pad descriptor after each programmed page */

#define FMC_CHIP_ID 0xA514D3AD

#define FMC_FMCTRL0  0x0
#define FMC_FMCTRL1  0x4
#define FMC_CMD      0x8
#define FMC_FMADDR0  0xC
#define FMC_FMADDR1  0x10
#define FMC_FMANUM   0x2C
#define FMC_FMDNUM   0x30
#define FMC_FMCSTAT  0x48
#define FMC_FMFIFO   0x80
#define FMC_RSCTRL   0x100

#define FMC_CMD_ID  0x90
#define FMC_CMD_READ 0x30
#define FMC_CMD_READSTATUS 0x70

#define FMC_MAX_LIST 512   /* pages one ADM command can queue */

#define TYPE_S5L8900_FMC "s5l8900.fmc"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900FMCState, S5L8900_FMC)

typedef struct S5L8900FMCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t fmctrl0;
    uint32_t fmctrl1;
    uint32_t fmaddr0;
    uint32_t fmaddr1;
    uint32_t fmanum;
    uint32_t fmdnum;
    uint32_t rsctrl;
    uint32_t cmd;
    bool reading_spare;

    uint8_t page_buffer[FMC_BYTES_PER_PAGE];
    uint8_t page_spare_buffer[FMC_BYTES_PER_SPARE];
    int32_t buffered_bank;
    int32_t buffered_page;

    /* Multi-page list handed over by the ADM. */
    bool reading_multiple_pages;
    int32_t cur_bank_reading;
    uint32_t banks_to_read[FMC_MAX_LIST];
    uint32_t pages_to_read[FMC_MAX_LIST];
    bool is_writing;
    /* A program (ADM 0x400/0x500): the list above, one FTL metadata block per page. */
    uint32_t prog_count;
    uint32_t prog_index;
    uint32_t prog_pad;           /* pad bytes still to swallow after a page */
    uint8_t prog_spares[FMC_MAX_LIST][FMC_META_BYTES];

    char *nand_path;      /* "nand" property: base directory */
    char *nand_overlay;   /* "nand-overlay" property: writable directory, or NULL */
    uint32_t banks;       /* "banks": chip enables with a chip (ID reads), default all 8 */
} S5L8900FMCState;

/* Select the active bank (FMCTRL0 bit 1+bank). */
void s5l8900_fmc_set_bank(S5L8900FMCState *s, uint32_t bank);
/* Read (bank, page) from the store: overlay, then base, else erased (all ones). */
void s5l8900_fmc_load_page(S5L8900FMCState *s, uint32_t bank, uint32_t page,
                           uint8_t *data, uint8_t *spare);
/* Every data and spare bit set: what an erased page reads as. */
bool s5l8900_fmc_page_erased(const uint8_t *data, const uint8_t *spare);
/* Erase a 128-page block of one bank in the overlay. */
bool s5l8900_fmc_erase_block(S5L8900FMCState *s, uint32_t bank, uint32_t block);
/* Program the first `count` entries of banks_to_read/pages_to_read/prog_spares
 * from the FIFO stream that follows. */
void s5l8900_fmc_start_program(S5L8900FMCState *s, unsigned count);
/* Load (bank, page) into the page buffers unless already there. */
void s5l8900_fmc_buffer_page(S5L8900FMCState *s, uint32_t page);

#endif
