#ifndef IPOD_TOUCH_FMSS_H
#define IPOD_TOUCH_FMSS_H

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/irq.h"

bool ipod_touch_fmss_io_failed(void);

#define TYPE_IPOD_TOUCH_FMSS                "ipodtouch.fmss"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchFMSSState, IPOD_TOUCH_FMSS)

#define NAND_BYTES_PER_PAGE 4096
#define NAND_BYTES_PER_SPARE 64
/* Block geometry. The plane stride the driver uses is 0x80 pages, and every
 * populated run in the reference NAND image starts on a 0x80 boundary. */
#define NAND_PAGES_PER_BLOCK 128

#define FMSS__FMCTRL1             0x4
#define FMSS__CS_IRQ              0xC0C
#define FMSS__CS_IRQMASK          0xC10
#define FMSS__CS_BUF_RST_OK       0xC64
#define FMSS_CINFO_TARGET_ADDR    0xD08
#define FMSS_PAGES_IN_ADDR        0xD0C
#define FMSS_CS_BUF_ADDR          0xD10
#define FMSS_NUM_PAGES            0xD18
#define FMSS_PAGE_SPARE_OUT_ADDR  0xD1C
#define FMSS_PAGES_OUT_ADDR       0xD20
#define FMSS_CSGENRC              0xD30

typedef struct IPodTouchFMSSState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint8_t *page_buffer;
    uint8_t *page_spare_buffer;

    uint32_t reg_cs_irq_bit;
    uint32_t reg_cinfo_target_addr;
    uint32_t reg_pages_in_addr;
    uint32_t reg_cs_buf_addr;
    uint32_t reg_num_pages;
    uint32_t reg_page_spare_out_addr;
    uint32_t reg_pages_out_addr;
    uint32_t reg_csgenrc;
    char *nand_path;
    char *nand_overlay;   /* writable COW overlay dir, or NULL to discard writes */

    /*
     * Packed base image (see imgtools/pack_nand.py), mapped once at realize.
     * When this is set the base NAND is served from memory and nand_path is
     * never opened per page -- which is the difference between one memcpy and
     * an fopen/fread/fclose for every 4 KB the guest reads. NULL means the base
     * is still a directory of .page files.
     */
    const uint8_t *packed;
    size_t packed_size;
    const uint32_t *packed_index;   /* num_cs * pages_per_cs, 1-based, 0 = absent */
    const uint8_t *packed_records;
    size_t packed_record_count;   /* bounds the index's slot numbers */
    uint32_t packed_num_cs;
    uint32_t packed_pages_per_cs;
    GHashTable *erased_blocks; /* (cs << 32) | block for blocks erased in the overlay */

    /*
     * Which pages the overlay actually holds, so a read that the overlay does
     * NOT have costs a hash lookup instead of a failed open().
     *
     * The overlay was consulted with fopen() on every single page read, on the
     * vCPU thread. That is invisible on a clean device -- almost every probe
     * misses -- but it is why a device with accumulated writes booted 2.5x
     * slower than a fresh one: the base image is mmap'd and free to read,
     * while every overlay probe was a syscall.
     */
    GHashTable *overlay_pages;
    bool overlay_indexed;
    /* Allocation blocks the volume in the base image covers; 0 until read out
     * of the image's GPT on first use. See fmss_total_blocks(). */
    uint32_t total_blocks;
    bool total_blocks_from_gpt;

    /*
     * Pages programmed since power-on, keyed by the *physical* (cs, page) the
     * guest addressed, holding page data followed by the guest's own spare.
     * The overlay on disk is written at the page's logical home instead (see
     * fmss_generated_layout), which is right for the persisted image but leaves
     * the physical page the FTL just programmed reading back as whatever the
     * base image had there. Flash does not work that way: a program makes that
     * page read back. This is that memory, and it is deliberately not persisted
     * -- the mapping only holds until the FTL is rebuilt at the next boot.
     *
     * It is keyed by physical page, so it is bounded by the size of the flash
     * (~520 MB if every page were rewritten) rather than by how much the guest
     * writes over the life of the session.
     */
    GHashTable *phys_pages;
} IPodTouchFMSSState;

#endif
