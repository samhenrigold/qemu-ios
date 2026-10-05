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
 *   FMC +0x18/+0x1C address bytes (page read/program includes two column
 *        bytes before row; erase0x60 sends row only). +0x20 count - 1.
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
#include "migration/vmstate.h"

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
#define FMC_BASE        (b->s->fmc_off)    /* "fmc-offset": 0x40000, the S5L8920's 0x400 */
#define FMC_CE          0x0C
#define FMC_GO          0x10
#define FMC_CMD         0x14
#define FMC_ADDR0       0x18
#define FMC_ADDR1       0x1C
#define FMC_EVTEN       0x40
#define FMC_EVENTS      0x44
#define FMC_NAND_STATUS 0x48
#define ECC_BASE        (b->s->ecc_off)    /* "ecc-offset": 0x80000, the S5L8920's 0x800 */
#define ECC_SECTOR      0x0C
#define ECC_SUMMARY     0x10

#define FMI_ST_DONE     (1u << 1)
#define FMI_ST_FMC      (1u << 8)
#define FMC_EV_STATUS   (1u << 5)
#define ECC_BLANK       (1u << 1)
#define NAND_FAIL       1u              /* chip status: program/erase failed */
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
    uint32_t mode;                      /* H2FMIMode */
    uint8_t id_pos;                     /* next READ ID byte for a byte-wide read (go 0x10) */
    uint32_t row;
    bool read_pending; /* new NAND read awaits its first FMI transfer */
    /* "explicit-start": CEs whose latched page no transfer has taken yet, and
     * whether a control 5 has started a write transfer not yet complete */
    uint8_t unread;
    bool write_armed;
    uint32_t read_format; /* last completed phase of the latched read */
    /* Each chip latches its own page at the read command: the IOP firmware
     * commands the next CE before it transfers the previous one. */
    bool page_ok[8];                    /* loaded page has data (else blank) */
    uint32_t page_row[8];
    uint8_t page[8][H2FMI_BUF];
    uint32_t stride;
    uint8_t data[H2FMI_QUEUE * H2FMI_BUF], meta[H2FMI_QUEUE * META_BYTES];
    uint32_t data_len, meta_len;        /* bytes waiting, from data_off / meta_off on */
    uint32_t data_off, meta_off;        /* what the CDMA has taken of the front: pops are O(1) */
    /* Program path (the IOP firmware): cmd 0x80 + address, the CDMA fills the
     * FIFOs, control 0x5 moves them to the chip, cmd 0x10 confirms. */
    bool writing;
    uint8_t wdata[H2FMI_BUF], wmeta[8 * 64];     /* meta for a burst of pages, as the data */
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
    /*
     * Where FMC and ECC sit in each interface's window. The S5L8922 IOP
     * firmware (N18) addresses them as the A4's does, at +0x40000/+0x80000;
     * the S5L8920's (N88, "s5l8920x" build of the same iBoot-931) at
     * +0x400/+0x800, inside the DT's 4 KiB window.
     */
    uint32_t fmc_off, ecc_off;
    uint32_t ecc_blank_summary;
    /*
     * The S5L8920's firmware starts every transfer with a control write of its
     * own and leaves control at 3 or 5 between them: a read of the last page
     * of a multi-page op follows a status poll, not a new read command, and a
     * write's FIFOs fill (control still 5 from the last page) before the next
     * page's chip select and 0x80. With "explicit-start", a control 3 takes
     * any CE whose page no transfer has taken yet, and only a control 5 write
     * arms a write transfer. Off: the A4 firmware's inferred starts.
     */
    bool explicit_start;
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
        b->id_pos = 0;
        break;
    case 0xff:
        b->fmc[FMC_NAND_STATUS / 4] = NAND_READY;
        b->mode = MODE_NONE;
        b->read_pending = false;
        b->read_format = 0;
        b->unread = 0;
        b->write_armed = false;
        b->data_len = b->meta_len = b->data_off = b->meta_off = 0;
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
        uint32_t result = 1;

        if (ce >= 0 && s->iop && b->wpage_len[ce & 7]) {
            s5l8930_iop_nand_info(s->iop, &id, &mask, &pb);
            int per_bus = MAX(ctpop8(mask), 1);
            int cs = (ce & 7) * H2FMI_BUSES + b->n;
            result = s5l8930_iop_nand_program(s->iop, cs / per_bus, cs % per_bus, b->row,
                                     b->wpage[ce & 7], b->wpage_len[ce & 7], b->wpmeta[ce & 7]);
            b->wpage_len[ce & 7] = 0;
        }
        b->writing = false;
        b->fmc[FMC_NAND_STATUS / 4] = NAND_READY | (result == 1 ? 0 : NAND_FAIL);
        break;
    }
    case 0x60:              /* block erase: address follows, 0xd0 confirms */
        break;
    case 0xd0: {
        int ce = h2fmi_ce(b);
        uint32_t id, pb;
        uint8_t mask;
        uint32_t result = 1;

        if (ce >= 0 && s->iop) {
            s5l8930_iop_nand_info(s->iop, &id, &mask, &pb);
            int per_bus = MAX(ctpop8(mask), 1);
            int cs = (ce & 7) * H2FMI_BUSES + b->n;
            result = s5l8930_iop_nand_erase(s->iop, cs / per_bus, cs % per_bus, b->row);
        }
        b->fmc[FMC_NAND_STATUS / 4] = NAND_READY | (result == 1 ? 0 : NAND_FAIL);
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
        b->read_pending = true;
        if (ce >= 0) {
            b->unread |= 1u << (ce & 7);
        }
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
        if ((cmds & 0xff) == 0x60) {
            /* NAND block erase has no column address. The three row bytes
             * start at ADDR0 byte0; ADDR1 can still contain a previous read. */
            if (getenv("NAND_TRACE")) {
                fprintf(stderr, "NAND erase address a0=%x a1=%x count=%x\n",
                        b->fmc[FMC_ADDR0 / 4], b->fmc[FMC_ADDR1 / 4], b->fmc[0x20 / 4]);
            }
            b->row = b->fmc[FMC_ADDR0 / 4] & 0xffffff;
        } else {
            b->row = (b->fmc[FMC_ADDR0 / 4] >> 16) |
                     ((b->fmc[FMC_ADDR1 / 4] & 0xff) << 16);
        }
    }
    if (go & 2) {
        h2fmi_command(b, (cmds >> 8) & 0xff);
    }
    if ((go & 0x10) && b->mode == MODE_ID) {
        /*
         * A byte-wide read off the bus into +0x48: after READ ID, the next ID
         * byte. The S5L8920's IOP firmware reads the ID this way (go 0x9,
         * then 0x50 per byte) instead of through the FIFO. ponytail: id_pos
         * is not migrated; a snapshot mid-READ ID re-reads from byte 0.
         */
        uint32_t id = 0, pb;
        uint8_t mask = 0;
        int ce = h2fmi_ce(b);

        if (b->s->iop) {
            s5l8930_iop_nand_info(b->s->iop, &id, &mask, &pb);
        }
        b->fmc[FMC_NAND_STATUS / 4] = (ce >= 0 && (mask & (1u << ce)) && b->id_pos < 4) ?
                                      (id >> (8 * b->id_pos)) & 0xff : 0;
        b->id_pos++;
        h2fmi_fmc_events(b, (go & 0xb) | FMC_EV_STATUS);
        return;
    }
    if (go & 0x40) {
        /* Ready is immediate, but polling must preserve the last failure. */
        b->fmc[FMC_NAND_STATUS / 4] |= NAND_READY;
        h2fmi_fmc_events(b, (go & 0xb) | FMC_EV_STATUS);
        return;
    }
    h2fmi_fmc_events(b, go & 0xb);
}

