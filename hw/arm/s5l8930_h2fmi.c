/*
 * S5L8930 ("A4") H2FMI: the two NAND flash interfaces as iBoot drives them
 * directly. The kernel never touches these registers (its NAND goes through
 * the IOP firmware, s5l8930_iop.c); iBoot-817.29's drivers/apple/h2fmi does,
 * and this model answers exactly what that driver uses. Pages come from the
 * IOP's page store, so iBoot and the kernel see the same flash.
 *
 * Each interface has three 4 KiB windows (DT flash-controller0 reg): FMI at
 * +0, FMC (the NAND bus sequencer) at +0x40000, ECC at +0x80000. Register
 * roles, from iBoot 7B500 (addresses are iBoot VAs):
 *
 *   FMI +0x04 control: 3 = move the selected page (or ID) into the FIFOs
 *        (0x5ff037a0 read-id, 0x5ff04234 page); other values reset/flush.
 *   FMI +0x0C status, W1C: bit1 transfer done (0x5ff037a6, 0x5ff04440),
 *        bit8 = an enabled FMC event (0x5ff03c7a).
 *   FMI +0x10 interrupt enable (2, 0x100, 0x100000).
 *   FMI +0x34 page format: bits 0-7 sectors, bits 19-24 meta bytes.
 *   FMI +0x14 data FIFO (PIO 0x5ff03462, DMA 0x5ff03b86), +0x18 meta FIFO
 *        (DMA 0x5ff03bcc), +0x1C FIFO level: &0x18 = data ready (0x5ff03400).
 *   FMC +0x0C chip enable, one bit per CE (0x5ff03d82).
 *   FMC +0x10 go: bit0 command 1, bit3 address, bit1 command 2, 0x50 poll
 *        status until it matches +0x4C (0x5ff03a58, 0x5ff0431e).
 *   FMC +0x14 commands: byte0 first, byte1 second (0x00/0x30 read, 0x90 id,
 *        0xFF reset, 0x70 status).
 *   FMC +0x18/+0x1C address bytes (row = +0x18 bits 16-31 | +0x1C bits 0-7,
 *        0x5ff0332c), +0x20 address byte count - 1.
 *   FMC +0x40 event enable, +0x44 events W1C: bit0 cmd1, bit1 cmd2, bit3
 *        address, bit5 status matched (0x5ff03f9a); +0x48 last status byte.
 *   ECC +0x0C per-sector result, read once per sector (0x5ff04008): bit1
 *        blank, bit2 uncorrectable, bits 16-20 corrected bits. +0x10 page
 *        summary, W1C (0x5ff04246): bit3 = some sector had an error.
 *
 * Page data and meta come straight from the store, as on flash: meta stays
 * whitened, iBoot's FIL un-whitens it in software (0x5ff03af0) just as the
 * kernel does. There is no ECC to correct: a stored page is clean, a hole or
 * an erased page reads blank. Multi-page reads run one CDMA chain across all
 * pages while the FMI fills its FIFO a page at a time, so the FIFO windows
 * pace their channels (s5l8930_cdma_set_source).
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/arm/s5l8930.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930H2FMIState, S5L8930_H2FMI)

#define H2FMI_BUSES     2
#define H2FMI_WINDOW    0x100000        /* FMI1 = FMI0 + 1 MiB */
#define H2FMI_BUF       0x8800          /* > largest page + spare */
/*
 * Pages queued ahead of the consumer. The IOP firmware issues a multi-page
 * read's transfers before its CDMA chain drains them, and real hardware
 * stalls the transfer until the FIFO has room; this model transfers at
 * once and buffers instead (ponytail: capacity in place of backpressure;
 * a transfer that finds no room is logged and dropped).
 */
#define H2FMI_QUEUE     64

