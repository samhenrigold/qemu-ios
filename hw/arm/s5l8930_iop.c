/*
 * S5L8930 ("A4") IOP: high-level emulation of the second core's host-visible
 * surface. The IOP firmware (IOP_S5L8930X_firmware, "EmbeddedIOP for
 * s5l8930x", iBoot-817.29) never executes here; this device answers what the
 * kernel can observe of it: the AP-side control block at 0x86300000, the
 * doorbell SOFTINT in the IOP's own VIC window at 0xBF300000, and the "qwi"
 * message rings in guest DRAM that AppleS5L8920XARM7M and AppleS5L8920XIOPFMI
 * drive through it.
 *
 * Protocol: docs/research/gap-iop-mailbox-protocol.md §2-§3, checked
 * against the 7B500 kexts (ARM7M __text c04d2000, IOPFMI __text c04e8000) and
 * the firmware blob (fw offsets below are into the 0x1b000-byte image).
 *
 * NAND: with the "nand" property unset every page reads as erased. Set to a
 * directory, it is a writable file-backed store: geometry.json plus one
 * sparse file per chip select, bus<b>-ce<c>.pages, page index
 * block * pages_per_block + page at a stride of page_bytes + spare_bytes,
 * data then spare (the 10 meta bytes lead the spare). Holes (all-zero) read as 0xFF;
 * an all-0xFF page is blank (status 2). Writes go straight to the mapping.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "hw/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "system/runstate.h"
#include "qemu/error-report.h"
#include <sys/mman.h>

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930IOPState, S5L8930_IOP)

#ifdef DEBUG_S5L8930_IOP
#define DPRINTF(fmt, ...) fprintf(stderr, "s5l8930_iop[%8.3f]: " fmt, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/*
 * AP-side control block. _arm7mStart (c04d3420): CTRL=0x10, REQUIRE(CTRL&2)
 * "ARM7M not stopped for some reason", SELF=0x86300000, FW_BASE=phys,
 * FW_SIZE=size, CTRL=1. _arm7mStop (c04d2b84): wait up to 1 s for !(CTRL&4)
 * ("MOP", an outstanding memory op; never set here), CTRL=0, wait CTRL&2
 * "ARM7M failed to stop". Offsets 0x00-0x24 are the IOP's own cache controls
 * and are never touched by the AP.
 */
#define IOP_CTRL            0x100
#define IOP_CTRL_STOPPED    (1u << 1)
#define IOP_CTRL_RUN        0x1
#define IOP_CTRL_STOP       0x0
#define IOP_CTRL_HALT       0x10
#define IOP_FW_BASE         0x110   /* firmware physical base = IOP address 0 */
#define IOP_FW_SIZE         0x114
#define IOP_SELF            0x118   /* kext writes the block's own address */

/* IOP-side VICs: 4 PL192s, stride 0x10000. Only SOFTINT/SOFTINTCLEAR act. */
#define IOP_VIC_COUNT       4
#define IOP_VIC_STRIDE      0x10000
#define IOP_VIC_REGS        0x1000
#define VIC_SOFTINT         0x18
#define VIC_SOFTINTCLEAR    0x1c
#define IOP_IRQ_NMI         (1u << 2)   /* _arm7mSendNMI (c04d23d4) */
#define IOP_IRQ_DOORBELL    (1u << 3)   /* arm7mSendInterrupt (c04d2418) */

/*
 * Firmware image layout. The 'cnfg' block and the bss move with every
 * firmware build (7B500: cnfg 0xf018, bss 0xf160-0x1b000; 8C148/iBoot-931:
 * 0x15018, 0x15160-0x22000), so both are read from the image the kext loaded:
 * the bss bounds from its header words, the block by scanning for its magic.
 */
#define FW_CONFIG_MAGIC     0x636e6667  /* 'cnfg' */
#define FW_HDR_BSS_START    0x318       /* start() zeroes [fw[0x318], fw[0x31c]) */
#define FW_HDR_BSS_END      0x31c
#define FW_MAX_SIZE         0x100000    /* scans stop here (images are 0x1b000-0x22000) */
#define FW_CFG_MSGBUF       0x8
#define FW_CFG_RING(h)      (0xc + 8 * (h))
#define FW_CFG_COUNT(h)     (0x10 + 8 * (h))
#define IOP_MAX_ENDPOINTS   8           /* ARM7M_MAX_ENDPOINTS */
#define IOP_MAX_RING        1024

/* Endpoint handles (firmware task table fw 0xf000 -> descriptors). */
#define RING_CONTROL        0           /* 1 = message ring, IOP -> AP only */
#define RING_SDIO           3
#define RING_FMI0           5
#define RING_FMI1           6

/*
 * Ring = N x 16-byte entries, word 0 = item | ownership bit (bit 1 reserved).
 * The IOP is direction 0 on every ring it serves: a slot is pending when
 * bit 0 is clear, and the answer is the same item with bit 0 set, in place
 * (fw receive 0xd40 / send 0xe00; the kext reads context[rx] for the slot).
 */
#define RING_ENTRY_SIZE     16
#define RING_OWNER_AP       1
#define RING_ITEM(w0)       ((w0) & ~3u)

/* Control messages: 32 bytes, {u32 opcode, u32 status, ...}; fw 0xb44-0xc14. */
#define CTRL_MSG_SIZE       32
#define CTRL_OP_SLEP        0x736c6570  /* 'slep': answer, then halt */
#define CTRL_OP_NOP         0x6e6f7020  /* 'nop ' */
#define CTRL_OP_RSUM        0x7273756d  /* 'rsum' */
#define CTRL_OP_SPND        0x73706e64  /* 'spnd' */
#define CTRL_OP_TTIN        0x7474696e  /* 'ttin': console character */

/*
 * FMI/SDIO commands: 512 bytes, u32 opcode at +0, u32 status at +8
 * (fw FMI task 0x1254, SDIO task 0x1524; kext _fmiInitCommand c04e9084).
 */
#define CMD_SIZE            0x200
#define CMD_OPCODE          0x00
#define CMD_STATUS          0x08

#define FMI_OP_SET_CONFIG       1
#define FMI_OP_RESET_EVERYTHING 2
#define FMI_OP_ERASE_SINGLE     3
#define FMI_OP_READ_SINGLE      4
#define FMI_OP_READ_RAW         5
#define FMI_OP_WRITE_SINGLE     6
#define FMI_OP_WRITE_RAW        7
#define FMI_OP_READ_MULTIPLE    8
#define FMI_OP_WRITE_MULTIPLE   9
#define FMI_OP_WRITE_BOOTPAGE   10
#define FMI_OP_READ_BOOTPAGE    11
#define FMI_OP_ERASE_MULTIPLE   12
#define FMI_OP_READ_CHIP_IDS    20          /* v2 only: IDs moved out of op 2 */

/*
 * Two command ABIs. v1 (EmbeddedIOP iBoot-817, iOS 3.2): arguments from
 * +0x10. v2 (iBoot-931, iOS 4.2; the firmware that has h2fmi_iop_read_chip_ids
 * and PPN support): every argument moves up one word (+0x14), CE numbers are
 * u16, op 2 only resets and op 20 returns the IDs, and the multi-page and
 * erase outputs are laid out anew; CE arrays are u16 too (fw 8C148 0x2230 / 0x264c / 0x26c4).
 * iop_config picks the ABI from the loaded image, with the 'cnfg' block.
 */
