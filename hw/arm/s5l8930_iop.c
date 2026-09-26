/*
 * S5L8930 ("A4") IOP: high-level emulation of the second core's host-visible
 * surface. The IOP firmware (IOP_S5L8930X_firmware, "EmbeddedIOP for
 * s5l8930x", iBoot-817.29) never executes here; this device answers what the
 * kernel can observe of it: the AP-side control block at 0x86300000, the
 * doorbell SOFTINT in the IOP's own VIC window at 0xBF300000, and the "qwi"
 * message rings in guest DRAM that AppleS5L8920XARM7M and AppleS5L8920XIOPFMI
 * drive through it.
 *
 * Protocol: docs/ipad1/research/gap-iop-mailbox-protocol.md §2-§3, checked
 * against the 7B500 kexts (ARM7M __text c04d2000, IOPFMI __text c04e8000) and
 * the firmware blob (fw offsets below are into the 0x1b000-byte image).
 *
 * NAND is a blank chip: every page reads as erased. read/program/erase go
 * through nand_read_page()/nand_program_page()/nand_erase_block() so a
 * file-backed page store can replace them.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "hw/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930IOPState, S5L8930_IOP)

#ifdef DEBUG_S5L8930_IOP
#define DPRINTF(fmt, ...) fprintf(stderr, "s5l8930_iop: " fmt, ## __VA_ARGS__)
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

/* Firmware image layout (constants published by the firmware kext). */
#define FW_CONFIG           0xf018      /* ConfigurationOffset */
#define FW_CONFIG_MAGIC     0x636e6667  /* 'cnfg' */
#define FW_BSS_START        0xf160      /* fw[0x318]; start() zeroes bss */
#define FW_BSS_END          0x1b000     /* fw[0x31c]; PanicString etc. live here */
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

struct S5L8930IOPState {
    SysBusDevice parent_obj;
    MemoryRegion ctrl_mr;
    MemoryRegion vic_mr;

    uint32_t nand_id;       /* the 4 ID bytes the kext compares, LE packed */
    uint8_t nand_ce_mask;   /* CE slots populated on each bus */

    bool running;
    uint32_t fw_base;
    uint32_t fw_size;
    uint32_t self_addr;
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
static void iop_raise_ap_irq(void)
{
    iop_stl(S5L8930_VIC_BASE(S5L8930_IRQ_IOP / 32) + VIC_SOFTINT,
            1u << (S5L8930_IRQ_IOP % 32));
}

/* ---- NAND ------------------------------------------------------------- */

/*
 * ponytail: blank NAND. Replace these three with a file-backed page store;
 * everything above them only sees status codes and page buffers.
 */
static uint32_t nand_read_page(S5L8930IOPState *s, int bus, uint32_t ce,
                               uint32_t page, uint8_t *data, uint8_t *meta)
{
    memset(data, 0xff, s->bytes_per_page[bus] + s->bytes_per_spare[bus]);
    memset(meta, 0xff, FMI_META_BYTES);
    return FMI_STATUS_BLANK;
}

static uint32_t nand_program_page(S5L8930IOPState *s, int bus, uint32_t ce,
                                  uint32_t page, const uint8_t *data,
                                  const uint8_t *meta)
{
    DPRINTF("program bus %d ce %u page 0x%x (dropped)\n", bus, ce, page);
    return FMI_STATUS_OK;
}

static uint32_t nand_erase_block(S5L8930IOPState *s, int bus, uint32_t ce,
                                 uint32_t block)
{
    DPRINTF("erase bus %d ce %u block 0x%x (dropped)\n", bus, ce, block);
    return FMI_STATUS_OK;
}

/* ---- FMI ------------------------------------------------------------- */

#define CMD_GET(cmd, off)       ldl_le_p((cmd) + (off))
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
    uint32_t bpp = CMD_GET(cmd, 0x24);
    uint32_t spare = CMD_GET(cmd, 0x28);

    if (CMD_GET(cmd, 0x10) != bus || bpp == 0 || bpp > FMI_MAX_PAGE ||
        spare > FMI_MAX_PAGE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad config bus %u page %u spare %u\n",
                      __func__, CMD_GET(cmd, 0x10), bpp, spare);
        return FMI_STATUS_PARAM;
    }
    s->bytes_per_page[bus] = bpp;
    s->bytes_per_spare[bus] = spare;
    s->pages_per_block[bus] = CMD_GET(cmd, 0x1c);
    DPRINTF("set_config bus %d: %u CEs (mask 0x%x), %u pages/block, %u+%u bytes\n",
            bus, CMD_GET(cmd, 0x14), CMD_GET(cmd, 0x18),
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
    iop_write(CMD_GET(cmd, 0x10), ids, sizeof(ids));
    return FMI_STATUS_OK;
}