#define FMI_CONTROL     0x04
#define FMI_STATUS      0x0C
#define FMI_INTEN       0x10
#define FMI_DATA        0x14
#define FMI_META        0x18
#define FMI_LEVEL       0x1C
#define FMI_FORMAT      0x34
#define FMC_BASE        0x40000
#define FMC_CE          0x0C
#define FMC_GO          0x10
#define FMC_CMD         0x14
#define FMC_ADDR0       0x18
#define FMC_ADDR1       0x1C
#define FMC_EVTEN       0x40
#define FMC_EVENTS      0x44
#define FMC_NAND_STATUS 0x48
#define ECC_BASE        0x80000
#define ECC_SECTOR      0x0C
#define ECC_SUMMARY     0x10

#define FMI_ST_DONE     (1u << 1)
#define FMI_ST_FMC      (1u << 8)
#define FMC_EV_STATUS   (1u << 5)
#define ECC_BLANK       (1u << 1)
#define NAND_READY      0xe0            /* ready, ready, not write-protected */
#define META_BYTES      10              /* what the store keeps per page */

typedef enum { MODE_NONE, MODE_ID, MODE_PAGE } H2FMIMode;

typedef struct H2FMIBus {
    struct S5L8930H2FMIState *s;
    int n;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t fmi[0x40 / 4];
    uint32_t fmc[0x50 / 4];
    uint32_t ecc_summary;
    /* per-page sector results, oldest first: iBoot reads page i's while
     * page i+1 is already transferring (0x5ff0424c) */
    uint32_t ecc_q[H2FMI_QUEUE];
    uint32_t ecc_n, ecc_reads;
    H2FMIMode mode;
    uint32_t row;
    /* Each chip latches its own page at the read command: the IOP firmware
     * commands the next CE before it transfers the previous one. */
    bool page_ok[8];                    /* loaded page has data (else blank) */
    uint32_t page_row[8];
    uint8_t page[8][H2FMI_BUF];
    uint32_t stride;
    uint8_t data[H2FMI_QUEUE * H2FMI_BUF], meta[H2FMI_QUEUE * META_BYTES];
    uint32_t data_len, meta_len;
    /* Program path (the IOP firmware): cmd 0x80 + address, the CDMA fills the
     * FIFOs, control 0x5 moves them to the chip, cmd 0x10 confirms. */
    bool writing;
    uint8_t wdata[H2FMI_BUF], wmeta[64];
    uint32_t wdata_len, wmeta_len;
    /* Each chip's page register: filled by a write transfer (control 5),
     * programmed by 0x10 (program) or 0x11 (cache program, next page follows). */
    uint8_t wpage[8][H2FMI_BUF], wpmeta[8][META_BYTES];
    uint32_t wpage_len[8];
    uint32_t erase_row;
    /* Transfers waiting for FIFO room (real hardware stalls the transfer;
     * DONE follows the transfer, so the firmware's wait is faithful). */
    int8_t pending_ce[256];
    uint32_t pending_n;
} H2FMIBus;

struct S5L8930H2FMIState {
    SysBusDevice parent_obj;
    DeviceState *iop;                   /* owns the page store */
    DeviceState *cdma;
    H2FMIBus bus[H2FMI_BUSES];
};

#define HT(...) do { static int on_ = -1; if (on_ < 0) on_ = getenv("H2FMI_TRACE") != NULL; \
                     if (on_) qemu_log(__VA_ARGS__); } while (0)

/* FMI status as read: bit 8 follows the enabled FMC events. */
static uint32_t h2fmi_status(H2FMIBus *b)
{
    bool fmc = b->fmc[FMC_EVENTS / 4] & b->fmc[FMC_EVTEN / 4];

    return b->fmi[FMI_STATUS / 4] | (fmc ? FMI_ST_FMC : 0);
}

static void h2fmi_update_irq(H2FMIBus *b)
{
    qemu_set_irq(b->irq, (h2fmi_status(b) & b->fmi[FMI_INTEN / 4]) != 0);
}

static int h2fmi_ce(H2FMIBus *b)
{
    uint32_t m = b->fmc[FMC_CE / 4];
    return m ? ctz32(m) : -1;
}

static void h2fmi_fmc_events(H2FMIBus *b, uint32_t ev)
{
    b->fmc[FMC_EVENTS / 4] |= ev;
    h2fmi_update_irq(b);
}