/* Control 3: the selected page (or the ID) goes into the FIFOs. */
/* Move what is left of a FIFO to its start when the next push would run off the end. */
static void fifo_compact(uint8_t *buf, uint32_t *off, uint32_t len, uint32_t size, uint32_t need)
{
    if (*off && *off + len + need > size) {
        memmove(buf, buf + *off, len);
        *off = 0;
    }
}

/* With no ECC metadata extraction, the page register is a single raw
 * byte stream: data followed by physical spare. The stock IOP first drains
 * the data, changes FORMAT from 0x40004 to 0x8001, then drains the spare. */
static bool h2fmi_raw_read(H2FMIBus *b)
{
    uint32_t format = b->fmi[FMI_FORMAT / 4];
    return (format & 0xff) && !((format >> 19) & 0x3f);
}

static uint32_t h2fmi_read_bytes(H2FMIBus *b, uint32_t page_bytes)
{
    return h2fmi_raw_read(b) && b->stride >= page_bytes ? b->stride : page_bytes;
}

static bool h2fmi_room(H2FMIBus *b, uint32_t page_bytes)
{
    fifo_compact(b->data, &b->data_off, b->data_len, sizeof(b->data), page_bytes);
    fifo_compact(b->meta, &b->meta_off, b->meta_len, sizeof(b->meta), META_BYTES);
    return b->data_off + b->data_len + page_bytes <= sizeof(b->data) &&
           b->meta_off + b->meta_len + META_BYTES <= sizeof(b->meta) && b->ecc_n < ARRAY_SIZE(b->ecc_q);
}

