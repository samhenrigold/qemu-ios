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

    if (s->nand_overlay) {
        snprintf(filename, sizeof(filename), "%s/cs%d/%d.page", s->nand_overlay, cs, page_nr);
        f = fopen(filename, "rb");
    }
    if (!f) {
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
}

/* Store one physical page (data + spare) into the overlay, atomically. */
static void fmss_store_page(IPodTouchFMSSState *s, uint32_t cs, uint32_t page_nr,
                            const uint8_t *data, const uint8_t *spare)
{
    char dir[1088], filename[1152], tmp[1200];
    snprintf(dir, sizeof(dir), "%s/cs%d", s->nand_overlay, cs);
    g_mkdir_with_parents(dir, 0755);
    snprintf(filename, sizeof(filename), "%s/%d.page", dir, page_nr);
    snprintf(tmp, sizeof(tmp), "%s/.%d.page.tmp", dir, page_nr);

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
 * The pages_in buffer the guest hands us is the FTL's *logical* request:
 *   [0] = 0x00801000 | cs_bitmap   (first command word)
 *   [1] = op selector              (0x300/0x400/0x480/0x500/0x600/0x700 ...)
 *   [2] = 0
 *   [3..] = physical page-number array
 * Source page data lives at pages_out (two 2048-byte DMA source addresses per
 * page); the per-page 12-byte spare record lives at spare_out (stride 0xc).
 *
 * INCOMPLETE / EXPERIMENTAL: the mapping from data-buffer index to physical
 * (chip-select, page) is only fully solved for single-page writes. Multi-page
 * writes stripe across the four chip-selects with a plane/geometry interleave
 * that the driver computes from request-struct tables not visible at this
 * register interface (the resulting (cmd,page) script is DMA'd from a driver
 * buffer we cannot locate, and the FMSS microcode is not executed here). The
 * best empirical decode (cs = i % 4, page = desc[3+i]) still mis-places a
 * handful of multi-page pages, and because the FTL requires all-or-nothing
 * consistency even that corrupts the image on the next boot. Persistence is
 * therefore gated behind the (off-by-default) `nandrw` option and should be
 * treated as experimental until the multi-page geometry is reverse-engineered
 * from AppleS5L8720xFMSS (_fmssPrepareWriteScatteredPages / -Sequential).
 */
static void write_nand_pages(IPodTouchFMSSState *s)
{
    if (!s->nand_overlay) {
        return; /* no writable overlay -> writes are discarded (original behaviour) */
    }

    int np = s->reg_num_pages;
    uint32_t hdr0 = 0;
    cpu_physical_memory_read(s->reg_pages_in_addr, &hdr0, 4);
    uint32_t base_cs = find_bit_index(hdr0 & 0xff);


    for (int i = 0; i < np; i++) {
        uint32_t page_nr = 0;
        cpu_physical_memory_read(s->reg_pages_in_addr + (3 + i) * 4, &page_nr, 4);

        /*
         * np==1 is exact: cs from the header bitmap, page from desc[3].
         *
         * Multi-page is NOT solved. cs = i%4 is empirical -- an earlier session
         * tested using the header's cs for every page and found it made the
         * reboot corruption strictly worse, and read-backs show cs cycling. The
         * header bitmap does only ever have one bit set, so it most likely names
         * the starting chip rather than the whole set; that is consistent with
         * cycling and is not evidence against it.
         *
         * The real geometry is page = desc[3 + g(i, np)] where g is a plane/CS
         * interleave that depends on np (np=5 looks linear, np=17 groups four
         * chip-selects per descriptor entry). Until g is recovered from the
         * driver, this is best-effort and still corrupts across a reboot.
         */
        uint32_t cs = (np == 1) ? base_cs : (uint32_t)(i & 3);

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

        /* Trailing entries of large descriptors are stale padding reading as
         * page 0; writing them would clobber a real low page. */
        if (page_nr != 0) {
            fmss_store_page(s, cs, page_nr, s->page_buffer, s->page_spare_buffer);
        }
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
            if(s->reg_csgenrc == 0xa01) { read_nand_pages(s); }
            else if(s->reg_csgenrc == 0xa02) { write_nand_pages(s); }
            else {
                /* Erase and the other opcodes are not modelled. Confirmed by
                 * instrumentation that none of them fires during boot or an
                 * app install, so erase is not what blocks persistence. */
            }
            break;
        default:
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