static void h2fmi_command(H2FMIBus *b, uint8_t cmd)
{
    S5L8930H2FMIState *s = b->s;

    switch (cmd) {
    case 0x90:
        b->mode = MODE_ID;
        break;
    case 0xff:
        b->mode = MODE_NONE;
        b->data_len = b->meta_len = 0;
        break;
    case 0x70:              /* status, then 0x00 back to the page register */
    case 0x00:
        break;
    case 0x80:              /* page program: address follows, data through the FIFOs */
    case 0x81:              /* the next page of a cache program */
        b->writing = true;
        break;
    case 0x10:              /* program: the chip's page register goes to the flash */
    case 0x11: {            /* cache program: the same, the next page follows */
        int ce = h2fmi_ce(b);
        uint32_t id, pb;
        uint8_t mask;

        if (ce >= 0 && s->iop && b->wpage_len[ce & 7]) {
            s5l8930_iop_nand_info(s->iop, &id, &mask, &pb);
            int per_bus = MAX(ctpop8(mask), 1);
            int cs = (ce & 7) * H2FMI_BUSES + b->n;
            s5l8930_iop_nand_program(s->iop, cs / per_bus, cs % per_bus, b->row,
                                     b->wpage[ce & 7], b->wpage_len[ce & 7], b->wpmeta[ce & 7]);
            b->wpage_len[ce & 7] = 0;
        }
        b->writing = false;
        b->fmc[FMC_NAND_STATUS / 4] = NAND_READY;
        break;
    }
    case 0x60:              /* block erase: address follows, 0xd0 confirms */
        break;
    case 0xd0: {
        int ce = h2fmi_ce(b);
        uint32_t id, pb;
        uint8_t mask;

        if (ce >= 0 && s->iop) {
            s5l8930_iop_nand_info(s->iop, &id, &mask, &pb);
            int per_bus = MAX(ctpop8(mask), 1);
            int cs = (ce & 7) * H2FMI_BUSES + b->n;
            s5l8930_iop_nand_erase(s->iop, cs / per_bus, cs % per_bus, b->row);
        }
        b->fmc[FMC_NAND_STATUS / 4] = NAND_READY;
        break;
    }
    case 0x30: {
        int ce = h2fmi_ce(b);
        /*
         * A new page: whatever the last read left in the FIFOs was never
         * going to be taken (a pipelined read has drained the previous page
         * by now), and must not shift this page's data.
         */
        /* The IOP firmware pipelines: it issues the next page's command
         * while the CDMA still drains this one, so the FIFOs keep what a
         * transfer put there; only reset (0xff) or a transfer with an idle
         * pipeline starts them afresh. */
        b->mode = MODE_PAGE;
        if (ce >= 0 && s->iop) {
            b->page_ok[ce & 7] = false;
            b->page_row[ce & 7] = b->row;
            /*
             * iBoot numbers chip selects round-robin across the buses
             * (h2fmiInitVirtToPhysMap, 0x5ff026f8: CS 0 = FMI0 CE0, CS 1 =
             * FMI1 CE8, CS 2 = FMI0 CE1, ...); the page store is laid out
             * bus-major, CS n at bus<n / ce_per_bus>-ce<n % ce_per_bus>, as
             * the IOP model serves it to the kernel. Map through the CS so
             * both see the same flash.
             */
            uint32_t id, pb;
            uint8_t mask;
            s5l8930_iop_nand_info(s->iop, &id, &mask, &pb);
            int per_bus = MAX(ctpop8(mask), 1);
            int cs = getenv("H2FMI_IDENTITY") ? -1 : (ce & 7) * H2FMI_BUSES + b->n;
            b->page_ok[ce & 7] = cs < 0 ? s5l8930_iop_nand_read(s->iop, b->n, ce & 7, b->row, b->page[ce & 7], &b->stride)
                                : s5l8930_iop_nand_read(s->iop, cs / per_bus,
                                               cs % per_bus, b->row, b->page[ce & 7],
                                               &b->stride);
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8930.h2fmi%d: NAND command 0x%02x\n",
                      b->n, cmd);
        break;
    }
}

static void h2fmi_go(H2FMIBus *b, uint32_t go)
{
    uint32_t cmds = b->fmc[FMC_CMD / 4];

    HT("h%d go 0x%x cmds 0x%x a0 0x%x a1 0x%x ce 0x%x dl %u ml %u\n", b->n, go, cmds, b->fmc[FMC_ADDR0 / 4],
       b->fmc[FMC_ADDR1 / 4], b->fmc[FMC_CE / 4], b->data_len, b->meta_len);

    if (go & 1) {
        h2fmi_command(b, cmds & 0xff);
    }
    if (go & 8) {
        b->row = (b->fmc[FMC_ADDR0 / 4] >> 16) |
                 ((b->fmc[FMC_ADDR1 / 4] & 0xff) << 16);
    }
    if (go & 2) {
        h2fmi_command(b, (cmds >> 8) & 0xff);
    }
    if (go & 0x40) {
        /* Status poll: the part is always ready. */
        b->fmc[FMC_NAND_STATUS / 4] = NAND_READY;
        h2fmi_fmc_events(b, (go & 0xb) | FMC_EV_STATUS);
        return;
    }
    h2fmi_fmc_events(b, go & 0xb);
}

/* Control 3: the selected page (or the ID) goes into the FIFOs. */
static bool h2fmi_room(H2FMIBus *b, uint32_t page_bytes)
{
    return b->data_len + page_bytes <= sizeof(b->data) &&
           b->meta_len + META_BYTES <= sizeof(b->meta) && b->ecc_n < ARRAY_SIZE(b->ecc_q);
}

static void h2fmi_transfer(H2FMIBus *b, int ce)
{
    S5L8930H2FMIState *s = b->s;
    uint32_t page_bytes = 0, id = 0;
    uint8_t ce_mask = 0;

    if (s->iop) {
        s5l8930_iop_nand_info(s->iop, &id, &ce_mask, &page_bytes);
    }
    if (b->mode == MODE_ID) {
        uint8_t idb[8] = { 0 };
        if (ce >= 0 && (ce_mask & (1u << ce))) {
            stl_le_p(idb, id);
        }
        memcpy(b->data, idb, sizeof(idb));
        b->data_len = sizeof(idb);
    } else if (b->mode == MODE_PAGE && page_bytes && (b->pending_n || !h2fmi_room(b, page_bytes))) {
        /* No room, or older transfers still waiting: this one queues behind them
         * (the FIFO's order is the firmware's transfer order) and goes when the
         * CDMA drains (h2fmi_drain). */
        if (b->pending_n < ARRAY_SIZE(b->pending_ce)) {
            b->pending_ce[b->pending_n++] = ce;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "s5l8930.h2fmi%d: transfer queue full, page dropped\n", b->n);
        }
        return;
    } else if (b->mode == MODE_PAGE && page_bytes) {
        /* FMI +0x34 bits 19-24: meta bytes per page (0x5ff0398c). */
        uint32_t meta = MIN(META_BYTES,
                            sizeof(b->meta) - b->meta_len);
        uint8_t *m = b->meta + b->meta_len;

        bool ok = ce >= 0 && b->page_ok[ce & 7];
        const uint8_t *pg = b->page[ce & 7];

        if (ok) {
            memcpy(b->data + b->data_len, pg, page_bytes);
            memset(m, 0, meta);
            memcpy(m, pg + page_bytes, MIN(meta, META_BYTES));
        } else {
            memset(b->data + b->data_len, 0xff, page_bytes);
            memset(m, 0xff, meta);
        }
        b->data_len += page_bytes;
        b->meta_len += meta;
        b->ecc_q[b->ecc_n++] = ok ? 0 : ECC_BLANK;
        b->ecc_summary = 0;
    }
    b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
    h2fmi_update_irq(b);
    if (s->cdma) {
        s5l8930_cdma_kick(s->cdma);
    }
}