/* queued: the oldest waiting transfer, popped by h2fmi_drain; it goes now. */
static void h2fmi_transfer(H2FMIBus *b, int ce, bool queued)
{
    S5L8930H2FMIState *s = b->s;
    uint32_t page_bytes = 0, id = 0;
    uint8_t ce_mask = 0;
    uint32_t transfer_bytes;
    bool raw = h2fmi_raw_read(b);

    b->read_pending = false;
    b->read_format = b->fmi[FMI_FORMAT / 4];
    if (ce >= 0) {
        b->unread &= ~(1u << (ce & 7));
    }
    if (s->iop) {
        s5l8930_iop_nand_info(s->iop, &id, &ce_mask, &page_bytes);
    }
    transfer_bytes = h2fmi_read_bytes(b, page_bytes);
    if (b->mode == MODE_ID) {
        uint8_t idb[8] = { 0 };
        if (ce >= 0 && (ce_mask & (1u << ce))) {
            stl_le_p(idb, id);
        }
        memcpy(b->data, idb, sizeof(idb));
        b->data_len = sizeof(idb);
        b->data_off = 0;
    } else if (b->mode == MODE_PAGE && page_bytes && !queued &&
               (b->pending_n || !h2fmi_room(b, transfer_bytes))) {
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
        uint32_t meta = raw ? 0 : MIN(META_BYTES,
                            sizeof(b->meta) - b->meta_off - b->meta_len);
        uint8_t *m = b->meta + b->meta_off + b->meta_len;

        bool ok = ce >= 0 && b->page_ok[ce & 7];
        const uint8_t *pg = b->page[ce & 7];

        if (ok) {
            memcpy(b->data + b->data_off + b->data_len, pg, transfer_bytes);
            memset(m, 0, meta);
            memcpy(m, pg + page_bytes, MIN(meta, META_BYTES));
        } else {
            memset(b->data + b->data_off + b->data_len, 0xff, transfer_bytes);
            memset(m, 0xff, meta);
        }
        b->data_len += transfer_bytes;
        b->meta_len += meta;
        b->ecc_q[b->ecc_n++] = raw || ok ? 0 : ECC_BLANK;
        /* The S5L8920's firmware takes a blank page from the summary alone
         * (fw 0x3cdc: bit 6, then bit 3 uncorrectable), never reading the
         * per-sector words: "ecc-blank-summary" (0x40 there, 0 on the A4). */
        b->ecc_summary = raw || ok ? 0 : s->ecc_blank_summary;
    }
    b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
    h2fmi_update_irq(b);
    if (s->cdma) {
        s5l8930_cdma_kick(s->cdma);
    }
}

/* Meta bytes per page as the firmware formats it (FMI +0x34 bits 19-24), 10 on every K48 build. */
static uint32_t h2fmi_meta_per_page(H2FMIBus *b)
{
    uint32_t m = (b->fmi[FMI_FORMAT / 4] >> 19) & 0x3f;

    return m ? m : META_BYTES;
}

/*
 * A write transfer (control 5) completes when the FIFOs hold the page: its data and its meta, which
 * the firmware streams on two CDMA chains (a multi-page write's meta chain runs ahead of or behind the
 * data one). A to-device chain is done when the FIFO it fills has been drained by the chip, each FIFO
 * for its own chain (EmbeddedIOP h2fmi_write_multi waits for both: "Timeout waiting for CDMA during
 * successful NAND write operation").
 */
