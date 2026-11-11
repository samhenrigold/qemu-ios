/*
 * S5L8930 ("A4") H2FMI: the two NAND flash interfaces as iBoot drives them
 * directly. The kernel never touches these registers (its NAND goes through
 * the IOP firmware, s5l8930_iop.c); iBoot-817.29's drivers/apple/h2fmi does,
 * and this model answers exactly what that driver uses. Pages come from the
 * IOP's page store, so iBoot and the kernel see the same flash.
 *
 * The optional PPN mode adds A5/A6 package commands and the 32-bit FMISS
 * sequencer used by the original N42 IOP firmware. Its bring-up remains
 * incomplete; unsupported instructions stall and are logged.
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
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "hw/trace-printf.h"

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

typedef enum { MODE_NONE, MODE_ID, MODE_PAGE, MODE_PPN_FEATURE_WRITE, MODE_PPN_AUX_READ } H2FMIMode;

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
    uint32_t ppn_features[0x10000];
    uint8_t ppn_opcode;
    uint8_t ppn_status;
    uint32_t ppn_aux_off;
    uint32_t ppn_column;
    /* PPN 07/0A reads are pipelined. Status and DATA_OUT consume the oldest
     * request, even after subsequent commands have addressed another page. */
    uint32_t ppn_read_row[8 * 128], ppn_read_column[8 * 128];
    uint32_t ppn_read_status[8 * 128], ppn_read_count[8];
    unsigned ppn_queries;
    /* A5/A6 flash microsequencer: uploaded program RAM, instruction and
     * operand FIFOs, and the MMIO-read result FIFO. Decoded from the original
     * N42 IOP firmware; unsupported instructions stop rather than complete. */
    uint32_t seq_ram[256], seq_code[1024], seq_data[32], seq_result[32];
    uint32_t seq_code_n, seq_data_n, seq_result_n;
    uint32_t seq_regs[0x40 / 4], seq_page_off;
    bool seq_running;
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
    uint8_t data[H2FMI_QUEUE * H2FMI_BUF], meta[H2FMI_QUEUE * 64];
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
    uint64_t mmio_base;
    bool ppn;
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
    bool cfg_v0;                        /* "cfg-v0": see h2fmi_format */
    H2FMIBus bus[H2FMI_BUSES];
};

#define HT(...) do { if (trace_event_get_state_backends(TRACE_S5L8930_H2FMI_LOG)) { \
                     TRACE_PRINTF(trace_s5l8930_h2fmi_log, __VA_ARGS__); } } while (0)

/*
 * The page format in the A4 layout (FMI +0x34: bits 0-7 sectors, 19-24 meta
 * bytes). "cfg-v0": the s5l8920x firmware has no +0x34; its +0x00 config
 * carries the meta bytes in bits 8-12 (0x60a29 for a page with 10) and none
 * on a raw read (0xaa083 the data phase, 0xa8003 the spare phase), so it maps
 * to one sector with that meta, or for a raw read to the config's low half (a
 * nonzero sector byte, no meta, each phase a different value).
 */