/* A write transfer (control 5) completes when the FIFOs hold the page. */
static void h2fmi_write_check(H2FMIBus *b)
{
    uint32_t page_bytes = 0, id = 0;
    uint8_t mask;

    if ((b->fmi[FMI_CONTROL / 4] & 7) != 5) {
        return;
    }
    if (b->s->iop) {
        s5l8930_iop_nand_info(b->s->iop, &id, &mask, &page_bytes);
    }
    if (b->wdata_len >= page_bytes && page_bytes && !(b->fmi[FMI_STATUS / 4] & FMI_ST_DONE)) {
        int ce = h2fmi_ce(b);
        uint32_t mtake = MIN(META_BYTES, b->wmeta_len);

        if (ce >= 0) {
            memcpy(b->wpage[ce & 7], b->wdata, page_bytes);
            memset(b->wpmeta[ce & 7], 0, META_BYTES);
            memcpy(b->wpmeta[ce & 7], b->wmeta, mtake);
            b->wpage_len[ce & 7] = page_bytes;
        }
        memmove(b->wdata, b->wdata + page_bytes, b->wdata_len - page_bytes);
        b->wdata_len -= page_bytes;
        memmove(b->wmeta, b->wmeta + mtake, b->wmeta_len - mtake);
        b->wmeta_len -= mtake;
        b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
        h2fmi_update_irq(b);
        if (b->s->cdma) {
            if (!b->wdata_len) {   /* the chains that filled the FIFOs have been taken */
                s5l8930_cdma_sink_done(b->s->cdma, S5L8930_H2FMI_BASE + b->n * H2FMI_WINDOW, H2FMI_WINDOW);
            }
            s5l8930_cdma_kick(b->s->cdma);      /* room again: a stalled chain continues */
        }
    }
}