#define FMI_V2_MARKER           "h2fmi_iop_read_chip_ids"

/* Status codes as _fmiTranslateResult (c04e8ef0) understands them. */
#define FMI_STATUS_OK           1
#define FMI_STATUS_BLANK        2           /* kIOReturnUnformattedMedia */
#define FMI_STATUS_UECC         0x80000001  /* "device error" */
#define FMI_STATUS_PARAM        0x80000004  /* fw: unrecognised opcode too */
#define FMI_STATUS_SOME_BLANK   0x80000023  /* read_multi: blank + good mix */
#define FMI_STATUS_SOME_UECC    0x80000024
#define FMI_STATUS_ALL_UECC     0x80000025

#define FMI_ID_BYTES            5           /* per CE, 16 CEs per bus */
#define FMI_ID_CES              16
#define FMI_META_BYTES          10          /* bytes_per_meta, fw set_config */
#define FMI_BOOTPAGE_BYTES      0x600       /* kext c04e92b6 */
#define FMI_MAX_PAGE            0x4000
#define FMI_MAX_MULTI           0x200       /* 0x800-byte CE/page arrays */

#define SDIO_OP_PING            1
#define SDIO_STATUS_OK          0
#define SDIO_STATUS_UNKNOWN     2           /* fw 0x164c "unrecognised sdio opcode" */

#define NAND_BUSES              2
#define NAND_CES                8

struct S5L8930IOPState {
    SysBusDevice parent_obj;
    MemoryRegion ctrl_mr;
    MemoryRegion vic_mr;

    DeviceState *sdio;      /* ring 3 goes here; NULL = no card */
    uint32_t nand_id;       /* the 4 ID bytes the kext compares, LE packed */
    uint8_t nand_ce_mask;   /* CE slots populated on each bus */
    char *nand_dir;         /* page store directory; NULL = blank chip */
    char *overlay_dir;      /* copy-on-write overlay; base is then read-only */
    uint8_t *chip[NAND_BUSES][NAND_CES];    /* mmap of bus<b>-ce<c>.pages */
    int chip_fd[NAND_BUSES][NAND_CES];
    uint8_t *ovl[NAND_BUSES][NAND_CES];     /* overlay pages, same layout */
    int ovl_fd[NAND_BUSES][NAND_CES];
    uint8_t *dirty[NAND_BUSES][NAND_CES];   /* 1 bit/page: overlay is authoritative */
    uint32_t page_stride;       /* store geometry: page + spare bytes */
    uint32_t store_page_bytes;
    uint32_t store_ppb;
    uint32_t pages_per_ce;

    QEMUTimer *irq_timer;
    bool running;
    uint32_t fw_base;
    uint32_t fw_size;
    uint32_t self_addr;
    uint32_t fw_config;     /* 'cnfg' address; 0 = not found yet (not migrated: rescanned) */
    uint32_t fmi_arg;       /* FMI argument shift: 0 = v1, 4 = v2; set with fw_config */
    uint32_t vic_softint[IOP_VIC_COUNT];
    uint32_t vic_regs[IOP_VIC_COUNT][IOP_VIC_REGS / 4];
    uint32_t ring_rx[IOP_MAX_ENDPOINTS];
    /* Geometry per bus, from FMI set_config; defaults are the K48 part. */
    uint32_t bytes_per_page[NAND_BUSES];
    uint32_t bytes_per_spare[NAND_BUSES];
    uint32_t pages_per_block[NAND_BUSES];
};

static inline uint32_t iop_ldl(hwaddr addr)
{
    return ldl_le_phys(&address_space_memory, addr);
}

static inline void iop_stl(hwaddr addr, uint32_t val)
{
    stl_le_phys(&address_space_memory, addr, val);
}

static inline void iop_read(hwaddr addr, void *buf, hwaddr len)
{
    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
}

static inline void iop_write(hwaddr addr, const void *buf, hwaddr len)
{
    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
}

/*
 * IOP -> AP doorbell. The firmware (fw 0x6c14) sets SOFTINT bit 3 on the
 * AP's VIC0; the AP VIC driver clears it with SOFTINTCLEAR on dispatch
 * (AppleARMPL192VIC c04d0e6, ipid-mask 0xf). A level on a VIC input line
 * would never be cleared, so do exactly what the firmware does.
 */
static void iop_irq_expire(void *opaque)
{
    iop_stl(S5L8930_VIC_BASE(S5L8930_IRQ_IOP / 32) + VIC_SOFTINT,
            1u << (S5L8930_IRQ_IOP % 32));
}

/*
 * Deferred by about a real IOP's round trip, so the answer never interrupts
 * the AP in the middle of the store that rang the doorbell.
 */
static void iop_raise_ap_irq(S5L8930IOPState *s)
{
    timer_mod(s->irq_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * SCALE_US);
}

/* ---- NAND ------------------------------------------------------------- */

static bool nand_addr_bad(S5L8930IOPState *s, int bus, uint32_t ce, uint32_t page)
{
    bool bad = ce >= NAND_CES || !(s->nand_ce_mask & (1u << ce)) ||
               (s->nand_dir && page >= s->pages_per_ce);

    if (bad) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bus %d ce %u page 0x%x off the part\n",
                      __func__, bus, ce, page);
    }
    return bad;
}

static inline bool nand_dirty(S5L8930IOPState *s, int bus, uint32_t ce, uint32_t page)
{
    return s->overlay_dir && (s->dirty[bus][ce][page >> 3] & (1u << (page & 7)));
}

static inline void nand_set_dirty(S5L8930IOPState *s, int bus, uint32_t ce,
                                  uint32_t page)
{
    s->dirty[bus][ce][page >> 3] |= 1u << (page & 7);
}

/*
 * Stamp the overlay file with the time of this write. Stores through the
 * MAP_SHARED mapping reach the file but do not move its mtime, and the app
 * pairs a RAM snapshot with the overlay by exactly that: an overlay newer
 * than the snapshot means flash advanced after the save and the snapshot must
 * not be restored over it (DeviceStateStorage.overlayIsNewer).
 */
static inline void nand_touch(S5L8930IOPState *s, int bus, uint32_t ce)
{
    futimens(s->ovl_fd[bus][ce], NULL);
}

/*
 * Where a page currently lives: the overlay once it has been programmed or
 * erased there, else the base store. NULL for the blank chip.
 */
static uint8_t *nand_page(S5L8930IOPState *s, int bus, uint32_t ce,
                          uint32_t page, bool for_write)
{
    if (!s->nand_dir) {
        return NULL;
    }
    if (s->overlay_dir && (for_write || nand_dirty(s, bus, ce, page))) {
        return s->ovl[bus][ce] + (size_t)page * s->page_stride;
    }
    return s->chip[bus][ce] + (size_t)page * s->page_stride;
}

