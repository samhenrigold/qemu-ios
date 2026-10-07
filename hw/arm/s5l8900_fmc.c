/*
 * S5L8900 FMC NAND controller (iPod touch 1G). See s5l8900_fmc.h.
 */
#include "hw/arm/s5l8900_fmc.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/error-report.h"

/* ---- page store: overlay directory over a read-only base directory ---- */

static int fmc_active_bank(S5L8900FMCState *s)
{
    uint32_t bank_bitmap = (s->fmctrl0 >> 1) & 0xFF;
    for (int bank = 0; bank < FMC_NUM_BANKS; bank++) {
        if (bank_bitmap & (1 << bank)) {
            return bank;
        }
    }
    return -1;
}

void s5l8900_fmc_set_bank(S5L8900FMCState *s, uint32_t bank)
{
    s->fmctrl0 &= ~(0xFF << 1);
    if (bank < FMC_NUM_BANKS) {
        s->fmctrl0 |= 1 << (bank + 1);
    }
}

static bool fmc_read_file(const char *path, uint8_t *data, uint8_t *spare)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(data, 1, FMC_BYTES_PER_PAGE, f);
    n += fread(spare, 1, FMC_BYTES_PER_SPARE, f);
    fclose(f);
    if (n != FMC_BYTES_PER_PAGE + FMC_BYTES_PER_SPARE) {
        qemu_log_mask(LOG_GUEST_ERROR, "[fmc] short page file %s\n", path);
    }
    return true;
}

static bool fmc_all_ones(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

bool s5l8900_fmc_page_erased(const uint8_t *data, const uint8_t *spare)
{
    return fmc_all_ones(data, FMC_BYTES_PER_PAGE) && fmc_all_ones(spare, FMC_BYTES_PER_SPARE);
}

/* What an erased NAND page reads as: every bit set, data and spare alike. */
static void fmc_erased_page(uint8_t *data, uint8_t *spare)
{
    memset(data, 0xFF, FMC_BYTES_PER_PAGE);
    memset(spare, 0xFF, FMC_BYTES_PER_SPARE);
}

static void fmc_erase_marker(S5L8900FMCState *s, uint32_t bank, uint32_t block,
                             char *buf, size_t len)
{
    snprintf(buf, len, "%s/bank%u/blk%u.erased", s->nand_overlay, bank, block);
}

/*
 * The page store: a read-only base directory (bank<N>/<page>.page, 2048 data
 * + 64 spare bytes) under a writable overlay directory of the same layout.
 * Programs land in the overlay as page files; a block erase (ADM 0x600)
 * deletes the block's overlay pages and leaves a blk<N>.erased marker, so
 * the block's base pages stop showing through. A page with no file anywhere
 * has never been programmed and reads erased (0xFF), as the chip would.
 */
void s5l8900_fmc_load_page(S5L8900FMCState *s, uint32_t bank, uint32_t page,
                           uint8_t *data, uint8_t *spare)
{
    char path[PATH_MAX];

    if (s->nand_overlay) {
        snprintf(path, sizeof(path), "%s/bank%u/%u.page", s->nand_overlay, bank, page);
        if (fmc_read_file(path, data, spare)) {
            return;
        }
        fmc_erase_marker(s, bank, page / FMC_PAGES_PER_BLOCK, path, sizeof(path));
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            fmc_erased_page(data, spare);
            return;
        }
    }
    if (s->nand_path) {
        snprintf(path, sizeof(path), "%s/bank%u/%u.page", s->nand_path, bank, page);
        if (fmc_read_file(path, data, spare)) {
            return;
        }
    }
    fmc_erased_page(data, spare);
}

/*
 * Program one page. NAND programs can only clear bits, so the stored page is
 * the old contents ANDed with the new: programming an erased page stores
 * exactly what the FTL sent, and a reprogram without an erase behaves as on
 * the chip instead of silently replacing the page.
 */