/* Room again: the transfers that waited go now, oldest first. */
static void h2fmi_drain(H2FMIBus *b)
{
    uint32_t page_bytes = 0, id = 0;
    uint8_t mask;

    if (!b->pending_n || !b->s->iop) {
        return;
    }
    s5l8930_iop_nand_info(b->s->iop, &id, &mask, &page_bytes);
    while (b->pending_n && h2fmi_room(b, page_bytes)) {
        int ce = b->pending_ce[0];

        memmove(b->pending_ce, b->pending_ce + 1, --b->pending_n);
        h2fmi_transfer(b, ce);
    }
}

static uint32_t fifo_pop(uint8_t *buf, uint32_t *len, unsigned size)
{
    uint32_t v = 0;
    unsigned n = MIN(size, *len);

    for (unsigned i = 0; i < n; i++) {
        v |= buf[i] << (8 * i);
    }
    memmove(buf, buf + n, *len - n);
    *len -= n;
    return v;
}

/* One read per sector (FMI +0x34 bits 0-7); the last one retires the page. */
static uint32_t h2fmi_ecc_sector(H2FMIBus *b)
{
    uint32_t sectors = MAX(b->fmi[FMI_FORMAT / 4] & 0xff, 1);
    uint32_t v;

    if (!b->ecc_n) {
        return 0;
    }
    if (b->ecc_reads >= sectors) {
        /* All sectors read: the next page's results, if a later transfer
         * queued them; else the same page's again (the IOP firmware re-reads
         * a page's results after its second control write). */
        b->ecc_reads = 0;
        if (b->ecc_n > 1) {
            memmove(b->ecc_q, b->ecc_q + 1, sizeof(b->ecc_q) - sizeof(b->ecc_q[0]));
            b->ecc_n--;
        }
    }
    v = b->ecc_q[0];
    b->ecc_reads++;
    return v;
}