/* Single-page reads: +0x10 ce, +0x14 page, +0x18 data, +0x1c meta (op 4). */
static uint32_t fmi_read_single(S5L8930IOPState *s, int bus, uint8_t *cmd,
                                uint8_t *page, uint8_t *meta)
{
    uint32_t st = nand_read_page(s, bus, CMD_GET(cmd, 0x10), CMD_GET(cmd, 0x14),
                                 page, meta);

    iop_write(CMD_GET(cmd, 0x18), page, s->bytes_per_page[bus]);
    iop_write(CMD_GET(cmd, 0x1c), meta, FMI_META_BYTES);
    return st;
}

/* h2fmi_read_raw_page (fw 0x1be4): data then spare, no ECC, at +0x18. */
static uint32_t fmi_read_raw(S5L8930IOPState *s, int bus, uint8_t *cmd,
                             uint8_t *page, uint8_t *meta)
{
    nand_read_page(s, bus, CMD_GET(cmd, 0x10), CMD_GET(cmd, 0x14), page, meta);
    iop_write(CMD_GET(cmd, 0x18), page,
              s->bytes_per_page[bus] + s->bytes_per_spare[bus]);
    return FMI_STATUS_OK;
}

/* h2fmi_iop_read_bootpage (fw 0x1ab8): +0x18 gets the 0x600-byte boot page. */
static uint32_t fmi_read_bootpage(S5L8930IOPState *s, int bus, uint8_t *cmd,
                                  uint8_t *page, uint8_t *meta)
{
    uint32_t st = nand_read_page(s, bus, CMD_GET(cmd, 0x10), CMD_GET(cmd, 0x14),
                                 page, meta);

    iop_write(CMD_GET(cmd, 0x18), page,
              MIN(FMI_BOOTPAGE_BYTES, s->bytes_per_page[bus]));
    return st;
}

/*
 * h2fmi_iop_read_multiple / write_multiple (fw 0x1f94 / 0x1c30, kext
 * c04eb510): +0x10 count, +0x14 CE array, +0x18 page array, +0x1c/+0x20
 * data segment list and byte length, +0x24/+0x28 meta segment list and
 * length, +0x30 AES (ignored). Outputs: +0x5c pages completed, +0x60 final
 * status, +0x70/+0x74 failing CE/index (-1 = none).
 */
static uint32_t fmi_multi(S5L8930IOPState *s, int bus, uint8_t *cmd, bool write,
                          uint8_t *page, uint8_t *meta)
{
    uint32_t n = CMD_GET(cmd, 0x10);
    hwaddr ces = CMD_GET(cmd, 0x14), pages = CMD_GET(cmd, 0x18);
    uint32_t blank = 0, uecc = 0, st;
    SegCursor data, metas;
    uint32_t i;

    if (n == 0 || n > FMI_MAX_MULTI || !ces || !pages ||
        !CMD_GET(cmd, 0x1c) || !CMD_GET(cmd, 0x24)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad multi-page command (%u pages)\n",
                      __func__, n);
        return FMI_STATUS_PARAM;
    }
    seg_cursor_init(&data, CMD_GET(cmd, 0x1c), CMD_GET(cmd, 0x20));
    seg_cursor_init(&metas, CMD_GET(cmd, 0x24), CMD_GET(cmd, 0x28));

    for (i = 0; i < n; i++) {
        uint32_t ce = iop_ldl(ces + 4 * i), pg = iop_ldl(pages + 4 * i);

        if (write) {
            seg_copy(&data, page, s->bytes_per_page[bus], false);
            seg_copy(&metas, meta, FMI_META_BYTES, false);
            nand_program_page(s, bus, ce, pg, page, meta);
        } else {
            st = nand_read_page(s, bus, ce, pg, page, meta);
            blank += st == FMI_STATUS_BLANK;
            uecc += st == FMI_STATUS_UECC;
            seg_copy(&data, page, s->bytes_per_page[bus], true);
            seg_copy(&metas, meta, FMI_META_BYTES, true);
        }
    }
    st = write ? FMI_STATUS_OK : fmi_multi_status(n, blank, uecc);
    CMD_SET(cmd, 0x5c, n);
    CMD_SET(cmd, 0x60, st);
    CMD_SET(cmd, 0x70, 0xffffffff);
    CMD_SET(cmd, 0x74, 0xffffffff);
    return st;
}