/* data gets page + spare bytes, meta the leading FMI_META_BYTES of the spare. */
static uint32_t nand_read_page(S5L8930IOPState *s, int bus, uint32_t ce,
                               uint32_t page, uint8_t *data, uint8_t *meta)
{
    uint32_t len = s->bytes_per_page[bus] + s->bytes_per_spare[bus];
    uint8_t *p;
    uint32_t i;

    memset(data, 0xff, MAX(len, s->page_stride));
    memset(meta, 0xff, FMI_META_BYTES);
    if (nand_addr_bad(s, bus, ce, page)) {
        return FMI_STATUS_PARAM;
    }
    p = nand_page(s, bus, ce, page, false);
    if (!p) {
        return FMI_STATUS_BLANK;
    }
    len = s->page_stride;
    /*
     * An unwritten page is a hole in the sparse file and reads as zeros, but
     * erased NAND reads as 0xFF. Treat an all-zero stride as blank too: a
     * programmed page always carries non-zero FTL metadata in its spare.
     */
    for (i = 0; i < len && p[i] == 0; i++) {
    }
    if (i == len) {
        return FMI_STATUS_BLANK;
    }
    memcpy(data, p, len);
    memcpy(meta, p + s->store_page_bytes, FMI_META_BYTES);
    for (i = 0; i < len; i++) {
        if (data[i] != 0xff) {
            return FMI_STATUS_OK;
        }
    }
    return FMI_STATUS_BLANK;
}

/* The store as raw flash, for the H2FMI model (iBoot's direct NAND path). */
bool s5l8930_iop_nand_read(DeviceState *dev, int bus, uint32_t ce,
                           uint32_t page, uint8_t *buf, uint32_t *stride)
{
    S5L8930IOPState *s = S5L8930_IOP(dev);
    uint8_t meta[FMI_META_BYTES];

    *stride = s->nand_dir ? s->page_stride : 0;
    return bus < NAND_BUSES &&
           nand_read_page(s, bus, ce, page, buf, meta) == FMI_STATUS_OK;
}

void s5l8930_iop_nand_info(DeviceState *dev, uint32_t *id, uint8_t *ce_mask,
                           uint32_t *page_bytes)
{
    S5L8930IOPState *s = S5L8930_IOP(dev);

    *id = s->nand_id;
    *ce_mask = s->nand_ce_mask;
    *page_bytes = s->nand_dir ? s->store_page_bytes : 0;
}

/*
 * len bytes of data land at the page start (a raw write carries page + spare,
 * a boot page 0x600 bytes); meta, if given, leads the spare.
 */
static uint32_t nand_program_page(S5L8930IOPState *s, int bus, uint32_t ce,
                                  uint32_t page, const uint8_t *data,
                                  uint32_t len, const uint8_t *meta)
{
    uint8_t *p;

    if (nand_addr_bad(s, bus, ce, page)) {
        return FMI_STATUS_PARAM;
    }
    p = nand_page(s, bus, ce, page, true);
    if (!p) {
        DPRINTF("program bus %d ce %u page 0x%x (blank chip, dropped)\n",
                bus, ce, page);
        return FMI_STATUS_OK;
    }
    {
        uint32_t n = MIN(len, s->page_stride), i;

        for (i = 0; i < n && data[i] == 0; i++) {
        }
        if (i == n && (!meta || !memcmp(meta, "\0\0\0\0\0\0\0\0\0\0", FMI_META_BYTES))) {
            /* Would read back as a hole, i.e. blank. The FTL never does this. */
            qemu_log_mask(LOG_GUEST_ERROR, "%s: all-zero program of bus %d ce %u "
                          "page 0x%x reads back blank\n", __func__, bus, ce, page);
        }
        memcpy(p, data, n);
    }
    if (meta) {
        memcpy(p + s->store_page_bytes, meta, FMI_META_BYTES);
    }
    if (s->overlay_dir) {
        nand_set_dirty(s, bus, ce, page);
        nand_touch(s, bus, ce);
    }
    return FMI_STATUS_OK;
}

static uint32_t nand_erase_block(S5L8930IOPState *s, int bus, uint32_t ce,
                                 uint32_t block)
{
    uint32_t first = block * s->store_ppb;
    uint8_t *p;
    int fd;

    if (nand_addr_bad(s, bus, ce, first)) {
        return FMI_STATUS_PARAM;
    }
    p = nand_page(s, bus, ce, first, true);
    fd = s->overlay_dir ? s->ovl_fd[bus][ce] : s->chip_fd[bus][ce];
    if (s->overlay_dir) {
        uint32_t i;

        for (i = 0; i < s->store_ppb; i++) {
            nand_set_dirty(s, bus, ce, first + i);
        }
        nand_touch(s, bus, ce);
    }
    if (!p) {
        DPRINTF("erase bus %d ce %u block 0x%x (blank chip, dropped)\n",
                bus, ce, block);
        return FMI_STATUS_OK;
    }
    /* Erased = a hole (reads as zeros = blank); no disk space consumed. */
    {
        off_t off = (off_t)first * s->page_stride;
        off_t blen = (off_t)s->store_ppb * s->page_stride;
#ifdef F_PUNCHHOLE
        struct fpunchhole fp = { .fp_offset = off, .fp_length = blen };

        if (fcntl(fd, F_PUNCHHOLE, &fp) == 0) {
            return FMI_STATUS_OK;
        }
#endif
        (void)off;
        memset(p, 0, blen);
    }
    return FMI_STATUS_OK;
}

/* ---- FMI ------------------------------------------------------------- */

#define CMD_GET(cmd, off)       ldl_le_p((cmd) + (off))
/* An FMI argument, v1 offset in, as the loaded firmware's ABI has it. */
#define ARG(off)                ldl_le_p(cmd + (off) + s->fmi_arg)
#define ARG_CE(off)             (ARG(off) & (s->fmi_arg ? 0xffff : 0xffffffff))
#define CMD_SET(cmd, off, val)  stl_le_p((cmd) + (off), (val))

/*
 * Multi-page commands hand over CDMA segment lists: arrays of
 * {u32 phys, u32 len} the kext builds per NAND page
 * (VSGenerateDMAListByNandPage c04eb740); the fw streams pages through them
 * in order. Meta segments are FMI_META_BYTES each.
 */
typedef struct SegCursor {
    hwaddr list;
    uint32_t nseg;
    uint32_t idx;
    uint32_t off;
} SegCursor;

static void seg_cursor_init(SegCursor *c, hwaddr list, uint32_t bytes)
{
    c->list = list;
    c->nseg = bytes / 8;
    c->idx = 0;
    c->off = 0;
}

static void seg_copy(SegCursor *c, uint8_t *buf, uint32_t len, bool to_guest)
{
    while (len) {
        uint32_t phys, slen, chunk;

        if (c->idx >= c->nseg) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: DMA segment list at 0x%" HWADDR_PRIx
                          " too short by %u bytes\n", __func__, c->list, len);
            return;
        }
        phys = iop_ldl(c->list + 8 * c->idx);
        slen = iop_ldl(c->list + 8 * c->idx + 4);
        chunk = MIN(len, slen - c->off);
        if (to_guest) {
            iop_write(phys + c->off, buf, chunk);
        } else {
            iop_read(phys + c->off, buf, chunk);
        }
        buf += chunk;
        len -= chunk;
        c->off += chunk;
        if (c->off >= slen) {
            c->idx++;
            c->off = 0;
        }
    }
}