static uint64_t h2fmi_read(void *opaque, hwaddr off, unsigned size)
{
    H2FMIBus *b = opaque;

    if (off < sizeof(b->fmi)) {
        switch (off) {
        case FMI_DATA: {
            uint32_t v = fifo_pop(b->data, &b->data_len, size);
            h2fmi_drain(b);
            return v;
        }
        case FMI_META: {
            uint32_t v = fifo_pop(b->meta, &b->meta_len, size);
            h2fmi_drain(b);
            return v;
        }
        case FMI_LEVEL:
            return b->data_len ? 0x18 : 0;
        case FMI_STATUS:
            return h2fmi_status(b);
        default:
            return b->fmi[off / 4];
        }
    }
    if (off >= FMC_BASE && off < FMC_BASE + sizeof(b->fmc)) {
        return b->fmc[(off - FMC_BASE) / 4];
    }
    if (off == ECC_BASE + ECC_SECTOR) {
        uint32_t v = h2fmi_ecc_sector(b);
        HT("h%d ecc -> 0x%x (n %u r %u)\n", b->n, v, b->ecc_n, b->ecc_reads);
        return v;
    }
    if (off == ECC_BASE + ECC_SUMMARY) {
        return b->ecc_summary;
    }
    qemu_log_mask(LOG_UNIMP, "s5l8930.h2fmi%d: read 0x%" HWADDR_PRIx "\n",
                  b->n, off);
    return 0;
}

static void h2fmi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    H2FMIBus *b = opaque;
    uint32_t v = val;

    if (off < sizeof(b->fmi)) {
        switch (off) {
        case FMI_CONTROL:
            HT("h%d ctl 0x%x prev 0x%x dl %u ml %u ecc %u/%u pend %u fmt 0x%x\n", b->n, v, b->fmi[off / 4],
               b->data_len, b->meta_len, b->ecc_n, b->ecc_reads, b->pending_n, b->fmi[FMI_FORMAT / 4]);
            /*
             * A read transfer starts when a write enters read mode (bits 0-1
             * become 3) or, in read mode, raises bit 7 (the next page of a
             * pipelined read). Rewriting the mode is not a new page: both
             * drivers clear bit 7 with a read-modify-write once a page's ECC
             * is read (iBoot-817 0x5ff04066: 3 -> 3; the IOP firmware:
             * 0x83 -> 3). iBoot pipelines with 0x82 then 3 (0x5ff0422e), the
             * IOP firmware with 3 then 0x83.
             */
            if ((v & 7) == 5) {
                /* Write transfer: done once the FIFOs hold the page. */
                b->fmi[off / 4] = v;
                h2fmi_write_check(b);
                return;
            }
            uint32_t prev = b->fmi[off / 4];
            if ((v & 3) == 3 && ((prev & 3) != 3 || ((v & 0x80) && !(prev & 0x80)))) {
                if (!(b->fmi[off / 4] & 0x80)) {
                    b->ecc_n = b->ecc_reads = 0;
                }
                h2fmi_transfer(b, h2fmi_ce(b));
            }
            b->fmi[off / 4] = v;
            return;
        case FMI_STATUS:
            b->fmi[off / 4] &= ~v;
            break;
        case FMI_DATA:
            if (b->wdata_len + size <= sizeof(b->wdata)) {
                for (unsigned i = 0; i < size; i++) {
                    b->wdata[b->wdata_len++] = v >> (8 * i);
                }
            }
            h2fmi_write_check(b);
            return;
        case FMI_META:
            if (b->wmeta_len + size <= sizeof(b->wmeta)) {
                for (unsigned i = 0; i < size; i++) {
                    b->wmeta[b->wmeta_len++] = v >> (8 * i);
                }
            }
            return;
        case FMI_LEVEL:
            return;
        default:
            b->fmi[off / 4] = v;
            break;
        }
        h2fmi_update_irq(b);
        return;
    }
    if (off >= FMC_BASE && off < FMC_BASE + sizeof(b->fmc)) {
        hwaddr r = off - FMC_BASE;
        switch (r) {
        case FMC_GO:
            b->fmc[r / 4] = v;
            if (v) {
                h2fmi_go(b, v);
            }
            return;
        case FMC_EVENTS:
            b->fmc[r / 4] &= ~v;
            break;
        default:
            b->fmc[r / 4] = v;
            break;
        }
        h2fmi_update_irq(b);
        return;
    }
    if (off == ECC_BASE + ECC_SUMMARY) {
        b->ecc_summary &= ~v;
        return;
    }
    if (off == ECC_BASE + 0x08 || off == ECC_BASE + ECC_SECTOR || off == ECC_BASE + 0x14) {
        return;         /* ECC configuration, per-sector ack, unknown: nothing to model */
    }
    /* the rest: nothing to model. */
    qemu_log_mask(LOG_UNIMP, "s5l8930.h2fmi%d: write 0x%" HWADDR_PRIx
                  " = 0x%x\n", b->n, off, v);
}