static void h2fmi_write_check(H2FMIBus *b)
{
    uint32_t page_bytes = 0, id = 0, mper = h2fmi_meta_per_page(b);
    uint8_t mask;
    hwaddr base = S5L8930_H2FMI_BASE + b->n * H2FMI_WINDOW;

    if ((b->fmi[FMI_CONTROL / 4] & 7) != 5 || (b->s->explicit_start && !b->write_armed)) {
        return;
    }
    if (b->s->iop) {
        s5l8930_iop_nand_info(b->s->iop, &id, &mask, &page_bytes);
    }
    if (page_bytes && b->wdata_len >= page_bytes && b->wmeta_len >= mper &&
        !(b->fmi[FMI_STATUS / 4] & FMI_ST_DONE)) {
        int ce = h2fmi_ce(b);

        if (ce >= 0) {
            memcpy(b->wpage[ce & 7], b->wdata, page_bytes);
            memset(b->wpmeta[ce & 7], 0, META_BYTES);
            memcpy(b->wpmeta[ce & 7], b->wmeta, MIN(mper, META_BYTES));
            b->wpage_len[ce & 7] = page_bytes;
        }
        memmove(b->wdata, b->wdata + page_bytes, b->wdata_len - page_bytes);
        b->wdata_len -= page_bytes;
        memmove(b->wmeta, b->wmeta + mper, b->wmeta_len - mper);
        b->wmeta_len -= mper;
        b->write_armed = false;
        b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
        h2fmi_update_irq(b);
        if (b->s->cdma) {
            if (!b->wdata_len) {
                s5l8930_cdma_sink_done(b->s->cdma, base + FMI_DATA, 4);
            }
            if (!b->wmeta_len) {
                s5l8930_cdma_sink_done(b->s->cdma, base + FMI_META, 4);
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
    while (b->pending_n && h2fmi_room(b, h2fmi_read_bytes(b, page_bytes))) {
        int ce = b->pending_ce[0];

        memmove(b->pending_ce, b->pending_ce + 1, --b->pending_n);
        h2fmi_transfer(b, ce, true);
    }
}

static uint32_t fifo_pop(uint8_t *buf, uint32_t *off, uint32_t *len, unsigned size)
{
    uint32_t v = 0;
    unsigned n = MIN(size, *len);

    for (unsigned i = 0; i < n; i++) {
        v |= buf[*off + i] << (8 * i);
    }
    *off += n;
    *len -= n;
    if (!*len) {
        *off = 0;
    }
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
            uint32_t v = fifo_pop(b->data, &b->data_off, &b->data_len, size);
            h2fmi_drain(b);
            return v;
        }
        case FMI_META: {
            uint32_t v = fifo_pop(b->meta, &b->meta_off, &b->meta_len, size);
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
             * become 3), follows a new NAND read command (raw reads retain
             * control 3), or, in read mode, raises bit 7 (the next page of a
             * pipelined read). Rewriting the mode is not a new page: both
             * drivers clear bit 7 with a read-modify-write once a page's ECC
             * is read (iBoot-817 0x5ff04066: 3 -> 3; the IOP firmware:
             * 0x83 -> 3). iBoot pipelines with 0x82 then 3 (0x5ff0422e), the
             * IOP firmware with 3 then 0x83.
             */
            if ((v & 7) == 5) {
                /* Write transfer: done once the FIFOs hold the page. */
                b->fmi[off / 4] = v;
                b->write_armed = true;
                h2fmi_write_check(b);
                return;
            }
            uint32_t prev = b->fmi[off / 4];
            if ((v & 7) == 0) {
                b->read_pending = false;
                b->read_format = 0;
            }
            int ce = h2fmi_ce(b);
            bool unread = b->s->explicit_start && ce >= 0 && (b->unread >> (ce & 7)) & 1;
            if ((v & 3) == 3 && (b->read_pending || unread || (prev & 3) != 3 ||
                                 ((v & 0x80) && !(prev & 0x80)))) {
                if (!(b->fmi[off / 4] & 0x80)) {
                    b->ecc_n = b->ecc_reads = 0;
                }
                h2fmi_transfer(b, h2fmi_ce(b), false);
            } else if ((v & 3) == 3 && b->mode == MODE_PAGE &&
                       h2fmi_raw_read(b) &&
                       b->read_format != b->fmi[FMI_FORMAT / 4]) {
                /* Raw physical reads have separate data and spare phases.
                 * FORMAT selects a new phase within the existing byte stream;
                 * each phase completes independently and has a W1C DONE.
                 * Do not reload the page or require bytes still in the FIFO:
                 * a prepared CDMA chain may already have drained them. */
                b->read_format = b->fmi[FMI_FORMAT / 4];
                b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
                h2fmi_update_irq(b);
                if (b->s->cdma) {
                    s5l8930_cdma_kick(b->s->cdma);
                }
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
            h2fmi_write_check(b);
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
        b->read_pending = false;
        b->read_format = 0;
        b->unread = 0;
        b->write_armed = false;
        b->data_len = b->meta_len = b->data_off = b->meta_off = 0;
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

/* The IOP firmware leaves the controller mid-operation between any two of its instructions:
 * latched pages, the FIFOs, the queue, the program registers all move with a snapshot. */
/* These FIFOs are fixed arrays, not pointers. VBUFFER_UINT32 dereferences
 * their first bytes as a host address; additionally bound the incoming length
 * before upstream buffer I/O, rather than checking after it overwrote memory. */
static int h2fmi_fifo_get(QEMUFile *f, void *pv, size_t size,
                         const VMStateField *field)
{
    return size > field->num ? -EINVAL : vmstate_info_buffer.get(f, pv, size, field);
}

static int h2fmi_fifo_put(QEMUFile *f, void *pv, size_t size,
                         const VMStateField *field, JSONWriter *vmdesc)
{
    return size > field->num ? -EINVAL : vmstate_info_buffer.put(f, pv, size, field, vmdesc);
}

static const VMStateInfo h2fmi_fifo_info = {
    .name = "h2fmi-bounded-fifo",
    .get = h2fmi_fifo_get,
    .put = h2fmi_fifo_put,
};

#define H2FMI_FIFO(_field, _length) { \
    .name = #_field, \
    .size_offset = vmstate_offset_value(H2FMIBus, _length, uint32_t), \
    .info = &h2fmi_fifo_info, \
    .num = sizeof(((H2FMIBus *)0)->_field), \
    .flags = VMS_VBUFFER, \
    .offset = offsetof(H2FMIBus, _field), \
}

static const VMStateDescription vmstate_h2fmi_bus = {
    .name = "s5l8930.h2fmi-bus",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(fmi, H2FMIBus, 0x40 / 4),
        VMSTATE_UINT32_ARRAY(fmc, H2FMIBus, 0x50 / 4),
        VMSTATE_UINT32(ecc_summary, H2FMIBus),
        VMSTATE_UINT32_ARRAY(ecc_q, H2FMIBus, H2FMI_QUEUE),
        VMSTATE_UINT32(ecc_n, H2FMIBus),
        VMSTATE_UINT32(ecc_reads, H2FMIBus),
        VMSTATE_UINT32(mode, H2FMIBus),
        VMSTATE_UINT32(row, H2FMIBus),
        VMSTATE_BOOL_ARRAY(page_ok, H2FMIBus, 8),
        VMSTATE_UINT32_ARRAY(page_row, H2FMIBus, 8),
        VMSTATE_BUFFER_UNSAFE(page, H2FMIBus, 0, sizeof(((H2FMIBus *)0)->page)),
        VMSTATE_UINT32(stride, H2FMIBus),
        VMSTATE_UINT32(data_len, H2FMIBus),
        H2FMI_FIFO(data, data_len),
        VMSTATE_UINT32(meta_len, H2FMIBus),
        H2FMI_FIFO(meta, meta_len),
        VMSTATE_BOOL(writing, H2FMIBus),
        VMSTATE_UINT32(wdata_len, H2FMIBus),
        H2FMI_FIFO(wdata, wdata_len),
        VMSTATE_UINT32(wmeta_len, H2FMIBus),
        H2FMI_FIFO(wmeta, wmeta_len),
        VMSTATE_BUFFER_UNSAFE(wpage, H2FMIBus, 0, sizeof(((H2FMIBus *)0)->wpage)),
        VMSTATE_BUFFER_UNSAFE(wpmeta, H2FMIBus, 0, sizeof(((H2FMIBus *)0)->wpmeta)),
        VMSTATE_UINT32_ARRAY(wpage_len, H2FMIBus, 8),
        VMSTATE_UINT32(erase_row, H2FMIBus),
        VMSTATE_BUFFER_UNSAFE(pending_ce, H2FMIBus, 0, sizeof(((H2FMIBus *)0)->pending_ce)),
        VMSTATE_UINT32(pending_n, H2FMIBus),
        VMSTATE_END_OF_LIST()
    },
};

static int h2fmi_pre_save(void *opaque)
{
    S5L8930H2FMIState *s = opaque;

    for (int i = 0; i < H2FMI_BUSES; i++) {     /* the stream carries the FIFOs from offset 0 */
        H2FMIBus *b = &s->bus[i];
        fifo_compact(b->data, &b->data_off, b->data_len, 0, 0);
        fifo_compact(b->meta, &b->meta_off, b->meta_len, 0, 0);
    }
    return 0;
}

static int h2fmi_pre_load(void *opaque)
{
    S5L8930H2FMIState *s = opaque;

    for (int i = 0; i < H2FMI_BUSES; i++) {     /* lengths are validated against the buffers */
        s->bus[i].data_len = s->bus[i].meta_len = s->bus[i].wdata_len = s->bus[i].wmeta_len = 0;
        s->bus[i].data_off = s->bus[i].meta_off = 0;
        s->bus[i].read_pending = false; /* v1 streams had no pending-read state */
        s->bus[i].read_format = UINT32_MAX; /* absent phase subsection */
    }
    return 0;
}

static int h2fmi_post_load(void *opaque, int version_id)
{
    S5L8930H2FMIState *s = opaque;

    for (int i = 0; i < H2FMI_BUSES; i++) {
        H2FMIBus *b = &s->bus[i];
        if (b->read_format == UINT32_MAX) {
            b->read_format = b->fmi[FMI_FORMAT / 4];
        }
        if (b->data_len > sizeof(b->data) || b->meta_len > sizeof(b->meta) ||
            b->wdata_len > sizeof(b->wdata) || b->wmeta_len > sizeof(b->wmeta) ||
            b->ecc_n > H2FMI_QUEUE || b->pending_n > ARRAY_SIZE(b->pending_ce)) {
            return -EINVAL;
        }
    }
    return 0;
}

/* Optional subsection preserves the read latch and completed phase without
 * changing the original v1 FIFO stream. Version 1 has only the pending latch;
 * older snapshots default their completed phase to the restored FORMAT. */
static bool h2fmi_read_pending_needed(void *opaque)
{
    S5L8930H2FMIState *s = opaque;
    return s->bus[0].read_pending || s->bus[1].read_pending ||
           s->bus[0].read_format || s->bus[1].read_format;
}

static const VMStateDescription vmstate_h2fmi_read_pending = {
    .name = "s5l8930.h2fmi/read-pending",
    .version_id = 2,
    .minimum_version_id = 1,
    .needed = h2fmi_read_pending_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(bus[0].read_pending, S5L8930H2FMIState),
        VMSTATE_BOOL(bus[1].read_pending, S5L8930H2FMIState),
        VMSTATE_UINT32_V(bus[0].read_format, S5L8930H2FMIState, 2),
        VMSTATE_UINT32_V(bus[1].read_format, S5L8930H2FMIState, 2),
        VMSTATE_END_OF_LIST()
    },
};

/* The explicit-start latches: only boards with that property have them. */
static bool h2fmi_explicit_start_needed(void *opaque)
{
    return ((S5L8930H2FMIState *)opaque)->explicit_start;
}

static const VMStateDescription vmstate_h2fmi_explicit_start = {
    .name = "s5l8930.h2fmi/explicit-start",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = h2fmi_explicit_start_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(bus[0].unread, S5L8930H2FMIState),
        VMSTATE_UINT8(bus[1].unread, S5L8930H2FMIState),
        VMSTATE_BOOL(bus[0].write_armed, S5L8930H2FMIState),
        VMSTATE_BOOL(bus[1].write_armed, S5L8930H2FMIState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_s5l8930_h2fmi = {
    .name = TYPE_S5L8930_H2FMI,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = h2fmi_pre_save,
    .pre_load = h2fmi_pre_load,
    .post_load = h2fmi_post_load,
    .subsections = (const VMStateDescription * const []) {
        &vmstate_h2fmi_read_pending,
        &vmstate_h2fmi_explicit_start,
        NULL
    },
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(bus, S5L8930H2FMIState, H2FMI_BUSES, 1, vmstate_h2fmi_bus, H2FMIBus),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s5l8930_h2fmi_props[] = {
    DEFINE_PROP_LINK("iop", S5L8930H2FMIState, iop, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_LINK("cdma", S5L8930H2FMIState, cdma, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT32("fmc-offset", S5L8930H2FMIState, fmc_off, 0x40000),
    DEFINE_PROP_UINT32("ecc-offset", S5L8930H2FMIState, ecc_off, 0x80000),
    DEFINE_PROP_UINT32("ecc-blank-summary", S5L8930H2FMIState, ecc_blank_summary, 0),
    DEFINE_PROP_BOOL("explicit-start", S5L8930H2FMIState, explicit_start, false),
};

static void s5l8930_h2fmi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8930_h2fmi_realize;
    dc->vmsd = &vmstate_s5l8930_h2fmi;
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