static uint32_t h2fmi_format(H2FMIBus *b)
{
    uint32_t cfg = b->fmi[0];
    uint32_t meta = (cfg >> 8) & 0x1f;

    if (!b->s->cfg_v0) {
        return b->fmi[FMI_FORMAT / 4];
    }
    if (!cfg) {
        return 0;
    }
    return meta ? (meta << 19) | 1 : (cfg & 0xffff) | 1;
}

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

    /*
     * Any other command ends READ ID: the byte reads that follow are the
     * status or data again. Left in ID mode, a status poll (go 0x50) read an
     * ID byte. A cold boot never showed it (a page read follows the ID reads
     * on every bus); after a hibernate wake the IOP firmware's first op on
     * FMI1 is a program, and its status check found bit 7 (not write-
     * protected) clear: "WMR_ASSERT failed:(bWPDisabled)" in
     * h2fmi_get_current_writable_CE_state (n88 8C148a IOP 0x4dec).
     */
    if (cmd != 0x90 && b->mode == MODE_ID) {
        b->mode = MODE_NONE;
    }
    switch (cmd) {
    case 0x0a:
    case 0x07:
        if (s->ppn) { b->ppn_opcode = cmd; break; }
        qemu_log_mask(LOG_UNIMP, "h2fmi: PPN READ on non-PPN part\n");
        break;
    case 0xee:
    case 0x92:
        if (s->ppn) {
            b->ppn_opcode = cmd;
            b->ppn_status = 0x40;
            b->ppn_aux_off = 0;
            b->mode = MODE_PPN_AUX_READ;
            break;
        }
        qemu_log_mask(LOG_UNIMP, "h2fmi: PPN query on non-PPN part\n");
        break;
    case 0x7a:
        if (s->ppn) {
            int ce = h2fmi_ce(b);
            if (s->iop && ce >= 0 && b->ppn_read_count[ce & 7]) {
                unsigned slot = (ce & 7) * 128;
                uint32_t id, pb; uint8_t mask;
                s5l8930_iop_nand_info(s->iop, &id, &mask, &pb);
                int per_bus = MAX(ctpop8(mask), 1);
                int cs = (ce & 7) * H2FMI_BUSES + b->n;
                b->page_row[ce & 7] = b->ppn_read_row[slot];
                b->ppn_column = b->ppn_read_column[slot];
                b->page_ok[ce & 7] = s5l8930_iop_nand_read(s->iop,
                    cs / per_bus, cs % per_bus, b->page_row[ce & 7],
                    b->page[ce & 7], &b->stride);
                unsigned n = --b->ppn_read_count[ce & 7];
                memmove(b->ppn_read_row + slot, b->ppn_read_row + slot + 1, n * 4);
                memmove(b->ppn_read_column + slot, b->ppn_read_column + slot + 1, n * 4);
                memmove(b->ppn_read_status + slot, b->ppn_read_status + slot + 1, n * 4);
            }
            b->read_pending = true;
            break;
        }
        qemu_log_mask(LOG_UNIMP, "h2fmi: PPN DATA_OUT on non-PPN part\n");
        break;
    case 0xb8:
    case 0x79:
    case 0x97:
    case 0x77:
    case 0x7d:
    case 0xe7:
        if (s->ppn) {
            int ce = h2fmi_ce(b);
            if (cmd == 0x7d && ce >= 0 && b->ppn_read_count[ce & 7])
                b->ppn_status = b->ppn_read_status[(ce & 7) * 128];
            b->fmc[FMC_NAND_STATUS / 4] = b->ppn_status;
            break;
        }
        qemu_log_mask(LOG_UNIMP, "h2fmi: PPN operation on non-PPN part\n");
        break;
    case 0xb0: /* PPN sequenced management-data input; B8 commits it. */
        if (!s->ppn) break;
        b->seq_page_off = 0;
        b->ppn_status = 0x40;
        b->mode = MODE_PPN_FEATURE_WRITE;
        /* CDMA may prefill this command's bytes before the sequencer issues it. */
        break;
    case 0xef:
        if (s->ppn) {
            b->ppn_status = 0x40;
            b->mode = MODE_PPN_FEATURE_WRITE;
            b->wdata_len = b->wmeta_len = 0;
            break;
        }
        qemu_log_mask(LOG_UNIMP, "h2fmi: SET_FEATURE on non-PPN part\n");
        break;
    case 0x90:
        b->mode = MODE_ID;
        b->id_pos = 0;
        /* The ID goes through the FIFO on the next control 3 even if control is already 3: 3.1.3's
         * S5L8920 IOP firmware reads each CE's ID in turn without clearing control between them. */
        b->read_pending = true;
        break;
    case 0xff:
        memset(b->ppn_read_count, 0, sizeof(b->ppn_read_count));
        b->ppn_status = 0x40;
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
    case 0x87:              /* PPN queued page-data input. */
    case 0x8a:              /* PPN boot/SLC page program. */
        if (!s->ppn) break;
        b->seq_page_off = 0;
        b->mode = MODE_PAGE;
        if (h2fmi_ce(b) >= 0) b->wpage_len[h2fmi_ce(b) & 7] = 0;
        /* fall through */
    case 0x80:              /* page program: address follows, data through the FIFOs */
    case 0x81:              /* the next page of a cache program */
        b->writing = true;
        break;
    case 0x17:              /* PPN boot/SLC program confirm. */
        if (!s->ppn) break;
        /* fall through */
    case 0x10:              /* program: the chip's page register goes to the flash */
    case 0x11: {            /* cache program: the same, the next page follows */
        int ce = h2fmi_ce(b);
        uint32_t id, pb;
        uint8_t mask;
        uint32_t result = 1;

        HT("h%d program ce %d row 0x%x len %u lpn 0x%x data %08x\n", b->n, ce, b->row,
           ce >= 0 ? b->wpage_len[ce & 7] : 0, ce >= 0 ? ldl_le_p(b->wpmeta[ce & 7]) : 0,
           ce >= 0 ? ldl_le_p(b->wpage[ce & 7]) : 0);
        if (ce >= 0 && s->iop && b->wpage_len[ce & 7]) {
            s5l8930_iop_nand_info(s->iop, &id, &mask, &pb);
            int per_bus = MAX(ctpop8(mask), 1);
            int cs = (ce & 7) * H2FMI_BUSES + b->n;
            result = s5l8930_iop_nand_program(s->iop, cs / per_bus, cs % per_bus, b->row,
                                     b->wpage[ce & 7], b->wpage_len[ce & 7],
                                     s->ppn ? NULL : b->wpmeta[ce & 7]);
            b->wpage_len[ce & 7] = 0;
        }
        b->writing = false;
        b->fmc[FMC_NAND_STATUS / 4] = (s->ppn ? 0x40 : NAND_READY) | (result == 1 ? 0 : NAND_FAIL);
        if (s->ppn) b->ppn_status = b->fmc[FMC_NAND_STATUS / 4];
        break;
    }
    case 0x67:              /* PPN normal erase, address completes it. */
        break;
    case 0x6a:              /* PPN SLC erase setup. */
        if (!s->ppn) break;
        /* fall through */
    case 0x60:              /* block erase: address follows, 0xd0 confirms */
        break;
    case 0xd7:              /* PPN SLC erase confirm. */
        if (!s->ppn) break;
        /* fall through */
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
        b->fmc[FMC_NAND_STATUS / 4] = (s->ppn ? 0x40 : NAND_READY) | (result == 1 ? 0 : NAND_FAIL);
        if (s->ppn) b->ppn_status = b->fmc[FMC_NAND_STATUS / 4];
        break;
    }
    case 0x37:
        if (!s->ppn) break;
        /* PPN confirms the addressed page with 0x37. */
        /* fall through */
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
        if (s->ppn) {
            b->ppn_status = ce >= 0 && b->page_ok[ce & 7] ? 0x40 : 0x49;
            b->fmc[FMC_NAND_STATUS / 4] = b->ppn_status;
            if (b->seq_running && ce >= 0) {
                unsigned c = ce & 7, n = b->ppn_read_count[c];
                if (n < 128) {
                    unsigned slot = c * 128 + n;
                    b->ppn_read_row[slot] = b->row;
                    b->ppn_read_column[slot] = b->ppn_column;
                    b->ppn_read_status[slot] = b->ppn_status;
                    b->ppn_read_count[c]++;
                } else qemu_log_mask(LOG_GUEST_ERROR, "PPN read queue overflow\n");
            }
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

    HT("h%d go 0x%x cmds 0x%x a0 0x%x a1 0x%x ce 0x%x dl %u ml %u count%u\n", b->n, go, cmds, b->fmc[FMC_ADDR0 / 4],
       b->fmc[FMC_ADDR1 / 4], b->fmc[FMC_CE / 4], b->data_len, b->meta_len, b->fmc[0x20 / 4]);

    if (go & 1) {
        h2fmi_command(b, cmds & 0xff);
    }
    if (go & 8) {
        if ((cmds & 0xff) == 0x60) {
            /* NAND block erase has no column address. The three row bytes
             * start at ADDR0 byte0; ADDR1 can still contain a previous read. */
            if (trace_event_get_state_backends(TRACE_S5L8930_NAND_LOG)) {
                TRACE_PRINTF(trace_s5l8930_nand_log, "NAND erase address a0=%x a1=%x count=%x\n",
                        b->fmc[FMC_ADDR0 / 4], b->fmc[FMC_ADDR1 / 4], b->fmc[0x20 / 4]);
            }
            b->row = b->fmc[FMC_ADDR0 / 4] & 0xffffff;
        } else if (b->s->ppn && ((cmds & 0xff) == 0x0a || (cmds & 0xff) == 0x07 ||
                                  (cmds & 0xff) == 0x8a || (cmds & 0xff) == 0x87 || (cmds & 0xff) == 0x6a || (cmds & 0xff) == 0x67)) {
            /* Multi-read's eight address bytes include its status/column
             * fields first; the packed PPN row is the second word. */
            /* bounded: the index selects only FMC_ADDR0 or FMC_ADDR1. */
            uint32_t row = b->fmc[((b->fmc[0x20 / 4] == 7) ? FMC_ADDR1 : FMC_ADDR0) / 4] & 0x7fffff;
            uint32_t page = row & 0xff;
            uint32_t block = (row >> 8) & 0x7ff;
            uint32_t cau = (row >> 19) & 0xf;
            b->row = (cau * 1064 + block) * 256 + page;
            if ((cmds & 0xff) == 0x0a || (cmds & 0xff) == 0x07)
                b->ppn_column = b->fmc[0x20 / 4] == 7 ? b->fmc[FMC_ADDR0 / 4] >> 16 : 0;
            if ((cmds & 0xff) == 0x67) h2fmi_command(b, 0xd7);
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
        b->fmc[FMC_NAND_STATUS / 4] |= b->s->ppn ? 0x40 : NAND_READY;
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
    uint32_t format = h2fmi_format(b);
    return (format & 0xff) && !((format >> 19) & 0x3f);
}

static uint32_t h2fmi_read_bytes(H2FMIBus *b, uint32_t page_bytes)
{
    /* cfg-v0: a raw read's spare goes through the meta FIFO (below), the data FIFO has the page alone */
    return h2fmi_raw_read(b) && !b->s->cfg_v0 && b->stride >= page_bytes ? b->stride : page_bytes;
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
    b->read_format = h2fmi_format(b);
    if (ce >= 0) {
        b->unread &= ~(1u << (ce & 7));
    }
    if (s->iop) {
        s5l8930_iop_nand_info(s->iop, &id, &ce_mask, &page_bytes);
    }
    transfer_bytes = h2fmi_read_bytes(b, page_bytes);
    if (s->ppn && b->mode == MODE_PPN_AUX_READ) {
        uint8_t payload[512] = {0};
        uint32_t bytes = MIN((h2fmi_format(b) >> 8) & 0xffff, sizeof(payload));
        if (b->ppn_opcode == 0x92) {
            memcpy(payload, "PPN Device Info", 15);
            const uint32_t info[] = {2, 4, 1064, 11, 256, 128, 8, 1, 2, 16448};
            for (unsigned i = 0; i < ARRAY_SIZE(info); i++) stl_le_p(payload + 0x10 + 4 * i, info[i]);
            const uint8_t timings[] = {50, 20, 10, 15, 60, 15, 25, 0, 50, 15, 25};
            memcpy(payload + 0xa0, timings, sizeof(timings));
            stl_le_p(payload + 0xe0, 8);
            stl_le_p(payload + 0xe4, 8);
            stl_le_p(payload + 0xe8, 8);
            /* Preparation entries, not a timing value. The original IOP
             * bounds a write batch against this field (11A465 offset
             * 9a80..9a8a); the A6 handoff permits 128 physical pages per
             * transaction. Advertise that supported virtual capacity,
             * rather than the initial eight-entry bring-up placeholder.
             * The matching physical Hynix part's value is unqualified. */
            stl_le_p(payload + 0xec, 128);
            stl_le_p(payload + 0xf0, 5);
            stl_le_p(payload + 0xf4, 5);
            stl_le_p(payload + 0xf8, 5);
            stl_le_p(payload + 0xfc, 100);
        } else {
            uint32_t feature = b->fmc[FMC_ADDR0 / 4] & 0xffff;
            if (b->ppn_queries++ < 64) qemu_log_mask(LOG_UNIMP,
                "PPN bus%d GET_FEATURE 0x%04x bytes %u\n", b->n, feature, bytes);
            if (feature == 0x9080) memcpy(payload, "040472P_HYNIX_B_", 15);
            else if ((feature & 0xff) == 0x84) {
                /* Factory-good block bitmap, one bit per block in this CAU. */
                memset(payload, 0xff, sizeof(payload));
            }
            else stl_le_p(payload, b->ppn_features[feature]);
        }
        uint32_t offset = MIN(b->ppn_aux_off, sizeof(payload));
        uint32_t available = MIN(bytes, sizeof(payload) - offset);
        memset(b->data, 0xff, bytes);
        memcpy(b->data, payload + offset, available);
        b->ppn_aux_off += available;
        b->data_off = 0; b->data_len = bytes;
    } else if (b->mode == MODE_ID) {
        uint8_t idb[8] = { 0 };
        if (ce >= 0 && (ce_mask & (1u << ce))) {
            stl_le_p(idb, id);
        }
        if (s->ppn) {
            const uint8_t ppn_id[8] = {0x50, 0x50, 0x4e, 1, 5, 5, 0, 0};
            memcpy(idb, ppn_id, sizeof(idb));
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
        /*
         * cfg-v0: the s5l8920x firmware reads a raw page as four 1 KiB data
         * phases (config 0xaa083) then four spare phases (0xa8003) on the
         * meta channel, so the whole spare goes to the meta FIFO.
         */
        uint32_t spare = s->cfg_v0 && raw && b->stride > page_bytes ? b->stride - page_bytes : 0;
        uint32_t meta = MIN(raw ? spare : META_BYTES,
                            sizeof(b->meta) - b->meta_off - b->meta_len);
        uint8_t *m = b->meta + b->meta_off + b->meta_len;

        bool ok = ce >= 0 && b->page_ok[ce & 7];
        const uint8_t *pg = b->page[ce & 7];

        if (ok) {
            memcpy(b->data + b->data_off + b->data_len, pg, transfer_bytes);
            memset(m, 0, meta);
            memcpy(m, pg + page_bytes, raw ? meta : MIN(meta, META_BYTES));
        } else {
            memset(b->data + b->data_off + b->data_len, 0xff, transfer_bytes);
            memset(m, 0xff, meta);
        }
        HT("h%d read ce %d row 0x%x ok %d fmt 0x%x mpp %u meta %u: %02x%02x%02x%02x %02x%02x%02x%02x %02x %02x\n",
           b->n, ce, ce >= 0 ? b->page_row[ce & 7] : b->row, ok, h2fmi_format(b), (h2fmi_format(b) >> 19) & 0x3f, meta,
           m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9]);
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
    uint32_t m = (h2fmi_format(b) >> 19) & 0x3f;

    return m ? m : META_BYTES;
}

/*
 * A write transfer (control 5) completes when the FIFOs hold the page: its data and its meta, which
 * the firmware streams on two CDMA chains (a multi-page write's meta chain runs ahead of or behind the
 * data one). A to-device chain is done when the FIFO it fills has been drained by the chip, each FIFO
 * for its own chain (EmbeddedIOP h2fmi_write_multi waits for both: "Timeout waiting for CDMA during
 * successful NAND write operation").
 */
static void h2fmi_seq_run(H2FMIBus *b);

static void h2fmi_write_check(H2FMIBus *b)
{
    uint32_t page_bytes = 0, id = 0, mper = h2fmi_meta_per_page(b);
    uint8_t mask;
    hwaddr base = b->s->mmio_base + b->n * H2FMI_WINDOW;

    if (b->s->ppn && b->mode == MODE_PPN_FEATURE_WRITE &&
        (b->fmi[FMI_CONTROL / 4] & 7) == 5) {
        uint32_t bytes = (h2fmi_format(b) >> 8) & 0xffff;
        if (bytes && bytes <= 4 && b->wdata_len >= bytes) {
            uint32_t feature = b->fmc[FMC_ADDR0 / 4] & 0xffff;
            uint32_t value = 0;
            memcpy(&value, b->wdata, bytes);
            b->ppn_features[feature] = le32_to_cpu(value);
            b->wdata_len = 0;
            b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
            h2fmi_update_irq(b);
        }
        return;
    }
    if (b->s->ppn && (b->fmi[FMI_CONTROL / 4] & 7) == 4) {
        h2fmi_seq_run(b);
        return;
    }
    if ((b->fmi[FMI_CONTROL / 4] & 7) != 5 || (b->s->explicit_start && !b->write_armed)) {
        return;
    }
    if (b->s->iop) {
        s5l8930_iop_nand_info(b->s->iop, &id, &mask, &page_bytes);
    }
    bool ppn_boot = b->s->ppn && h2fmi_raw_read(b);
    if (ppn_boot) {
        /* The PPN boot path uses FMI's byte-count format and generated BCH,
         * not the ordinary 4 KiB+metadata task. Original IOP 6660 programs
         * three 512-byte phases (FORMAT=0x20001) before one NAND confirm. */
        page_bytes = (h2fmi_format(b) >> 8) & 0xffff;
        mper = 0;
    }
    if (page_bytes && b->wdata_len >= page_bytes && b->wmeta_len >= mper &&
        !(b->fmi[FMI_STATUS / 4] & FMI_ST_DONE)) {
        int ce = h2fmi_ce(b);

        if (ce >= 0) {
            uint32_t offset = ppn_boot ? b->wpage_len[ce & 7] : 0;
            if (offset > H2FMI_BUF || page_bytes > H2FMI_BUF - offset) return;
            memcpy(b->wpage[ce & 7] + offset, b->wdata, page_bytes);
            memset(b->wpmeta[ce & 7], 0, META_BYTES);
            memcpy(b->wpmeta[ce & 7], b->wmeta, MIN(mper, META_BYTES));
            b->wpage_len[ce & 7] = offset + page_bytes;
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
        v |= (uint32_t)buf[*off + i] << (8 * i);
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
    uint32_t sectors = MAX(h2fmi_format(b) & 0xff, 1);
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

static uint64_t h2fmi_read(void *opaque, hwaddr off, unsigned size);
static void h2fmi_write(void *opaque, hwaddr off, uint64_t val, unsigned size);

/* One FMI DMA task is a logical page. PPN places its metadata after the
 * first 1024 bytes, before the remaining data bytes of that logical page. */
static bool h2fmi_seq_task(H2FMIBus *b)
{
    uint32_t bytes = (h2fmi_format(b) & 0xff) * 1024;
    uint32_t meta = (h2fmi_format(b) >> 19) & 0x3f;
    int ce = h2fmi_ce(b);
    if (b->mode == MODE_PPN_FEATURE_WRITE) {
        uint32_t count = (h2fmi_format(b) >> 8) & 0xffff;
        if (!count || count > sizeof(b->wdata) || b->wdata_len < count) return false;
        uint32_t feature = b->fmc[FMC_ADDR0 / 4] & 0xffff;
        if (count >= 4) b->ppn_features[feature] = ldl_le_p(b->wdata);
        b->wdata_len -= count;
        memmove(b->wdata, b->wdata + count, b->wdata_len);
        b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
        if (b->s->cdma && !b->wdata_len)
            s5l8930_cdma_sink_done(b->s->cdma,
                                 b->s->mmio_base + b->n * H2FMI_WINDOW + FMI_DATA, 4);
        h2fmi_update_irq(b);
        return true;
    }
    if (!bytes || bytes < 1024 || ce < 0 ||
        b->seq_page_off + bytes + meta > H2FMI_BUF) {
        return false;
    }
    if (b->writing) {
        if (b->wdata_len < bytes || b->wmeta_len < meta) return false;
        uint8_t *p = b->wpage[ce & 7] + b->seq_page_off;
        memcpy(p, b->wdata, 1024);
        memcpy(p + 1024, b->wmeta, meta);
        memcpy(p + 1024 + meta, b->wdata + 1024, bytes - 1024);
        b->seq_page_off += bytes + meta;
        b->wpage_len[ce & 7] = b->seq_page_off;
        b->wdata_len -= bytes;
        b->wmeta_len -= meta;
        memmove(b->wdata, b->wdata + bytes, b->wdata_len);
        memmove(b->wmeta, b->wmeta + meta, b->wmeta_len);
        b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
        if (b->s->cdma) {
            hwaddr base = b->s->mmio_base + b->n * H2FMI_WINDOW;
            if (!b->wdata_len) s5l8930_cdma_sink_done(b->s->cdma, base + FMI_DATA, 4);
            if (!b->wmeta_len) s5l8930_cdma_sink_done(b->s->cdma, base + FMI_META, 4);
            s5l8930_cdma_kick(b->s->cdma);
        }
        return true;
    }
    fifo_compact(b->data, &b->data_off, b->data_len, sizeof(b->data), bytes);
    fifo_compact(b->meta, &b->meta_off, b->meta_len, sizeof(b->meta), meta);
    if (b->data_off + b->data_len + bytes > sizeof(b->data) ||
        b->meta_off + b->meta_len + meta > sizeof(b->meta)) {
        return false;
    }
    uint8_t *d = b->data + b->data_off + b->data_len;
    uint8_t *m = b->meta + b->meta_off + b->meta_len;
    const uint8_t *page = b->page[ce & 7] + b->seq_page_off;
    if (b->page_ok[ce & 7]) {
        memcpy(d, page, 1024);
        memcpy(m, page + 1024, meta);
        memcpy(d + 1024, page + 1024 + meta, bytes - 1024);
    } else {
        memset(d, 0xff, bytes);
        memset(m, 0xff, meta);
    }
    b->seq_page_off += bytes + meta;
    b->data_len += bytes;
    b->meta_len += meta;
    HT("h%d PPN read row%x off%x bytes%u meta%u ok%d data%08x tag%08x\n",
       b->n, b->page_row[ce & 7], b->seq_page_off - bytes - meta,
       bytes, meta, b->page_ok[ce & 7], ldl_le_p(d),
       meta >= 4 ? ldl_le_p(m) : 0);
    b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
    if (b->s->cdma) {
        s5l8930_cdma_kick(b->s->cdma);
    }
    h2fmi_update_irq(b);
    return true;
}

static void h2fmi_seq_run(H2FMIBus *b)
{
    if (b->seq_running) {
        return;
    }
    b->seq_running = true;
    for (unsigned budget = 0; b->seq_code_n && budget < 4096; budget++) {
        uint32_t insn = b->seq_code[0], op = insn >> 24;
        uint32_t arg = insn & 0xffffff, consume = 1;
        HT("h%d FMISS %08x operands%u results%u pageoff%u writing%d mode%u wd%u wm%u fmt%x\n",
           b->n, insn, b->seq_data_n, b->seq_result_n, b->seq_page_off, b->writing,
           b->mode, b->wdata_len, b->wmeta_len, h2fmi_format(b));
        switch (op) {
        case 0x08: /* Direct NAND command. */
            h2fmi_command(b, arg & 0xff);
            if ((arg & 0xff) == 0x7a) b->seq_page_off = b->ppn_column;
            break;
        case 0x09: /* Chip enables. */
            b->fmc[FMC_CE / 4] = arg;
            break;
        case 0x0a: /* Poll NAND status: mask in byte1, expected in byte0. */
            if ((b->fmc[FMC_NAND_STATUS / 4] & ((arg >> 8) & 0xff)) !=
                (arg & 0xff)) goto stalled;
            break;
        case 0x0b: /* Execute one FMI DMA task. */
            if (!h2fmi_seq_task(b)) goto stalled;
            break;
        case 0x0c: /* Sequencer notification record (tag in bits16..23). */
            if (b->seq_regs[0x20 / 4] & 1) goto stalled;
            b->seq_regs[0x10 / 4] = arg;
            b->seq_regs[0x20 / 4] |= 1;
            b->fmi[FMI_STATUS / 4] |= 0x10000000;
            h2fmi_update_irq(b);
            break;
        case 0x0d: /* End of an uploaded macro; RAM slices are expanded inline. */
            break;
        case 0x0e: /* Delay in controller clock ticks; operations finish synchronously. */
            break;
        case 0x0f: /* Write immediate to controller-relative MMIO. */
            if (b->seq_code_n < 2) goto stalled;
            h2fmi_write(b, arg, b->seq_code[1], 4);
            consume = 2;
            break;
        case 0x10: /* Write next operand to controller-relative MMIO. */
            if (!b->seq_data_n) goto stalled;
            h2fmi_write(b, arg, b->seq_data[0], 4);
            memmove(b->seq_data, b->seq_data + 1,
                    --b->seq_data_n * sizeof(uint32_t));
            break;
        case 0x11: { /* Program-RAM slice: word start in bits8..23, count in0..7. */
            uint32_t start = arg >> 8, count = arg & 0xff;
            if (start + count > ARRAY_SIZE(b->seq_ram) ||
                b->seq_code_n - 1 + count > ARRAY_SIZE(b->seq_code)) goto stalled;
            memmove(b->seq_code + count, b->seq_code + 1,
                    (b->seq_code_n - 1) * sizeof(uint32_t));
            memcpy(b->seq_code, b->seq_ram + start, count * sizeof(uint32_t));
            b->seq_code_n += count - 1;
            continue;
        }
        case 0x12: /* Poll MMIO against following mask and expected words. */
            if (b->seq_code_n < 3 ||
                (h2fmi_read(b, arg, 4) & b->seq_code[1]) != b->seq_code[2])
                goto stalled;
            consume = 3;
            break;
        case 0x14: /* Read MMIO into the result FIFO. */
            if (b->seq_result_n == ARRAY_SIZE(b->seq_result)) goto stalled;
            b->seq_result[b->seq_result_n++] = h2fmi_read(b, arg, 4);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "h2fmi%d FMISS unsupported instruction %08x\n",
                          b->n, insn);
            goto stalled;
        }
        b->seq_code_n -= consume;
        memmove(b->seq_code, b->seq_code + consume,
                b->seq_code_n * sizeof(uint32_t));
    }
 stalled:
    b->seq_running = false;
}

static uint64_t h2fmi_read(void *opaque, hwaddr off, unsigned size)
{
    H2FMIBus *b = opaque;

    if (b->s->ppn && off >= 0xc0000 && off < 0xc0040) {
        h2fmi_seq_run(b);
        switch (off) {
        case 0xc0004: return MIN(b->seq_code_n, 32);
        case 0xc0018: return b->seq_data_n;
        case 0xc0034: return b->seq_result_n;
        case 0xc0030: {
            uint32_t v = b->seq_result_n ? b->seq_result[0] : 0;
            if (b->seq_result_n) memmove(b->seq_result, b->seq_result + 1,
                                       --b->seq_result_n * sizeof(uint32_t));
            return v;
        }
        default: return b->seq_regs[(off & 0x3f) / 4];
        }
    }
    if (b->s->ppn && off >= 0xc1000 && off < 0xc1400)
        return b->seq_ram[(off - 0xc1000) / 4];
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
            if (b->s->ppn && (b->fmi[FMI_CONTROL / 4] & 7) == 5 &&
                b->wdata_len < sizeof(b->wdata)) return 0x18;
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

    if (b->s->ppn && off >= 0xc1000 && off < 0xc1400) {
        b->seq_ram[(off - 0xc1000) / 4] = v;
        return;
    }
    if (b->s->ppn && off >= 0xc0000 && off < 0xc0040) {
        switch (off) {
        case 0xc0000:
            if (b->seq_code_n < ARRAY_SIZE(b->seq_code))
                b->seq_code[b->seq_code_n++] = v;
            break;
        case 0xc0014:
            if (b->seq_data_n < ARRAY_SIZE(b->seq_data))
                b->seq_data[b->seq_data_n++] = v;
            break;
        case 0xc0020: b->seq_regs[0x20 / 4] &= ~v; break;
        default: b->seq_regs[(off & 0x3f) / 4] = v; break;
        }
        h2fmi_seq_run(b);
        return;
    }
    if (off < sizeof(b->fmi)) {
        switch (off) {
        case FMI_CONTROL:
            HT("h%d ctl 0x%x prev 0x%x dl %u ml %u ecc %u/%u pend %u fmt 0x%x\n", b->n, v, b->fmi[off / 4],
               b->data_len, b->meta_len, b->ecc_n, b->ecc_reads, b->pending_n, h2fmi_format(b));
            if (b->s->ppn && (v == 6 || (v & 0x800))) {
                memset(b->ppn_read_count, 0, sizeof(b->ppn_read_count));
                b->data_len = b->meta_len = b->data_off = b->meta_off = 0;
                b->pending_n = b->ecc_n = b->ecc_reads = 0;
                b->fmi[FMI_STATUS / 4] &= ~FMI_ST_DONE;
            }
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
                /* Write transfer: done once the FIFOs hold the page. A new transfer's DONE is its own:
                 * 3.1.3's S5L8922 IOP firmware programs CE after CE without clearing the last one's. */
                b->fmi[off / 4] = v;
                b->fmi[FMI_STATUS / 4] &= ~FMI_ST_DONE;
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
            if ((v & 3) == 3 && (b->read_pending || unread ||
                                 (b->s->ppn && b->mode == MODE_PPN_AUX_READ) || (prev & 3) != 3 ||
                                 ((v & 0x80) && !(prev & 0x80)))) {
                if (!(b->fmi[off / 4] & 0x80)) {
                    b->ecc_n = b->ecc_reads = 0;
                }
                h2fmi_transfer(b, h2fmi_ce(b), false);
            } else if ((v & 3) == 3 && b->mode == MODE_PAGE &&
                       h2fmi_raw_read(b) &&
                       (b->read_format != h2fmi_format(b) || b->s->explicit_start)) {
                /* explicit-start: every control 3 is a phase of its own, the
                 * s5l8920x firmware's raw read takes the page in two with the
                 * same config. */
                /* Raw physical reads have separate data and spare phases.
                 * FORMAT selects a new phase within the existing byte stream;
                 * each phase completes independently and has a W1C DONE.
                 * Do not reload the page or require bytes still in the FIFO:
                 * a prepared CDMA chain may already have drained them. */
                b->read_format = h2fmi_format(b);
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
    H2FMIBus *b = &s->bus[(addr - s->mmio_base) / H2FMI_WINDOW % H2FMI_BUSES];
    hwaddr reg = (addr - s->mmio_base) % H2FMI_WINDOW;

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
        memset(b->seq_ram, 0, sizeof(b->seq_ram));
        memset(b->seq_regs, 0, sizeof(b->seq_regs));
        memset(b->ppn_features, 0, sizeof(b->ppn_features));
        b->seq_code_n = b->seq_data_n = b->seq_result_n = 0;
        b->seq_page_off = 0;
        b->seq_running = false;
        b->ppn_queries = 0;
        b->ppn_status = 0x40;
        b->ppn_aux_off = b->ppn_column = 0;
        memset(b->ppn_read_count, 0, sizeof(b->ppn_read_count));
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
        s5l8930_cdma_set_source(s->cdma, s->mmio_base,
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
            b->read_format = h2fmi_format(b);
        }
        if (b->data_len > sizeof(b->data) || b->meta_len > sizeof(b->meta) ||
            b->wdata_len > sizeof(b->wdata) || b->wmeta_len > sizeof(b->wmeta) ||
            b->ecc_n > H2FMI_QUEUE || b->pending_n > ARRAY_SIZE(b->pending_ce) ||
            b->stride > H2FMI_BUF || b->seq_code_n > ARRAY_SIZE(b->seq_code) ||
            b->seq_data_n > ARRAY_SIZE(b->seq_data) ||
            b->seq_result_n > ARRAY_SIZE(b->seq_result) ||
            b->seq_page_off > H2FMI_BUF || b->ppn_column > H2FMI_BUF || b->ppn_aux_off > 512) {                /* a raw read copies stride bytes of page[] */
            return -EINVAL;
        }
        for (unsigned ce = 0; ce < ARRAY_SIZE(b->wpage_len); ce++) {
            if (b->wpage_len[ce] > H2FMI_BUF || b->ppn_read_count[ce] > 128) {
                return -EINVAL;
            }
            for (unsigned j = 0; j < b->ppn_read_count[ce]; j++) {
                if (b->ppn_read_column[ce * 128 + j] > H2FMI_BUF)
                    return -EINVAL;
            }
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

static const VMStateDescription vmstate_h2fmi_ppn_bus = {
    .name = "h2fmi-ppn-bus",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ppn_features, H2FMIBus, 0x10000),
        VMSTATE_UINT8(ppn_opcode, H2FMIBus),
        VMSTATE_UINT8(ppn_status, H2FMIBus),
        VMSTATE_UINT32(ppn_aux_off, H2FMIBus),
        VMSTATE_UINT32(ppn_column, H2FMIBus),
        VMSTATE_UINT32_ARRAY(ppn_read_row, H2FMIBus, 8 * 128),
        VMSTATE_UINT32_ARRAY(ppn_read_column, H2FMIBus, 8 * 128),
        VMSTATE_UINT32_ARRAY(ppn_read_status, H2FMIBus, 8 * 128),
        VMSTATE_UINT32_ARRAY(ppn_read_count, H2FMIBus, 8),
        VMSTATE_UINT32_ARRAY(seq_ram, H2FMIBus, 256),
        VMSTATE_UINT32_ARRAY(seq_code, H2FMIBus, 1024),
        VMSTATE_UINT32_ARRAY(seq_data, H2FMIBus, 32),
        VMSTATE_UINT32_ARRAY(seq_result, H2FMIBus, 32),
        VMSTATE_UINT32_ARRAY(seq_regs, H2FMIBus, 16),
        VMSTATE_UINT32(seq_code_n, H2FMIBus),
        VMSTATE_UINT32(seq_data_n, H2FMIBus),
        VMSTATE_UINT32(seq_result_n, H2FMIBus),
        VMSTATE_UINT32(seq_page_off, H2FMIBus),
        VMSTATE_END_OF_LIST()
    },
};

static bool h2fmi_ppn_needed(void *opaque)
{
    return S5L8930_H2FMI(opaque)->ppn;
}

static const VMStateDescription vmstate_h2fmi_ppn = {
    .name = TYPE_S5L8930_H2FMI "/ppn",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = h2fmi_ppn_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(bus, S5L8930H2FMIState, H2FMI_BUSES, 1,
                             vmstate_h2fmi_ppn_bus, H2FMIBus),
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
        &vmstate_h2fmi_ppn,
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
    DEFINE_PROP_UINT64("mmio-base", S5L8930H2FMIState, mmio_base, S5L8930_H2FMI_BASE),
    DEFINE_PROP_UINT32("fmc-offset", S5L8930H2FMIState, fmc_off, 0x40000),
    DEFINE_PROP_UINT32("ecc-offset", S5L8930H2FMIState, ecc_off, 0x80000),
    DEFINE_PROP_UINT32("ecc-blank-summary", S5L8930H2FMIState, ecc_blank_summary, 0),
    DEFINE_PROP_BOOL("ppn", S5L8930H2FMIState, ppn, false),
    DEFINE_PROP_BOOL("explicit-start", S5L8930H2FMIState, explicit_start, false),
    DEFINE_PROP_BOOL("cfg-v0", S5L8930H2FMIState, cfg_v0, false),
};

static void s5l8930_h2fmi_class_init(ObjectClass *klass, const void *data)
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