/* Fold per-page results the way h2fmi_read_multi does (fw 0x4594-0x45f0). */
static uint32_t fmi_multi_status(uint32_t n, uint32_t blank, uint32_t uecc)
{
    if (blank) {
        return n <= blank ? FMI_STATUS_BLANK : FMI_STATUS_SOME_BLANK;
    }
    if (uecc) {
        return n <= uecc ? FMI_STATUS_ALL_UECC : FMI_STATUS_SOME_UECC;
    }
    return FMI_STATUS_OK;
}

/* h2fmi_iop_set_config (fw 0x2628); the kext fills it at c04e90a0. */
static uint32_t fmi_set_config(S5L8930IOPState *s, int bus, uint8_t *cmd)
{
    uint32_t bpp = ARG(0x24);
    uint32_t spare = ARG(0x28);

    if (ARG(0x10) != bus || bpp == 0 || bpp > FMI_MAX_PAGE ||
        spare > FMI_MAX_PAGE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad config bus %u page %u spare %u\n",
                      __func__, ARG(0x10), bpp, spare);
        return FMI_STATUS_PARAM;
    }
    /* The kext's first set_config is a generic 2048+64 x 1 pre-probe one. */
    if (s->nand_dir && (bpp + spare != s->page_stride ||
                        ARG(0x1c) != s->store_ppb)) {
        DPRINTF("set_config bus %d: %u+%u x %u differs from the store\n",
                bus, bpp, spare, ARG(0x1c));
    }
    /*
     * v2 passes the meta layout (+0x50 valid bytes, the per-page DMA; +0x54
     * total, YaFTL's struct) where v1's firmware hard-coded 10 bytes. Only
     * the DMA size matters here; the pre-probe config sends 0.
     */
    if (s->fmi_arg && CMD_GET(cmd, 0x50) && CMD_GET(cmd, 0x50) != FMI_META_BYTES) {
        qemu_log_mask(LOG_UNIMP, "%s: bus %d wants %u meta bytes per page, model moves %d\n",
                      __func__, bus, CMD_GET(cmd, 0x50), FMI_META_BYTES);
    }
    s->bytes_per_page[bus] = bpp;
    s->bytes_per_spare[bus] = spare;
    s->pages_per_block[bus] = ARG(0x1c);
    DPRINTF("set_config bus %d: %u CEs (mask 0x%x), %u pages/block, %u+%u bytes\n",
            bus, ARG(0x14), ARG(0x18),
            s->pages_per_block[bus], bpp, spare);
    return FMI_STATUS_OK;
}

/*
 * h2fmi_iop_reset_everything (fw 0x2520): reads the ID of all 16 CE slots
 * into an 80-byte buffer at +0x10. The kext (c04e96f6) packs bytes 0-3 LE,
 * takes bus 0 CE 0 as the reference and skips slots whose ID is 0.
 */
static uint32_t fmi_reset_everything(S5L8930IOPState *s, int bus, uint8_t *cmd)
{
    uint8_t ids[FMI_ID_CES * FMI_ID_BYTES] = { 0 };
    int ce;

    for (ce = 0; ce < 8; ce++) {
        if (s->nand_ce_mask & (1u << ce)) {
            stl_le_p(&ids[ce * FMI_ID_BYTES], s->nand_id);
            ids[ce * FMI_ID_BYTES + 4] = 0x54;
        }
    }
    iop_write(ARG(0x10), ids, sizeof(ids));
    return FMI_STATUS_OK;
}

/* Single-page reads: +0x10 ce, +0x14 page, +0x18 data, +0x1c meta (op 4). */
static uint32_t fmi_read_single(S5L8930IOPState *s, int bus, uint8_t *cmd,
                                uint8_t *page, uint8_t *meta)
{
    uint32_t st = nand_read_page(s, bus, ARG_CE(0x10), ARG(0x14), page, meta);

    iop_write(ARG(0x18), page, s->bytes_per_page[bus]);
    iop_write(ARG(0x1c), meta, FMI_META_BYTES);
    return st;
}

/* h2fmi_read_raw_page (fw 0x1be4): data then spare, no ECC, at +0x18. */
static uint32_t fmi_read_raw(S5L8930IOPState *s, int bus, uint8_t *cmd,
                             uint8_t *page, uint8_t *meta)
{
    nand_read_page(s, bus, ARG_CE(0x10), ARG(0x14), page, meta);
    iop_write(ARG(0x18), page,
              s->bytes_per_page[bus] + s->bytes_per_spare[bus]);
    return FMI_STATUS_OK;
}

/* h2fmi_iop_read_bootpage (fw 0x1ab8): +0x18 gets the 0x600-byte boot page. */
static uint32_t fmi_read_bootpage(S5L8930IOPState *s, int bus, uint8_t *cmd,
                                  uint8_t *page, uint8_t *meta)
{
    uint32_t st = nand_read_page(s, bus, ARG_CE(0x10), ARG(0x14), page, meta);

    iop_write(ARG(0x18), page,
              MIN(FMI_BOOTPAGE_BYTES, s->bytes_per_page[bus]));
    return st;
}

/*
 * h2fmi_iop_read_multiple / write_multiple (fw 0x1f94 / 0x1c30, kext
 * c04eb510; v1 offsets, v2 adds 4): +0x10 count, +0x14 CE array, +0x18 page array, +0x1c/+0x20
 * data segment list and byte length, +0x24/+0x28 meta segment list and
 * length, +0x30 AES (ignored). Outputs: +0x5c pages completed, +0x60 final
 * status, +0x70/+0x74 failing CE/index (-1 = none).
 */
