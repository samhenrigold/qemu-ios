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
    uint32_t ecc_q[4];
    uint32_t ecc_n, ecc_reads;
    H2FMIMode mode;
    uint32_t row;
    bool page_ok;                       /* loaded page has data (else blank) */
    uint8_t page[H2FMI_BUF];
    uint32_t stride;
    uint8_t data[2 * H2FMI_BUF], meta[64];
    uint32_t data_len, meta_len;
} H2FMIBus;

struct S5L8930H2FMIState {
    SysBusDevice parent_obj;
    DeviceState *iop;                   /* owns the page store */
    DeviceState *cdma;
    H2FMIBus bus[H2FMI_BUSES];
};

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
        break;
    case 0x70:              /* status, then 0x00 back to the page register */
    case 0x00:
        break;
    case 0x30: {
        int ce = h2fmi_ce(b);
        /*
         * A new page: whatever the last read left in the FIFOs was never
         * going to be taken (a pipelined read has drained the previous page
         * by now), and must not shift this page's data.
         */
        b->data_len = b->meta_len = 0;
        b->mode = MODE_PAGE;
        b->page_ok = false;
        if (ce >= 0 && s->iop) {
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
            int cs = (ce & 7) * H2FMI_BUSES + b->n;
            b->page_ok = s5l8930_iop_nand_read(s->iop, cs / per_bus,
                                               cs % per_bus, b->row, b->page,
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
static void h2fmi_transfer(H2FMIBus *b)
{
    S5L8930H2FMIState *s = b->s;
    uint32_t page_bytes = 0, id = 0;
    uint8_t ce_mask = 0;
    int ce = h2fmi_ce(b);

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
    } else if (b->mode == MODE_PAGE && page_bytes &&
               b->data_len + page_bytes <= sizeof(b->data)) {
        /* FMI +0x34 bits 19-24: meta bytes per page (0x5ff0398c). */
        uint32_t meta = MIN(META_BYTES,
                            sizeof(b->meta) - b->meta_len);
        uint8_t *m = b->meta + b->meta_len;

        if (b->page_ok) {
            memcpy(b->data + b->data_len, b->page, page_bytes);
            memset(m, 0, meta);
            memcpy(m, b->page + page_bytes, MIN(meta, META_BYTES));
        } else {
            memset(b->data + b->data_len, 0xff, page_bytes);
            memset(m, 0xff, meta);
        }
        b->data_len += page_bytes;
        b->meta_len += meta;
        if (b->ecc_n == ARRAY_SIZE(b->ecc_q)) {
            memmove(b->ecc_q, b->ecc_q + 1, sizeof(b->ecc_q) - sizeof(b->ecc_q[0]));
            b->ecc_n--;
        }
        b->ecc_q[b->ecc_n++] = b->page_ok ? 0 : ECC_BLANK;
        b->ecc_summary = 0;
    }
    b->fmi[FMI_STATUS / 4] |= FMI_ST_DONE;
    h2fmi_update_irq(b);
    if (s->cdma) {
        s5l8930_cdma_kick(s->cdma);
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
    v = b->ecc_q[0];
    if (++b->ecc_reads >= sectors) {
        b->ecc_reads = 0;
        memmove(b->ecc_q, b->ecc_q + 1, sizeof(b->ecc_q) - sizeof(b->ecc_q[0]));
        b->ecc_n--;
    }
    return v;
}

static uint64_t h2fmi_read(void *opaque, hwaddr off, unsigned size)
{
    H2FMIBus *b = opaque;

    if (off < sizeof(b->fmi)) {
        switch (off) {
        case FMI_DATA:
            return fifo_pop(b->data, &b->data_len, size);
        case FMI_META:
            return fifo_pop(b->meta, &b->meta_len, size);
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
        return h2fmi_ecc_sector(b);
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
            if ((v & 3) == 3) {
                /* 0x82 announces the next page of a pipelined read
                 * (0x5ff0422e); anything else before 3 starts a new one. */
                if (b->fmi[off / 4] != 0x82) {
                    b->ecc_n = b->ecc_reads = 0;
                }
                h2fmi_transfer(b);
            }
            b->fmi[off / 4] = v;
            return;
        case FMI_STATUS:
            b->fmi[off / 4] &= ~v;
            break;
        case FMI_DATA:
        case FMI_META:
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
    /* ECC configuration (+0x08) and the rest: nothing to model. */
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
static uint32_t h2fmi_fifo_avail(void *opaque, hwaddr addr)
{
    S5L8930H2FMIState *s = opaque;
    H2FMIBus *b = &s->bus[(addr - S5L8930_H2FMI_BASE) / H2FMI_WINDOW % H2FMI_BUSES];
    hwaddr reg = (addr - S5L8930_H2FMI_BASE) % H2FMI_WINDOW;

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
