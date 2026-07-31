#include "hw/arm/ipod_touch_fmss.h"

static uint8_t find_bit_index(uint8_t num) {
    int index = 0;
    while (num > 1) {
        num >>= 1;
        index++;
    }
    return index;
}

static void write_chip_info(IPodTouchFMSSState *s)
{
    uint32_t chipid[] = { 0xb614d5ad, 0xb614d5ad, 0xb614d5ad, 0xb614d5ad };
    cpu_physical_memory_write(s->reg_cinfo_target_addr, &chipid, 0x10);
}

/*
 * Erased-block bookkeeping.
 *
 * The FMSS driver only ever issues read (csgenrc 0xa01) and write (0xa02)
 * through the trigger register, so a block erase is never visible to us -- but
 * it certainly happens, because the FTL rewrites blocks that are fully
 * populated in the base image while only programming a handful of their pages.
 * Without modelling the erase, the pages the FTL did not rewrite still read
 * back as the base image's *old* contents instead of as erased flash, so on
 * the next boot the FTL's scan finds stale valid-looking data where it expects
 * clean pages.
 *
 * NAND cannot program a page without erasing its block first, so we can infer
 * the erase from the write: the first time a block is programmed in the
 * overlay (and again whenever a page already present in the overlay is
 * reprogrammed) the whole block must have been erased just beforehand. We
 * record that as a marker file next to the pages so it survives a reboot, and
 * reads of unwritten pages in a marked block return erased flash rather than
 * falling through to the base image.
 */
static gpointer fmss_block_key(uint32_t cs, uint32_t block)
{
    return GUINT_TO_POINTER((cs << 24) | block);
}

static void fmss_block_marker_path(IPodTouchFMSSState *s, uint32_t cs,
                                   uint32_t block, char *buf, size_t len)
{
    snprintf(buf, len, "%s/cs%d/blk%u.erased", s->nand_overlay, cs, block);
}