static uint32_t fmi_multi(S5L8930IOPState *s, int bus, uint8_t *cmd, bool write,
                          uint8_t *page, uint8_t *meta)
{
    uint32_t n = ARG(0x10);
    hwaddr ces = ARG(0x14), pages = ARG(0x18);
    uint32_t blank = 0, uecc = 0, st;
    SegCursor data, metas;
    uint32_t i;

    if (n == 0 || n > FMI_MAX_MULTI || !ces || !pages ||
        !ARG(0x1c) || !ARG(0x24)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad multi-page command (%u pages)\n",
                      __func__, n);
        return FMI_STATUS_PARAM;
    }
    seg_cursor_init(&data, ARG(0x1c), ARG(0x20));
    seg_cursor_init(&metas, ARG(0x24), ARG(0x28));

    for (i = 0; i < n; i++) {
        /* v2 CE arrays are u16 (fw 8C148 0x4444: ldrh [ce_array, i << 1]) */
        uint32_t ce = s->fmi_arg ? lduw_le_phys(&address_space_memory, ces + 2 * i)
                                 : iop_ldl(ces + 4 * i);
        uint32_t pg = iop_ldl(pages + 4 * i);

        if (write) {
            seg_copy(&data, page, s->bytes_per_page[bus], false);
            seg_copy(&metas, meta, FMI_META_BYTES, false);
            nand_program_page(s, bus, ce, pg, page, s->bytes_per_page[bus], meta);
        } else {
            st = nand_read_page(s, bus, ce, pg, page, meta);
            blank += st == FMI_STATUS_BLANK;
            uecc += st == FMI_STATUS_UECC;
            seg_copy(&data, page, s->bytes_per_page[bus], true);
            seg_copy(&metas, meta, FMI_META_BYTES, true);
        }
    }
    st = write ? FMI_STATUS_OK : fmi_multi_status(n, blank, uecc);
    DPRINTF("%s %u pages bus %d first ce %u page 0x%x: blank %u -> 0x%x\n",
            write ? "write" : "read", n, bus, iop_ldl(ces), iop_ldl(pages),
            blank, st);
    if (s->fmi_arg) {           /* v2: +0x60 count, +0x64 status, +0x6c failing */
        CMD_SET(cmd, 0x60, n);
        CMD_SET(cmd, 0x64, st);
        CMD_SET(cmd, 0x6c, 0xffffffff);
    } else {
        CMD_SET(cmd, 0x5c, n);
        CMD_SET(cmd, 0x60, st);
        CMD_SET(cmd, 0x70, 0xffffffff);
        CMD_SET(cmd, 0x74, 0xffffffff);
    }
    return st;
}

/* Erase single (fw 0x240c): +0x10 ce, +0x14 block; out +0x18 count, +0x20 status word. */
static uint32_t fmi_erase_single(S5L8930IOPState *s, int bus, uint8_t *cmd)
{
    uint32_t st = nand_erase_block(s, bus, ARG_CE(0x10), ARG(0x14));

    CMD_SET(cmd, 0x18 + s->fmi_arg, 1);
    CMD_SET(cmd, 0x20 + s->fmi_arg, st == FMI_STATUS_OK ? 0 : FMI_STATUS_UECC);
    return st;
}

/*
 * Erase multiple (fw 0x2484, kext c04e9a60): +0x10 count, +0x14 16 CEs,
 * +0x54 16 blocks, +0xa0/+0xa4 per-operation status ring (0 = ok); out
 * +0x94 count completed.
 */
static uint32_t fmi_erase_multiple(S5L8930IOPState *s, int bus, uint8_t *cmd)
{
    uint32_t n = CMD_GET(cmd, 0x10), ring = CMD_GET(cmd, 0xa0);
    uint32_t ring_bytes = CMD_GET(cmd, 0xa4), st = FMI_STATUS_OK;
    uint32_t i;

    if (s->fmi_arg) {
        /* v2 (fw 0x26c4): +0x14 count, +0x18 16 u16 CEs, +0x38 16 u32 blocks; out +0x78 done, +0x84 failing */
        n = ARG(0x10);
        if (n > 16) {
            return FMI_STATUS_PARAM;
        }
        for (i = 0; i < n; i++) {
            if (nand_erase_block(s, bus, lduw_le_p(cmd + 0x18 + 2 * i),
                                 CMD_GET(cmd, 0x38 + 4 * i)) != FMI_STATUS_OK) {
                st = FMI_STATUS_UECC;
            }
        }
        CMD_SET(cmd, 0x78, n);
        CMD_SET(cmd, 0x84, 0xffffffff);
        return st;
    }

    if (n > 16 || !ring || ring_bytes < 4 || ring_bytes > CMD_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad erase-multiple (%u blocks)\n",
                      __func__, n);
        return FMI_STATUS_PARAM;
    }
    for (i = 0; i < n; i++) {
        uint32_t r = nand_erase_block(s, bus, CMD_GET(cmd, 0x14 + 4 * i),
                                      CMD_GET(cmd, 0x54 + 4 * i));
        iop_stl(ring + 4 * (i % (ring_bytes / 4)), r == FMI_STATUS_OK ? 0 : r);
        if (r != FMI_STATUS_OK) {
            st = FMI_STATUS_UECC;
        }
    }
    CMD_SET(cmd, 0x94, n);
    return st;
}

static void iop_fmi_command(S5L8930IOPState *s, int bus, hwaddr item)
{
    g_autofree uint8_t *page = g_malloc(FMI_MAX_PAGE * 2);
    uint8_t meta[FMI_META_BYTES];
    uint8_t cmd[CMD_SIZE];
    uint32_t op, st, len;

    iop_read(item, cmd, sizeof(cmd));
    op = CMD_GET(cmd, CMD_OPCODE);
    DPRINTF("fmi%d op %u at 0x%" HWADDR_PRIx "\n", bus, op, item);

    switch (op) {
    case FMI_OP_SET_CONFIG:
        st = fmi_set_config(s, bus, cmd);
        break;
    case FMI_OP_RESET_EVERYTHING:   /* v2: reset only; the IDs are op 20's */
        st = s->fmi_arg ? FMI_STATUS_OK : fmi_reset_everything(s, bus, cmd);
        break;
    case FMI_OP_READ_CHIP_IDS:
        st = s->fmi_arg ? fmi_reset_everything(s, bus, cmd) : FMI_STATUS_PARAM;
        break;
    case FMI_OP_ERASE_SINGLE:
        st = fmi_erase_single(s, bus, cmd);
        break;
    case FMI_OP_READ_SINGLE:
        st = fmi_read_single(s, bus, cmd, page, meta);
        break;
    case FMI_OP_READ_RAW:
        st = fmi_read_raw(s, bus, cmd, page, meta);
        break;
    case FMI_OP_WRITE_SINGLE:
        iop_read(ARG(0x18), page, s->bytes_per_page[bus]);
        iop_read(ARG(0x1c), meta, FMI_META_BYTES);
        st = nand_program_page(s, bus, ARG_CE(0x10), ARG(0x14),
                               page, s->bytes_per_page[bus], meta);
        break;
    case FMI_OP_WRITE_RAW:
        len = s->bytes_per_page[bus] + s->bytes_per_spare[bus];
        iop_read(ARG(0x18), page, len);
        st = nand_program_page(s, bus, ARG_CE(0x10), ARG(0x14), page, len, NULL);
        break;
    case FMI_OP_WRITE_BOOTPAGE:
        iop_read(ARG(0x18), page, FMI_BOOTPAGE_BYTES);
        st = nand_program_page(s, bus, ARG_CE(0x10), ARG(0x14),
                               page, FMI_BOOTPAGE_BYTES, NULL);
        break;
    case FMI_OP_READ_MULTIPLE:
        st = fmi_multi(s, bus, cmd, false, page, meta);
        break;
    case FMI_OP_WRITE_MULTIPLE:
        st = fmi_multi(s, bus, cmd, true, page, meta);
        break;
    case FMI_OP_READ_BOOTPAGE:
        st = fmi_read_bootpage(s, bus, cmd, page, meta);
        break;
    case FMI_OP_ERASE_MULTIPLE:
        st = fmi_erase_multiple(s, bus, cmd);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: fmi%d opcode %u\n", __func__, bus, op);
        st = FMI_STATUS_PARAM;
        break;
    }
    CMD_SET(cmd, CMD_STATUS, st);
    iop_write(item, cmd, sizeof(cmd));
}