/* Erase single (fw 0x240c): +0x10 ce, +0x14 block; out +0x18 count, +0x20 status word. */
static uint32_t fmi_erase_single(S5L8930IOPState *s, int bus, uint8_t *cmd)
{
    uint32_t st = nand_erase_block(s, bus, CMD_GET(cmd, 0x10), CMD_GET(cmd, 0x14));

    CMD_SET(cmd, 0x18, 1);
    CMD_SET(cmd, 0x20, st == FMI_STATUS_OK ? 0 : FMI_STATUS_UECC);
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
    uint32_t op, st;

    iop_read(item, cmd, sizeof(cmd));
    op = CMD_GET(cmd, CMD_OPCODE);
    DPRINTF("fmi%d op %u at 0x%" HWADDR_PRIx "\n", bus, op, item);

    switch (op) {
    case FMI_OP_SET_CONFIG:
        st = fmi_set_config(s, bus, cmd);
        break;
    case FMI_OP_RESET_EVERYTHING:
        st = fmi_reset_everything(s, bus, cmd);
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
        iop_read(CMD_GET(cmd, 0x18), page, s->bytes_per_page[bus]);
        iop_read(CMD_GET(cmd, 0x1c), meta, FMI_META_BYTES);
        st = nand_program_page(s, bus, CMD_GET(cmd, 0x10), CMD_GET(cmd, 0x14),
                               page, meta);
        break;
    case FMI_OP_WRITE_RAW:
    case FMI_OP_WRITE_BOOTPAGE:
        iop_read(CMD_GET(cmd, 0x18), page, s->bytes_per_page[bus]);
        st = nand_program_page(s, bus, CMD_GET(cmd, 0x10), CMD_GET(cmd, 0x14),
                               page, NULL);
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
    hwaddr cfg = s->fw_base + FW_CONFIG;
    bool any = false;
    int i;

    if (!s->running) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: doorbell while stopped\n", __func__);
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
        iop_raise_ap_irq();
    }
}

/* CTRL = 1: what the firmware's start() does that the AP can see. */
static void iop_run(S5L8930IOPState *s)
{
    g_autofree uint8_t *zero = g_malloc0(FW_BSS_END - FW_BSS_START);
    uint32_t magic = iop_ldl(s->fw_base + FW_CONFIG);

    if (magic != FW_CONFIG_MAGIC) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: no 'cnfg' block at firmware base 0x%08x\n",
                      __func__, s->fw_base);
    } else {
        /* bss (PanicString, PanicFunction, PanicLog...) reads as clean. */
        iop_write(s->fw_base + FW_BSS_START, zero, FW_BSS_END - FW_BSS_START);
    }
    memset(s->ring_rx, 0, sizeof(s->ring_rx));
    s->running = true;
    DPRINTF("run: firmware at 0x%08x size 0x%x, message buffer 0x%08x\n",
            s->fw_base, s->fw_size, iop_ldl(s->fw_base + FW_CONFIG + FW_CFG_MSGBUF));
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

    s->running = false;
    s->fw_base = s->fw_size = s->self_addr = 0;
    memset(s->vic_softint, 0, sizeof(s->vic_softint));
    memset(s->vic_regs, 0, sizeof(s->vic_regs));
    memset(s->ring_rx, 0, sizeof(s->ring_rx));
    for (i = 0; i < NAND_BUSES; i++) {
        s->bytes_per_page[i] = 8192;
        s->bytes_per_spare[i] = 0x1b4;
        s->pages_per_block[i] = 128;
    }
}

static void s5l8930_iop_init(Object *obj)
{
    S5L8930IOPState *s = S5L8930_IOP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

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
    DEFINE_PROP_UINT32("nand-id", S5L8930IOPState, nand_id, 0x7294d7ec),
    DEFINE_PROP_UINT8("nand-ce-mask", S5L8930IOPState, nand_ce_mask, 0x3),
};

static void s5l8930_iop_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

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