static bool fmc_program_page(S5L8900FMCState *s, uint32_t bank, uint32_t page,
                             const uint8_t *data, const uint8_t *spare)
{
    char path[PATH_MAX], dir[PATH_MAX];
    uint8_t cell[FMC_BYTES_PER_PAGE], cell_spare[FMC_BYTES_PER_SPARE];

    s->buffered_page = -1;
    if (!s->nand_overlay) {
        qemu_log_mask(LOG_UNIMP, "[fmc] program bank %u page %u dropped: no nand-overlay\n",
                      bank, page);
        return true;
    }
    s5l8900_fmc_load_page(s, bank, page, cell, cell_spare);
    if (!s5l8900_fmc_page_erased(cell, cell_spare)) {
        /* An FTL never does this to a consistent store: the page was not erased. */
        qemu_log_mask(LOG_GUEST_ERROR, "[fmc] program of bank %u page %u, which is not erased\n", bank, page);
        if (getenv("IT_FMC_TRACE")) {
            fprintf(stderr, "[fmc] program of bank %u page %u, which is not erased\n", bank, page);
        }
    }
    for (int i = 0; i < FMC_BYTES_PER_PAGE; i++) {
        cell[i] &= data[i];
    }
    for (int i = 0; i < FMC_BYTES_PER_SPARE; i++) {
        cell_spare[i] &= spare[i];
    }
    snprintf(dir, sizeof(dir), "%s/bank%u", s->nand_overlay, bank);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        error_report("FMC: cannot create %s: %s", dir, strerror(errno));
        return false;
    }
    snprintf(path, sizeof(path), "%s/%u.page", dir, page);
    FILE *f = fopen(path, "wb");
    if (!f) {
        error_report("FMC: cannot write %s: %s", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(cell, 1, FMC_BYTES_PER_PAGE, f) == FMC_BYTES_PER_PAGE &&
              fwrite(cell_spare, 1, FMC_BYTES_PER_SPARE, f) == FMC_BYTES_PER_SPARE;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        error_report("FMC: short write to %s", path);
    }
    return ok;
}