/* ---- SDIO, control ------------------------------------------------------ */

static void iop_sdio_command(S5L8930IOPState *s, hwaddr item)
{
    uint32_t op = iop_ldl(item + CMD_OPCODE);

    if (op == SDIO_OP_PING) {
        iop_stl(item + CMD_STATUS, SDIO_STATUS_OK);
        return;
    }
    if (s->sdio) {
        uint8_t cmd[S5L8930_SDIO_CMD_SIZE];

        iop_read(item, cmd, sizeof(cmd));
        s5l8930_sdio_iop_command(s->sdio, cmd);
        iop_write(item, cmd, sizeof(cmd));
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: sdio opcode %u\n", __func__, op);
    iop_stl(item + CMD_STATUS, SDIO_STATUS_UNKNOWN);
}

static void iop_control_message(S5L8930IOPState *s, hwaddr item)
{
    uint32_t op = iop_ldl(item);
    uint32_t st = 0;

    DPRINTF("control 0x%08x\n", op);
    switch (op) {
    case CTRL_OP_NOP:
    case CTRL_OP_RSUM:
    case CTRL_OP_SPND:
    case CTRL_OP_TTIN:
        break;
    case CTRL_OP_SLEP:
        /* The firmware answers first, runs the sleep hooks, then halts. */
        s->running = false;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: unrecognised host opcode 0x%08x\n",
                      __func__, op);
        st = 1;
        break;
    }
    iop_stl(item + 4, st);
}

/* ---- rings ------------------------------------------------------------- */

/* Whether the loaded firmware speaks FMI v2 (see FMI_V2_MARKER). */
static bool iop_fmi_v2(S5L8930IOPState *s)
{
    uint32_t len = MIN(s->fw_size, FW_MAX_SIZE);
    g_autofree uint8_t *img = g_malloc(len);

    iop_read(s->fw_base, img, len);
    return memmem(img, len, FMI_V2_MARKER, strlen(FMI_V2_MARKER)) != NULL;
}

/* The firmware's 'cnfg' block (ring table, message buffer), found once per load. */
static hwaddr iop_config(S5L8930IOPState *s)
{
    uint32_t off;

    if (!s->fw_config) {
        s->fmi_arg = iop_fmi_v2(s) ? 4 : 0;
        for (off = 0; off + 4 <= MIN(s->fw_size, FW_MAX_SIZE); off += 4) {
            if (iop_ldl(s->fw_base + off) == FW_CONFIG_MAGIC) {
                s->fw_config = s->fw_base + off;
                break;
            }
        }
    }
    return s->fw_config;
}

static bool iop_walk_ring(S5L8930IOPState *s, int h, hwaddr ring, uint32_t n)
{
    bool any = false;
    uint32_t i;

    for (i = 0; i < n; i++) {
        hwaddr slot = ring + s->ring_rx[h] * RING_ENTRY_SIZE;
        uint32_t w0 = iop_ldl(slot);

        if ((w0 & 1) == RING_OWNER_AP || RING_ITEM(w0) == 0) {
            break;
        }
        switch (h) {
        case RING_CONTROL:
            iop_control_message(s, RING_ITEM(w0));
            break;
        case RING_SDIO:
            iop_sdio_command(s, RING_ITEM(w0));
            break;
        case RING_FMI0:
        case RING_FMI1:
            iop_fmi_command(s, h - RING_FMI0, RING_ITEM(w0));
            break;
        }
        iop_stl(slot, RING_ITEM(w0) | RING_OWNER_AP);
        s->ring_rx[h] = (s->ring_rx[h] + 1) % n;
        any = true;
    }
    return any;
}

/*
 * AP -> IOP doorbell: every server task drains its ring. The ring table is
 * re-read from the config block each time because the kext registers the
 * FMI/SDIO endpoints after the control ring, not necessarily before run.
 */
static void iop_doorbell(S5L8930IOPState *s)
{
    static const int served[] = { RING_CONTROL, RING_SDIO, RING_FMI0, RING_FMI1 };
    hwaddr cfg = iop_config(s);
    bool any = false;
    int i;

    if (!s->running) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: doorbell while stopped\n", __func__);
        return;
    }
    if (!cfg) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no 'cnfg' block in the firmware\n", __func__);
        return;
    }
    for (i = 0; i < ARRAY_SIZE(served); i++) {
        int h = served[i];
        hwaddr ring = iop_ldl(cfg + FW_CFG_RING(h));
        uint32_t n = iop_ldl(cfg + FW_CFG_COUNT(h));

        if (!ring || n == 0) {
            continue;
        }
        if (n > IOP_MAX_RING) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: ring %d has %u entries\n",
                          __func__, h, n);
            continue;
        }
        any |= iop_walk_ring(s, h, ring, n);
    }
    if (any) {
        iop_raise_ap_irq(s);
    }
}

/* CTRL = 1: what the firmware's start() does that the AP can see. */
static void iop_run(S5L8930IOPState *s)
{
    uint32_t bss = iop_ldl(s->fw_base + FW_HDR_BSS_START);
    uint32_t end = iop_ldl(s->fw_base + FW_HDR_BSS_END);

    s->fw_config = 0;
    if (!iop_config(s) || bss >= end || end > s->fw_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: no 'cnfg' block / bss [0x%x, 0x%x) in firmware at 0x%08x+0x%x\n",
                      __func__, bss, end, s->fw_base, s->fw_size);
    } else {
        /* bss (PanicString, PanicFunction, PanicLog...) reads as clean. */
        g_autofree uint8_t *zero = g_malloc0(end - bss);
        iop_write(s->fw_base + bss, zero, end - bss);
    }
    memset(s->ring_rx, 0, sizeof(s->ring_rx));
    s->running = true;
    DPRINTF("run: firmware at 0x%08x size 0x%x, message buffer 0x%08x\n",
            s->fw_base, s->fw_size, iop_ldl(s->fw_config + FW_CFG_MSGBUF));
}

/* ---- MMIO -------------------------------------------------------------- */

