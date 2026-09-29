/*
 * S5L8900 FMC NAND controller (iPod touch 1G). See s5l8900_fmc.h.
 */
#include "hw/arm/s5l8900_fmc.h"
#include "hw/qdev-properties.h"
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

static void fmc_erased_marker(S5L8900FMCState *s, uint32_t bank, uint32_t page,
                              char *buf, size_t len)
{
    snprintf(buf, len, "%s/bank%u/blk%u.erased", s->nand_overlay, bank,
             page / FMC_PAGES_PER_BLOCK);
}

static void fmc_blank_page(uint8_t *data, uint8_t *spare)
{
    memset(data, 0, FMC_BYTES_PER_PAGE);
    memset(spare, 0, FMC_BYTES_PER_SPARE);
    spare[0xA] = 0xFF;   /* FTL "free page" mark */
}

/*
 * Overlay first, then the base image, then a blank page -- except that a
 * block the guest has erased (marker file, see fmc_program_page) never falls
 * through to the base image's stale contents.
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
        fmc_erased_marker(s, bank, page, path, sizeof(path));
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            fmc_blank_page(data, spare);
            return;
        }
    }
    if (s->nand_path) {
        snprintf(path, sizeof(path), "%s/bank%u/%u.page", s->nand_path, bank, page);
        if (fmc_read_file(path, data, spare)) {
            return;
        }
    }
    if (getenv("IT_FMC_TRACE")) {
        fprintf(stderr, "[fmc] blank page bank %u page %u (block %u)\n", bank, page, page / FMC_PAGES_PER_BLOCK);
    }
    fmc_blank_page(data, spare);
}

/*
 * NAND cannot program a page without erasing its block first, and the FMC
 * never shows us the erase. Infer it as the FMSS model does: the first program
 * into a block (or a re-program of a page already in the overlay) erases the
 * block -- drop its overlay pages and leave a marker so reads of the untouched
 * pages return erased flash rather than the base image.
 */
static bool fmc_program_page(S5L8900FMCState *s, uint32_t bank, uint32_t page)
{
    char path[PATH_MAX], dir[PATH_MAX];

    if (!s->nand_overlay) {
        qemu_log_mask(LOG_UNIMP, "[fmc] program bank %u page %u dropped: no nand-overlay\n",
                      bank, page);
        return true;
    }
    snprintf(dir, sizeof(dir), "%s/bank%u", s->nand_overlay, bank);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        error_report("FMC: cannot create %s: %s", dir, strerror(errno));
        return false;
    }
    snprintf(path, sizeof(path), "%s/%u.page", dir, page);
    fmc_erased_marker(s, bank, page, dir, sizeof(dir));
    if (!g_file_test(dir, G_FILE_TEST_EXISTS) || g_file_test(path, G_FILE_TEST_EXISTS)) {
        uint32_t first = page - page % FMC_PAGES_PER_BLOCK;
        FILE *m = fopen(dir, "wb");
        if (!m) {
            error_report("FMC: cannot write %s: %s", dir, strerror(errno));
            return false;
        }
        fclose(m);
        for (uint32_t p = first; p < first + FMC_PAGES_PER_BLOCK; p++) {
            char victim[PATH_MAX];
            snprintf(victim, sizeof(victim), "%s/bank%u/%u.page", s->nand_overlay, bank, p);
            if (remove(victim) != 0 && errno != ENOENT) {
                error_report("FMC: cannot erase %s: %s", victim, strerror(errno));
                return false;
            }
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        error_report("FMC: cannot write %s: %s", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(s->page_buffer, 1, FMC_BYTES_PER_PAGE, f) == FMC_BYTES_PER_PAGE &&
              fwrite(s->page_spare_buffer, 1, FMC_BYTES_PER_SPARE, f) == FMC_BYTES_PER_SPARE;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        error_report("FMC: short write to %s", path);
    }
    return ok;
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
        return FMC_CHIP_ID;
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
        break;
    case FMC_FMDNUM:
        s->reading_spare = (val == FMC_BYTES_PER_SPARE - 1);
        s->fmdnum = val;
        break;
    case FMC_FMFIFO:
        if (!s->is_writing) {
            return;
        }
        if (s->fmdnum > FMC_BYTES_PER_PAGE || s->fmdnum < 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "[fmc] FIFO write with FMDNUM %u\n", s->fmdnum);
            s->is_writing = false;
            return;
        }
        ((uint32_t *)s->page_buffer)[(FMC_BYTES_PER_PAGE - s->fmdnum) / 4] = val;
        s->fmdnum -= 4;
        if (s->fmdnum == 0) {
            s->is_writing = false;
            fmc_program_page(s, s->buffered_bank, s->buffered_page);
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
    s->cur_bank_reading = -1;
    s->buffered_page = -1;
    s->buffered_bank = -1;
}

static const Property s5l8900_fmc_properties[] = {
    DEFINE_PROP_STRING("nand", S5L8900FMCState, nand_path),
    DEFINE_PROP_STRING("nand-overlay", S5L8900FMCState, nand_overlay),
};

static void s5l8900_fmc_class_init(ObjectClass *oc, void *data)
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