bool s5l8900_fmc_erase_block(S5L8900FMCState *s, uint32_t bank, uint32_t block)
{
    char path[PATH_MAX];

    s->buffered_page = -1;
    if (!s->nand_overlay) {
        qemu_log_mask(LOG_UNIMP, "[fmc] erase bank %u block %u dropped: no nand-overlay\n",
                      bank, block);
        return true;
    }
    snprintf(path, sizeof(path), "%s/bank%u", s->nand_overlay, bank);
    if (g_mkdir_with_parents(path, 0755) != 0) {
        error_report("FMC: cannot create %s: %s", path, strerror(errno));
        return false;
    }
    for (uint32_t p = block * FMC_PAGES_PER_BLOCK; p < (block + 1) * FMC_PAGES_PER_BLOCK; p++) {
        snprintf(path, sizeof(path), "%s/bank%u/%u.page", s->nand_overlay, bank, p);
        if (remove(path) != 0 && errno != ENOENT) {
            error_report("FMC: cannot erase %s: %s", path, strerror(errno));
            return false;
        }
    }
    fmc_erase_marker(s, bank, block, path, sizeof(path));
    FILE *m = fopen(path, "wb");
    if (!m || fclose(m) != 0) {
        error_report("FMC: cannot write %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

void s5l8900_fmc_start_program(S5L8900FMCState *s, unsigned count)
{
    s->reading_multiple_pages = false;
    s->prog_count = count;
    s->prog_index = 0;
    s->prog_pad = 0;
    s->fmdnum = FMC_BYTES_PER_PAGE;
    s->is_writing = count > 0;
}

void s5l8900_fmc_buffer_page(S5L8900FMCState *s, uint32_t page)
{
    int bank = fmc_active_bank(s);
    if (bank < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "[fmc] page %u requested with no bank selected\n", page);
        return;
    }
    if (bank != s->buffered_bank || (int32_t)page != s->buffered_page) {
        s5l8900_fmc_load_page(s, bank, page, s->page_buffer, s->page_spare_buffer);
        s->buffered_page = page;
        s->buffered_bank = bank;
    }
}

/* ---- registers ---- */

static uint32_t fmc_fifo_pop(S5L8900FMCState *s)
{
    uint32_t read_val = 0;

    if (s->cmd == FMC_CMD_ID) {
        /* A chip enable with no chip behind it answers nothing (the M68's 0x0f mask: four). */
        int bank = fmc_active_bank(s);
        return bank >= 0 && bank < (int)s->banks ? FMC_CHIP_ID : 0;
    }
    if (s->cmd == FMC_CMD_READSTATUS) {
        return 1 << 6;
    }
    if (s->reading_multiple_pages) {
        if (s->fmdnum % FMC_BYTES_PER_PAGE == 0) {
            s->cur_bank_reading++;
            if (s->cur_bank_reading >= FMC_MAX_LIST) {
                qemu_log_mask(LOG_GUEST_ERROR, "[fmc] page list overrun\n");
                return 0;
            }
            s5l8900_fmc_set_bank(s, s->banks_to_read[s->cur_bank_reading]);
        }
        uint32_t page_offset = s->fmdnum % FMC_BYTES_PER_PAGE;
        if (page_offset == 0) {
            page_offset = FMC_BYTES_PER_PAGE;
        }
        if (s->cur_bank_reading >= 0) {
            s5l8900_fmc_buffer_page(s, s->pages_to_read[s->cur_bank_reading]);
        }
        read_val = ((uint32_t *)s->page_buffer)[(FMC_BYTES_PER_PAGE - page_offset) / 4];
    } else {
        uint32_t page = (s->fmaddr1 << 16) | (s->fmaddr0 >> 16);
        s5l8900_fmc_buffer_page(s, page);
        if (s->reading_spare) {
            uint32_t i = (FMC_BYTES_PER_SPARE - s->fmdnum - 1) / 4;
            read_val = i < FMC_BYTES_PER_SPARE / 4 ? ((uint32_t *)s->page_spare_buffer)[i] : 0;
        } else {
            uint32_t i = (FMC_BYTES_PER_PAGE - s->fmdnum - 1) / 4;
            read_val = i < FMC_BYTES_PER_PAGE / 4 ? ((uint32_t *)s->page_buffer)[i] : 0;
        }
    }
    s->fmdnum -= 4;
    return read_val;
}

static uint64_t s5l8900_fmc_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8900FMCState *s = opaque;

    switch (addr) {
    case FMC_FMCTRL0:
        return s->fmctrl0;
    case FMC_FMFIFO:
        return fmc_fifo_pop(s);
    case FMC_FMCSTAT:
        /* Everything ready, all eight banks present. */
        return 0x1FFE;
    case FMC_RSCTRL:
        return s->rsctrl;
    default:
        return 0;
    }
}

static void s5l8900_fmc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8900FMCState *s = opaque;

    switch (addr) {
    case FMC_FMCTRL0:
        s->fmctrl0 = val;
        break;
    case FMC_FMCTRL1:
        s->fmctrl1 = val;
        break;
    case FMC_FMADDR0:
        s->fmaddr0 = val;
        break;
    case FMC_FMADDR1:
        s->fmaddr1 = val;
        break;
    case FMC_FMANUM:
        s->fmanum = val;
        break;
    case FMC_CMD:
        s->cmd = val;
        if (getenv("IT_FMC_TRACE") && val != FMC_CMD_READ && val != FMC_CMD_READSTATUS && val != 0) {
            fprintf(stderr, "[fmc] cmd 0x%02x ctrl0 0x%x ctrl1 0x%x addr0 0x%x addr1 0x%x anum %u\n", (unsigned)val,
                    s->fmctrl0, s->fmctrl1, s->fmaddr0, s->fmaddr1, s->fmanum);
        }
        break;
    case FMC_FMDNUM:
        s->reading_spare = (val == FMC_BYTES_PER_SPARE - 1);
        s->fmdnum = val;
        break;
    case FMC_FMFIFO:
        /*
         * A program streams through the FIFO as the ADMFMC driver's DMA list
         * lays it out (_fmcPerformPartialIO): each page's 2048 data bytes,
         * then a 16-byte pad descriptor, page after page for the queued list.
         * The pad carries nothing the page keeps (the spare is in data3).
         */
        if (!s->is_writing) {
            return;
        }
        if (s->prog_pad) {
            s->prog_pad -= 4;
            if (!s->prog_pad && s->prog_index == s->prog_count) {
                s->is_writing = false;
            }
            return;
        }
        ((uint32_t *)s->page_buffer)[(FMC_BYTES_PER_PAGE - s->fmdnum) / 4] = val;
        s->fmdnum -= 4;
        if (s->fmdnum == 0) {
            uint32_t i = s->prog_index++;
            uint8_t spare[FMC_BYTES_PER_SPARE];
            /* Bytes 12..63 are the controller's ECC; nothing here reads them. */
            memset(spare, 0xFF, sizeof(spare));
            memcpy(spare, s->prog_spares[i], FMC_META_BYTES);
            fmc_program_page(s, s->banks_to_read[i], s->pages_to_read[i], s->page_buffer, spare);
            if (getenv("IT_FMC_TRACE")) {
                uint32_t *w = (uint32_t *)s->page_buffer;
                fprintf(stderr, "[fmc] program %u/%u bank %u page %u meta type %02x: %08x %08x .. %08x\n",
                        i + 1, s->prog_count, s->banks_to_read[i], s->pages_to_read[i],
                        s->prog_spares[i][9], w[0], w[1], w[511]);
            }
            s->fmdnum = FMC_BYTES_PER_PAGE;
            s->prog_pad = FMC_PROGRAM_PAD;
        }
        break;
    case FMC_RSCTRL:
        s->rsctrl = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps s5l8900_fmc_ops = {
    .read = s5l8900_fmc_read,
    .write = s5l8900_fmc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_fmc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    S5L8900FMCState *s = S5L8900_FMC(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_fmc_ops, s, "s5l8900.fmc", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void s5l8900_fmc_reset(DeviceState *d)
{
    S5L8900FMCState *s = S5L8900_FMC(d);

    s->fmctrl0 = 0;
    s->fmctrl1 = 0;
    s->fmaddr0 = 0;
    s->fmaddr1 = 0;
    s->fmanum = 0;
    s->fmdnum = 0;
    s->rsctrl = 0;
    s->cmd = 0;
    s->reading_spare = false;
    s->reading_multiple_pages = false;
    s->is_writing = false;
    s->prog_count = s->prog_index = s->prog_pad = 0;
    s->cur_bank_reading = -1;
    s->buffered_page = -1;
    s->buffered_bank = -1;
}

static const Property s5l8900_fmc_properties[] = {
    DEFINE_PROP_STRING("nand", S5L8900FMCState, nand_path),
    DEFINE_PROP_STRING("nand-overlay", S5L8900FMCState, nand_overlay),
    /* chip enables populated: 8 on the N45, 4 on the M68 */
    DEFINE_PROP_UINT32("banks", S5L8900FMCState, banks, FMC_NUM_BANKS),
};

static void s5l8900_fmc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, s5l8900_fmc_reset);
    device_class_set_props(dc, s5l8900_fmc_properties);
}

static const TypeInfo s5l8900_fmc_info = {
    .name          = TYPE_S5L8900_FMC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900FMCState),
    .instance_init = s5l8900_fmc_init,
    .class_init    = s5l8900_fmc_class_init,
};

static void s5l8900_fmc_register_types(void)
{
    type_register_static(&s5l8900_fmc_info);
}

type_init(s5l8900_fmc_register_types)