static uint64_t iop_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930IOPState *s = opaque;

    switch (offset) {
    case IOP_CTRL:
        return s->running ? 0 : IOP_CTRL_STOPPED;
    case IOP_FW_BASE:
        return s->fw_base;
    case IOP_FW_SIZE:
        return s->fw_size;
    case IOP_SELF:
        return s->self_addr;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled read 0x%04" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void iop_ctrl_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    S5L8930IOPState *s = opaque;

    switch (offset) {
    case IOP_CTRL:
        switch (value) {
        case IOP_CTRL_RUN:
            iop_run(s);
            break;
        case IOP_CTRL_STOP:
        case IOP_CTRL_HALT:
            s->running = false;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: CTRL <- 0x%" PRIx64 "\n",
                          __func__, value);
            break;
        }
        break;
    case IOP_FW_BASE:
        s->fw_base = value;
        s->fw_config = 0;
        break;
    case IOP_FW_SIZE:
        s->fw_size = value;
        break;
    case IOP_SELF:
        s->self_addr = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled write 0x%04" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "\n", __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps iop_ctrl_ops = {
    .read = iop_ctrl_read,
    .write = iop_ctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t iop_vic_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930IOPState *s = opaque;
    int vic = offset / IOP_VIC_STRIDE;
    hwaddr reg = offset % IOP_VIC_STRIDE;

    if (reg >= IOP_VIC_REGS) {
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled read 0x%05" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
    if (reg == VIC_SOFTINT) {
        return s->vic_softint[vic];
    }
    return s->vic_regs[vic][reg / 4];
}

static void iop_vic_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    S5L8930IOPState *s = opaque;
    int vic = offset / IOP_VIC_STRIDE;
    hwaddr reg = offset % IOP_VIC_STRIDE;

    if (reg >= IOP_VIC_REGS) {
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled write 0x%05" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "\n", __func__, offset, value);
        return;
    }
    switch (reg) {
    case VIC_SOFTINT:
        s->vic_softint[vic] |= value;
        if (vic == 0 && (value & IOP_IRQ_DOORBELL)) {
            /* The firmware's IRQ 3 handler clears its own SOFTINT first. */
            s->vic_softint[0] &= ~IOP_IRQ_DOORBELL;
            iop_doorbell(s);
        }
        if (vic == 0 && (value & IOP_IRQ_NMI)) {
            /* Panic-diagnostic NMI; the panic variables read as clean. */
            s->vic_softint[0] &= ~IOP_IRQ_NMI;
            DPRINTF("NMI\n");
#ifdef DEBUG_S5L8930_IOP
            {
                hwaddr cfg = iop_config(s);
                hwaddr ring = iop_ldl(cfg + FW_CFG_RING(0));
                uint32_t n = iop_ldl(cfg + FW_CFG_COUNT(0)), i;

                for (i = 0; i < n && i < 8; i++) {
                    uint32_t w0 = iop_ldl(ring + i * RING_ENTRY_SIZE);
                    DPRINTF("  ring0[%u] w0=0x%08x msg={0x%08x,0x%08x}\n", i, w0,
                            iop_ldl(RING_ITEM(w0)), iop_ldl(RING_ITEM(w0) + 4));
                }
            }
#endif
        }
        break;
    case VIC_SOFTINTCLEAR:
        s->vic_softint[vic] &= ~value;
        break;
    default:
        s->vic_regs[vic][reg / 4] = value;
        break;
    }
}

static const MemoryRegionOps iop_vic_ops = {
    .read = iop_vic_read,
    .write = iop_vic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* ---- device ------------------------------------------------------------ */

static void s5l8930_iop_reset(DeviceState *dev)
{
    S5L8930IOPState *s = S5L8930_IOP(dev);
    int i;

    timer_del(s->irq_timer);
    s->running = false;
    s->fw_base = s->fw_size = s->self_addr = s->fw_config = 0;
    memset(s->vic_softint, 0, sizeof(s->vic_softint));
    memset(s->vic_regs, 0, sizeof(s->vic_regs));
    memset(s->ring_rx, 0, sizeof(s->ring_rx));
    for (i = 0; i < NAND_BUSES; i++) {
        s->bytes_per_page[i] = 8192;
        s->bytes_per_spare[i] = 0x1b4;
        s->pages_per_block[i] = 128;
    }
}

static int64_t geometry_get(QDict *g, const char *key, Error **errp)
{
    QNum *n = qobject_to(QNum, qdict_get(g, key));
    int64_t v;

    if (!n || !qnum_get_try_int(n, &v)) {
        if (!*errp) {
            error_setg(errp, "geometry.json: missing integer \"%s\"", key);
        }
        return -1;
    }
    return v;
}

/* mmap a store file, creating it sparse at the full size when writable. */
static bool nand_map_file(const char *path, size_t size, bool writable,
                          uint8_t **map, int *fdp, Error **errp)
{
    int fd = open(path, writable ? O_RDWR | O_CREAT : O_RDONLY, 0644);

    if (fd < 0 || (writable && ftruncate(fd, size) < 0)) {
        error_setg_errno(errp, errno, "cannot open %s", path);
        return false;
    }
    *map = mmap(NULL, size, writable ? PROT_READ | PROT_WRITE : PROT_READ,
                writable ? MAP_SHARED : MAP_PRIVATE, fd, 0);
    if (*map == MAP_FAILED) {
        error_setg_errno(errp, errno, "cannot map %s", path);
        close(fd);
        return false;
    }
    *fdp = fd;
    return true;
}

/*
 * Stores through the MAP_SHARED maps sit in the host page cache until the
 * kernel writes them back. Every VM stop (the app's Stop, which pauses and
 * then quits; a snapshot; the guest's own power-off) pushes them to disk, so a
 * hard halt loses only what a power cut would, like the iPod's fsync'd pages.
 */
static void iop_vm_state(void *opaque, bool running, RunState state)
{
    S5L8930IOPState *s = opaque;
    size_t size = (size_t)s->pages_per_ce * s->page_stride;
    int64_t t0 = g_get_monotonic_time();

    if (running) {
        return;
    }
    for (int bus = 0; bus < NAND_BUSES; bus++) {
        for (int ce = 0; ce < NAND_CES; ce++) {
            uint8_t *pages = s->overlay_dir ? s->ovl[bus][ce] : s->chip[bus][ce];
            if (pages && msync(pages, size, MS_SYNC) < 0) {
                error_report("s5l8930-iop: msync bus%d-ce%d: %s", bus, ce, strerror(errno));
            }
            if (s->dirty[bus][ce] && msync(s->dirty[bus][ce], s->pages_per_ce / 8, MS_SYNC) < 0) {
                error_report("s5l8930-iop: msync bus%d-ce%d.dirty: %s", bus, ce, strerror(errno));
            }
        }
    }
    info_report("s5l8930-iop: NAND synced on stop in %" PRId64 " ms",
                (g_get_monotonic_time() - t0) / 1000);
}

/*
 * Open the page store under "nand"; with "nand-overlay" the base is mapped
 * read-only and every program/erase lands in the overlay directory (same
 * file layout plus a bus<b>-ce<c>.dirty bitmap of the pages it owns). A
 * device reset is "delete the overlay", a snapshot is "copy it".
 */
static void s5l8930_iop_realize(DeviceState *dev, Error **errp)
{
    S5L8930IOPState *s = S5L8930_IOP(dev);
    g_autofree char *path = NULL;
    g_autofree char *json = NULL;
    QObject *obj;
    QDict *g;
    Error *err = NULL;
    const char *id;
    int64_t page_bytes, spare_bytes, ppb, blocks, ce_per_bus, buses;
    size_t size;
    int bus, ce, dfd;

    if (!s->nand_dir) {
        if (s->overlay_dir) {
            error_setg(errp, "nand-overlay needs a base nand store");
        }
        return;
    }
    if (s->overlay_dir && g_mkdir_with_parents(s->overlay_dir, 0755) < 0) {
        error_setg_errno(errp, errno, "cannot create %s", s->overlay_dir);
        return;
    }
    path = g_strdup_printf("%s/geometry.json", s->nand_dir);
    if (!g_file_get_contents(path, &json, NULL, NULL)) {
        error_setg(errp, "cannot read %s", path);
        return;
    }
    obj = qobject_from_json(json, &err);
    g = qobject_to(QDict, obj);
    if (!g) {
        if (!err) {
            error_setg(&err, "not a JSON object");
        }
        error_propagate_prepend(errp, err, "%s: ", path);
        qobject_unref(obj);
        return;
    }
    page_bytes = geometry_get(g, "page_bytes", &err);
    spare_bytes = geometry_get(g, "spare_bytes", &err);
    ppb = geometry_get(g, "pages_per_block", &err);
    blocks = geometry_get(g, "blocks_per_ce", &err);
    ce_per_bus = geometry_get(g, "ce_per_bus", &err);
    buses = geometry_get(g, "buses", &err);
    id = qdict_get_try_str(g, "chip_id");
    if (err) {
        error_propagate(errp, err);
        qobject_unref(obj);
        return;
    }
    /* The store defines the page geometry; the chip population must match the model. */
    if (page_bytes <= 0 || spare_bytes < FMI_META_BYTES || ppb <= 0 || blocks <= 0 ||
        buses != NAND_BUSES || ce_per_bus != ctpop8(s->nand_ce_mask) ||
        !id || g_ascii_strtoull(id, NULL, 16) != s->nand_id) {
        error_setg(errp, "%s does not match the IOP model (%d buses x %d CE, "
                   "id 0x%08x) or has a bad page geometry", path, NAND_BUSES,
                   ctpop8(s->nand_ce_mask), s->nand_id);
        qobject_unref(obj);
        return;
    }
    qobject_unref(obj);
    for (bus = 0; bus < NAND_BUSES; bus++) {
        s->bytes_per_page[bus] = page_bytes;
        s->bytes_per_spare[bus] = spare_bytes;
        s->pages_per_block[bus] = ppb;
    }
    s->page_stride = page_bytes + spare_bytes;
    s->store_page_bytes = page_bytes;
    s->store_ppb = ppb;
    s->pages_per_ce = blocks * ppb;
    size = (size_t)s->pages_per_ce * s->page_stride;

    for (bus = 0; bus < NAND_BUSES; bus++) {
        for (ce = 0; ce < NAND_CES; ce++) {
            g_autofree char *f = NULL;

            if (!(s->nand_ce_mask & (1u << ce))) {
                continue;
            }
            f = g_strdup_printf("%s/bus%d-ce%d.pages", s->nand_dir, bus, ce);
            if (!nand_map_file(f, size, !s->overlay_dir, &s->chip[bus][ce],
                               &s->chip_fd[bus][ce], errp)) {
                return;
            }
            if (!s->overlay_dir) {
                continue;
            }
            g_free(f);
            f = g_strdup_printf("%s/bus%d-ce%d.pages", s->overlay_dir, bus, ce);
            if (!nand_map_file(f, size, true, &s->ovl[bus][ce],
                               &s->ovl_fd[bus][ce], errp)) {
                return;
            }
            g_free(f);
            f = g_strdup_printf("%s/bus%d-ce%d.dirty", s->overlay_dir, bus, ce);
            if (!nand_map_file(f, s->pages_per_ce / 8, true, &s->dirty[bus][ce],
                               &dfd, errp)) {
                return;
            }
            close(dfd);
        }
    }
    qemu_add_vm_change_state_handler(iop_vm_state, s);
}

static void s5l8930_iop_init(Object *obj)
{
    S5L8930IOPState *s = S5L8930_IOP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    /* K48 part; realize validates the page store against these. */
    for (i = 0; i < NAND_BUSES; i++) {
        s->bytes_per_page[i] = 8192;
        s->bytes_per_spare[i] = 0x1b4;
        s->pages_per_block[i] = 128;
    }

    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, iop_irq_expire, s);
    memory_region_init_io(&s->ctrl_mr, obj, &iop_ctrl_ops, s,
                          TYPE_S5L8930_IOP, S5L8930_IOP_SIZE);
    sysbus_init_mmio(sbd, &s->ctrl_mr);
    memory_region_init_io(&s->vic_mr, obj, &iop_vic_ops, s,
                          TYPE_S5L8930_IOP ".vic", S5L8930_IOP_VIC_SIZE);
    sysbus_init_mmio(sbd, &s->vic_mr);
}