static const MemoryRegionOps h2fmi_ops = {
    .read = h2fmi_read,
    .write = h2fmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* How many bytes a CDMA channel may take from a FIFO window right now. */
static uint32_t h2fmi_fifo_avail(void *opaque, hwaddr addr, bool to_device)
{
    S5L8930H2FMIState *s = opaque;
    H2FMIBus *b = &s->bus[(addr - S5L8930_H2FMI_BASE) / H2FMI_WINDOW % H2FMI_BUSES];
    hwaddr reg = (addr - S5L8930_H2FMI_BASE) % H2FMI_WINDOW;

    if (to_device) {    /* room for a write */
        return reg == FMI_DATA ? sizeof(b->wdata) - b->wdata_len
             : reg == FMI_META ? sizeof(b->wmeta) - b->wmeta_len : ~0u;
    }
    return reg == FMI_DATA ? b->data_len : reg == FMI_META ? b->meta_len : ~0u;
}

static void s5l8930_h2fmi_reset(DeviceState *dev)
{
    S5L8930H2FMIState *s = S5L8930_H2FMI(dev);

    for (int i = 0; i < H2FMI_BUSES; i++) {
        H2FMIBus *b = &s->bus[i];
        memset(b->fmi, 0, sizeof(b->fmi));
        memset(b->fmc, 0, sizeof(b->fmc));
        b->ecc_summary = b->ecc_n = b->ecc_reads = 0;
        b->mode = MODE_NONE;
        b->data_len = b->meta_len = 0;
        b->pending_n = 0;
        qemu_set_irq(b->irq, 0);
    }
}

static void s5l8930_h2fmi_realize(DeviceState *dev, Error **errp)
{
    S5L8930H2FMIState *s = S5L8930_H2FMI(dev);

    if (s->cdma) {
        s5l8930_cdma_set_source(s->cdma, S5L8930_H2FMI_BASE,
                                H2FMI_BUSES * H2FMI_WINDOW, h2fmi_fifo_avail, s);
    }
}

static void s5l8930_h2fmi_init(Object *obj)
{
    S5L8930H2FMIState *s = S5L8930_H2FMI(obj);

    for (int i = 0; i < H2FMI_BUSES; i++) {
        H2FMIBus *b = &s->bus[i];
        g_autofree char *name = g_strdup_printf("s5l8930.h2fmi%d", i);

        b->s = s;
        b->n = i;
        memory_region_init_io(&b->iomem, obj, &h2fmi_ops, b, name,
                              H2FMI_WINDOW);
        /* A transfer kicks the CDMA, which drains the FIFO windows from inside
         * this region's write handler; QEMU's guard would drop those reads. */
        b->iomem.disable_reentrancy_guard = true;
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &b->iomem);
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &b->irq);
    }
}

static const Property s5l8930_h2fmi_props[] = {
    DEFINE_PROP_LINK("iop", S5L8930H2FMIState, iop, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_LINK("cdma", S5L8930H2FMIState, cdma, TYPE_DEVICE, DeviceState *),
};

static void s5l8930_h2fmi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8930_h2fmi_realize;
    device_class_set_legacy_reset(dc, s5l8930_h2fmi_reset);
    device_class_set_props(dc, s5l8930_h2fmi_props);
}

static const TypeInfo s5l8930_h2fmi_info = {
    .name = TYPE_S5L8930_H2FMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930H2FMIState),
    .instance_init = s5l8930_h2fmi_init,
    .class_init = s5l8930_h2fmi_class_init,
};

static void s5l8930_h2fmi_register_types(void)
{
    type_register_static(&s5l8930_h2fmi_info);
}

type_init(s5l8930_h2fmi_register_types)