static bool fmss_block_is_erased(IPodTouchFMSSState *s, uint32_t cs, uint32_t block)
{
    char marker[1152];

    if (!s->nand_overlay) {
        return false;
    }
    if (!s->erased_blocks) {
        s->erased_blocks = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    if (g_hash_table_contains(s->erased_blocks, fmss_block_key(cs, block))) {
        return true;
    }
    fmss_block_marker_path(s, cs, block, marker, sizeof(marker));
    if (g_file_test(marker, G_FILE_TEST_EXISTS)) {
        g_hash_table_add(s->erased_blocks, fmss_block_key(cs, block));
        return true;
    }
    return false;
}

/* Drop every overlay page of a block and mark it erased. */
static void fmss_erase_block(IPodTouchFMSSState *s, uint32_t cs, uint32_t block)
{
    char path[1152];
    uint32_t first = block * NAND_PAGES_PER_BLOCK;

    for (uint32_t p = first; p < first + NAND_PAGES_PER_BLOCK; p++) {
        snprintf(path, sizeof(path), "%s/cs%d/%u.page", s->nand_overlay, cs, p);
        remove(path);
    }

    snprintf(path, sizeof(path), "%s/cs%d", s->nand_overlay, cs);
    g_mkdir_with_parents(path, 0755);
    fmss_block_marker_path(s, cs, block, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) { fclose(f); }

    if (!s->erased_blocks) {
        s->erased_blocks = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    g_hash_table_add(s->erased_blocks, fmss_block_key(cs, block));
}

/*
 * Load one physical page (data + spare) into the caller buffers. When a
 * writable overlay is configured it takes precedence over the read-only base
 * image (copy-on-write); a page present in neither reads back as a blank/erased
 * page, exactly as the original read-only model did.
 */
static void fmss_load_page(IPodTouchFMSSState *s, uint32_t cs, uint32_t page_nr,
                           uint8_t *data, uint8_t *spare)
{
    char filename[1088];
    FILE *f = NULL;
    bool from_overlay = false;

    if (s->nand_overlay) {
        snprintf(filename, sizeof(filename), "%s/cs%d/%d.page", s->nand_overlay, cs, page_nr);
        f = fopen(filename, "rb");
        from_overlay = (f != NULL);
        if (f && getenv("FMSS_RTRACE")) {
            printf("RH cs=%u page=%u\n", cs, page_nr); fflush(stdout);
        }
    }
    if (!f && !fmss_block_is_erased(s, cs, page_nr / NAND_PAGES_PER_BLOCK)) {
        snprintf(filename, sizeof(filename), "%s/cs%d/%d.page", s->nand_path, cs, page_nr);
        f = fopen(filename, "rb");
    }

    if (!f) {
        memset(data, 0, NAND_BYTES_PER_PAGE);
        memset(spare, 0, NAND_BYTES_PER_SPARE);
        ((uint32_t *)spare)[2] = 0x00FF00FF; /* clean/erased marker */
        return;
    }
    if (fread(data, 1, NAND_BYTES_PER_PAGE, f) != NAND_BYTES_PER_PAGE) { /* short read tolerated */ }
    if (fread(spare, 1, NAND_BYTES_PER_SPARE, f) != NAND_BYTES_PER_SPARE) { /* ditto */ }
    fclose(f);

    /* Diagnostic: serve the base image's spare metadata even for overlay
     * pages, to test whether the persisted FTL metadata is what breaks the
     * next boot. */
    if (from_overlay && getenv("FMSS_BASESPARE")) {
        snprintf(filename, sizeof(filename), "%s/cs%d/%d.page", s->nand_path, cs, page_nr);
        FILE *bf = fopen(filename, "rb");
        memset(spare, 0, NAND_BYTES_PER_SPARE);
        if (bf) {
            if (fseek(bf, NAND_BYTES_PER_PAGE, SEEK_SET) == 0) {
                if (fread(spare, 1, NAND_BYTES_PER_SPARE, bf) != NAND_BYTES_PER_SPARE) { }
            }
            fclose(bf);
        } else {
            ((uint32_t *)spare)[2] = 0x00FF00FF;
        }
    }
}

/* Store one physical page (data + spare) into the overlay, atomically. */
static void fmss_store_page(IPodTouchFMSSState *s, uint32_t cs, uint32_t page_nr,
                            const uint8_t *data, const uint8_t *spare)
{
    char dir[1088], filename[1152], tmp[1200];
    uint32_t block = page_nr / NAND_PAGES_PER_BLOCK;

    snprintf(dir, sizeof(dir), "%s/cs%d", s->nand_overlay, cs);
    g_mkdir_with_parents(dir, 0755);
    snprintf(filename, sizeof(filename), "%s/%d.page", dir, page_nr);
    snprintf(tmp, sizeof(tmp), "%s/.%d.page.tmp", dir, page_nr);

    /* A page cannot be programmed unless its block was erased first: either
     * this is the first program into the block, or the page already holds
     * overlay data and is being reprogrammed. Either way, model the erase. */
    if (getenv("FMSS_ERASE") &&
        (!fmss_block_is_erased(s, cs, block) ||
         g_file_test(filename, G_FILE_TEST_EXISTS))) {
        fmss_erase_block(s, cs, block);
    }

    FILE *f = fopen(tmp, "wb");
    if (!f) { return; }
    fwrite(data, 1, NAND_BYTES_PER_PAGE, f);
    fwrite(spare, 1, NAND_BYTES_PER_SPARE, f);
    fclose(f);
    if (rename(tmp, filename) != 0) { remove(tmp); }
}

static void read_nand_pages(IPodTouchFMSSState *s)
{
    // boot args
    const char *boot_args = "kextlog=0xfff debug=0x8 cpus=1 rd=disk0s1 serial=1 pmu-debug=0x1 io=0xffff8fff debug-usb=0xffffffff amfi_allow_any_signature=1 -v zalloc_debug"; // if not const then overwritten
    cpu_physical_memory_write(0x0ff2a584, boot_args, strlen(boot_args));

    // patch iBoot - we want to inject the bluetooth MAC address which is located as sub-node of uart1 and not uart3 in the device tree...
    const char *chr = "arm-io/uart1/bluetooth";
    cpu_physical_memory_write(0x0ff2206c, chr, strlen(chr));

    int page_out_buf_ind = 0;
    for(int page_ind = 0; page_ind < s->reg_num_pages; page_ind++) {
        uint32_t page_nr = 0;
        uint32_t page_out_addr = 0;
        uint32_t cs = 0;
        cpu_physical_memory_read(s->reg_pages_in_addr + (page_ind * sizeof(uint32_t)), &page_nr, sizeof(uint32_t));
        cpu_physical_memory_read(s->reg_cs_buf_addr + (page_ind * sizeof(uint32_t)), &cs, sizeof(uint32_t));
        uint32_t og_cs = cs;
        cs = find_bit_index(cs);

        if(cs > 3) {
            printf("CS %d invalid! (original CS: %d, reading page %d)\n", cs, og_cs, page_nr);
            hw_error("CS %d invalid!", cs);
        }

        fmss_load_page(s, cs, page_nr, s->page_buffer, s->page_spare_buffer);

        // we write away the page in two parts, 2048 bytes first and then the other 2048 bytes.
        int write_buf_size = NAND_BYTES_PER_PAGE / 2;
        for(int i = 0; i < 2; i++) {
            cpu_physical_memory_read(s->reg_pages_out_addr + (page_out_buf_ind * sizeof(uint32_t)), &page_out_addr, sizeof(uint32_t));
            cpu_physical_memory_write(page_out_addr, s->page_buffer + i * write_buf_size, write_buf_size);
            page_out_buf_ind++;
        }

        // finally, write the spare
        cpu_physical_memory_write(s->reg_page_spare_out_addr + page_ind * 0xc, s->page_spare_buffer, NAND_BYTES_PER_SPARE);
    }
}

/*
 * Handle a flash WRITE command (csgenrc 0xa02), storing pages into the overlay.
 *
 * Unlike a read, a write descriptor is NOT a flat page array. The driver
 * (_fmssPrepareWriteSequential / _fmssPrepareWriteScatteredPages in
 * com.apple.driver.AppleS5L8720xFMSS) builds a small script of two-word
 * entries and points FMSS_CS_BUF_ADDR (0xD10) at it:
 *
 *   [2i]   command word: 0x00801000 | (1 << csShift) | plane bits,
 *                        plus 0x70000000 when this chip-select already
 *                        appeared earlier in the same script
 *   [2i+1] physical page number on that chip-select
 *   ...
 *   [2n]   0x00000000 terminator
 *
 * A real 8-entry script observed at runtime (4 chip-selects x 2 planes):
 *   00801101 00000602  70811001 00000682   <- cs0, pages 0x602 / 0x682
 *   00801102 00000602  70811002 00000682   <- cs1
 *   00801104 00000602  70811004 00000682   <- cs2
 *   00801108 00000602  70811008 00000682   <- cs3
 *   00000000                                <- terminator
 *
 * Two consequences, both different from a read:
 *  - The write path never writes FMSS_PAGES_IN_ADDR (0xD0C) or FMSS_NUM_PAGES
 *    (0xD18); those still hold the previous *read*'s values, so reg_num_pages
 *    is meaningless here. The entry count comes from the terminator.
 *  - Everything past the terminator is stale data from earlier commands that
 *    shared the same DMA buffer, and must be ignored.
 *
 * Entry i's page data is the two 2048-byte DMA source addresses at
 * pages_out[2i] and pages_out[2i+1]; its 12-byte spare record is at
 * spare_out + i*0xc (stride 0xc == the driver's 3 spare words per page).
 */
#define FMSS_MAX_WRITE_ENTRIES 512

static void write_nand_pages(IPodTouchFMSSState *s)
{
    if (!s->nand_overlay) {
        return; /* no writable overlay -> writes are discarded (original behaviour) */
    }

    uint32_t desc = s->reg_cs_buf_addr;

    for (int i = 0; i < FMSS_MAX_WRITE_ENTRIES; i++) {
        uint32_t cmd = 0, page_nr = 0;
        cpu_physical_memory_read(desc + (2 * i) * 4, &cmd, 4);
        if (cmd == 0) {
            break; /* end-of-script terminator */
        }
        cpu_physical_memory_read(desc + (2 * i + 1) * 4, &page_nr, 4);

        uint32_t cs = find_bit_index(cmd & 0xff);
        if (cs > 3) {
            printf("%s: bad chip-select in command word 0x%08x (entry %d)\n",
                   __func__, cmd, i);
            break;
        }

        /* gather the 4096-byte page from the two 2048-byte DMA source halves */
        int half = NAND_BYTES_PER_PAGE / 2;
        uint32_t src0 = 0, src1 = 0;
        cpu_physical_memory_read(s->reg_pages_out_addr + (2*i) * 4, &src0, 4);
        cpu_physical_memory_read(s->reg_pages_out_addr + (2*i + 1) * 4, &src1, 4);
        memset(s->page_buffer, 0, NAND_BYTES_PER_PAGE);
        if (src0) cpu_physical_memory_read(src0, s->page_buffer, half);
        if (src1) cpu_physical_memory_read(src1, s->page_buffer + half, half);

        /* gather the 12-byte spare record, zero-padded to the on-disk 64 bytes */
        memset(s->page_spare_buffer, 0, NAND_BYTES_PER_SPARE);
        cpu_physical_memory_read(s->reg_page_spare_out_addr + i * 0xc, s->page_spare_buffer, 0xc);

        if (getenv("FMSS_DUMP")) {
            static int nd = 0;
            if (nd++ < 400) {
                uint32_t *sp = (uint32_t *)s->page_spare_buffer;
                uint32_t d0 = 0, d1 = 0;
                memcpy(&d0, s->page_buffer, 4);
                memcpy(&d1, s->page_buffer + half, 4);
                printf("WE i=%2d cmd=%08x cs=%u page=%6u src=%08x/%08x data=%08x/%08x spare=%08x %08x %08x\n",
                       i, cmd, cs, page_nr, src0, src1, d0, d1, sp[0], sp[1], sp[2]);
                fflush(stdout);
            }
        }

        fmss_store_page(s, cs, page_nr, s->page_buffer, s->page_spare_buffer);
    }
}

static uint64_t ipod_touch_fmss_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchFMSSState *s = (IPodTouchFMSSState *)opaque;
    switch(addr)
    {
        case FMSS__CS_BUF_RST_OK:
            return 0x1;
        case FMSS__CS_IRQ:
            return s->reg_cs_irq_bit;
        case FMSS__CS_IRQMASK:
            return 0x1;
        case FMSS__FMCTRL1:
            return (0x1 << 30);
        case 0xD00:
            return 42;
        case 0x00000C30:
            return 0x1;
        default:
            printf("%s: read invalid location 0x%08x.\n", __func__, addr);
            break;
    }
    return 0;
}

static void ipod_touch_fmss_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchFMSSState *s = (IPodTouchFMSSState *)opaque;

    switch(addr) {
        case 0xC00:
            if(val == 0x0000ffb5) { s->reg_cs_irq_bit = 1; } // TODO ugly and hard-coded
            if(val == 0xfff5) { s->reg_cs_irq_bit = 1; qemu_set_irq(s->irq, 1); }
            break;
        case FMSS__CS_IRQ:
            s->reg_cs_irq_bit = 0;
            qemu_set_irq(s->irq, 0);
            break;
        case FMSS_CINFO_TARGET_ADDR:
            s->reg_cinfo_target_addr = val;
            write_chip_info(s);
            break;
        case FMSS_PAGES_IN_ADDR:
            s->reg_pages_in_addr = val;
            break;
        case FMSS_CS_BUF_ADDR:
            s->reg_cs_buf_addr = val;
            break;
        case FMSS_NUM_PAGES:
            s->reg_num_pages = val;
            break;
        case FMSS_PAGE_SPARE_OUT_ADDR:
            s->reg_page_spare_out_addr = val;
            break;
        case FMSS_PAGES_OUT_ADDR:
            s->reg_pages_out_addr = val;
            break;
        case FMSS_CSGENRC:
            s->reg_csgenrc = val;
            break;
        case 0xD38:
            if (getenv("FMSS_TRACE") && s->reg_csgenrc != 0xa01 && s->reg_csgenrc != 0xa02) {
                printf("FMSS_OP csgenrc=%08x d0c=%08x d10=%08x d18=%08x\n",
                       s->reg_csgenrc, s->reg_pages_in_addr,
                       s->reg_cs_buf_addr, s->reg_num_pages);
                fflush(stdout);
            }
            if(s->reg_csgenrc == 0xa01) { read_nand_pages(s); }
            else if(s->reg_csgenrc == 0xa02) { write_nand_pages(s); }
            else {
                /* Erase and the other opcodes are not modelled. Confirmed by
                 * instrumentation that none of them fires during boot or an
                 * app install, so erase is not what blocks persistence. */
            }
            break;
        default:
            if (getenv("FMSS_TRACE")) {
                static uint8_t seen[0x1000];
                if (addr < 0x1000 && !seen[addr]) {
                    seen[addr] = 1;
                    printf("FMSS_W %04x = %08x (first)\n", (unsigned)addr, (unsigned)val);
                    fflush(stdout);
                }
            }
            break;
    }
}