static const VMStateDescription vmstate_s5l8930_iop = {
    .name = TYPE_S5L8930_IOP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(irq_timer, S5L8930IOPState),
        VMSTATE_BOOL(running, S5L8930IOPState),
        VMSTATE_UINT32(fw_base, S5L8930IOPState),
        VMSTATE_UINT32(fw_size, S5L8930IOPState),
        VMSTATE_UINT32(self_addr, S5L8930IOPState),
        VMSTATE_UINT32_ARRAY(vic_softint, S5L8930IOPState, IOP_VIC_COUNT),
        VMSTATE_UINT32_2DARRAY(vic_regs, S5L8930IOPState, IOP_VIC_COUNT,
                               IOP_VIC_REGS / 4),
        VMSTATE_UINT32_ARRAY(ring_rx, S5L8930IOPState, IOP_MAX_ENDPOINTS),
        VMSTATE_UINT32_ARRAY(bytes_per_page, S5L8930IOPState, NAND_BUSES),
        VMSTATE_UINT32_ARRAY(bytes_per_spare, S5L8930IOPState, NAND_BUSES),
        VMSTATE_UINT32_ARRAY(pages_per_block, S5L8930IOPState, NAND_BUSES),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Samsung 0x7294D7EC: 8 KiB pages, 128 pages/block, 0x1038 blocks/CE, the
 * iBoot-817 chip table's part for the 16 GB K48 (2 buses x 2 CEs). The DT
 * nand node (#ce, #ce-blocks, #block-pages, #page-bytes, #spare-bytes,
 * device-readid) must describe the same part.
 */
static const Property s5l8930_iop_properties[] = {
    DEFINE_PROP_UINT32("nand-id", S5L8930IOPState, nand_id, 0xb614d5ad),
    DEFINE_PROP_UINT8("nand-ce-mask", S5L8930IOPState, nand_ce_mask, 0xf),
    DEFINE_PROP_STRING("nand", S5L8930IOPState, nand_dir),
    DEFINE_PROP_STRING("nand-overlay", S5L8930IOPState, overlay_dir),
    DEFINE_PROP_LINK("sdio", S5L8930IOPState, sdio, TYPE_DEVICE, DeviceState *),
};

static void s5l8930_iop_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8930_iop_realize;
    dc->vmsd = &vmstate_s5l8930_iop;
    device_class_set_props(dc, s5l8930_iop_properties);
    device_class_set_legacy_reset(dc, s5l8930_iop_reset);
}

static const TypeInfo s5l8930_iop_info = {
    .name          = TYPE_S5L8930_IOP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930IOPState),
    .instance_init = s5l8930_iop_init,
    .class_init    = s5l8930_iop_class_init,
};

static void s5l8930_iop_register_types(void)
{
    type_register_static(&s5l8930_iop_info);
}

type_init(s5l8930_iop_register_types)