static const MemoryRegionOps fmss_ops = {
    .read = ipod_touch_fmss_read,
    .write = ipod_touch_fmss_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_fmss_realize(DeviceState *dev, Error **errp)
{

}

static void ipod_touch_fmss_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchFMSSState *s = IPOD_TOUCH_FMSS(dev);

    memory_region_init_io(&s->iomem, obj, &fmss_ops, s, "fmss", 0xF00);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->page_buffer = (uint8_t *)g_malloc(NAND_BYTES_PER_PAGE);
    s->page_spare_buffer = (uint8_t *)g_malloc(NAND_BYTES_PER_SPARE);
}

static void ipod_touch_fmss_finalize(Object *obj)
{
    IPodTouchFMSSState *s = IPOD_TOUCH_FMSS(obj);

    g_free(s->page_buffer);
    g_free(s->page_spare_buffer);
}

static void ipod_touch_fmss_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_fmss_realize;
}

static const TypeInfo ipod_touch_fmss_info = {
    .name          = TYPE_IPOD_TOUCH_FMSS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchFMSSState),
    .instance_init = ipod_touch_fmss_init,
    .instance_finalize = ipod_touch_fmss_finalize,
    .class_init    = ipod_touch_fmss_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_fmss_info);
}

type_init(ipod_touch_machine_types)
