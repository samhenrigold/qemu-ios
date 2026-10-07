/*
 * S5L8900 ADM: the NAND upload DMA in front of the FMC (iPod touch 1G,
 * 0x38800000). The kernel's flash driver drops a command block into the ADM's
 * data2 section (command, page count, per-page bank and page numbers) and
 * kicks CTRL2; the ADM queues the page list on the FMC and reports each page's
 * spare into the data3 section. Ported from devos50's ipod_touch_adm.c.
 * High-level emulation of the ADM firmware (fidelity class H): the real block
 * runs code from the code section, which is not interpreted here.
 */
#include "qemu/osdep.h"
#include "hw/arm/s5l8900_adm.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"

static uint32_t adm_read_be32(S5L8900ADMState *s, hwaddr addr)
{
    uint8_t b[4];
    address_space_read(&s->downstream_as, addr, MEMTXATTRS_UNSPECIFIED, b, 4);
    return ldl_be_p(b);
}

static uint16_t adm_read_be16(S5L8900ADMState *s, hwaddr addr)
{
    uint8_t b[2];
    address_space_read(&s->downstream_as, addr, MEMTXATTRS_UNSPECIFIED, b, 2);
    return lduw_be_p(b);
}

static uint8_t adm_read_u8(S5L8900ADMState *s, hwaddr addr)
{
    uint8_t b;
    address_space_read(&s->downstream_as, addr, MEMTXATTRS_UNSPECIFIED, &b, 1);
    return b;
}

/*
 * Report the first 12 spare bytes of each queued page into the data3 section:
 * the FTL's per-page metadata (logical page number, flags, its 0xFF "free"
 * mark at byte 10). devos50's model wrote a synthetic free mark for every
 * page of a multi-page read; the single-page path has always handed over the
 * page's own spare, and the FTL compares the two views.
 */
static void adm_report_spares(S5L8900ADMState *s, unsigned n)
{
    S5L8900FMCState *fmc = s->fmc;
    uint8_t data[FMC_BYTES_PER_PAGE], spare[FMC_BYTES_PER_SPARE];

    for (unsigned i = 0; i < n; i++) {
        s5l8900_fmc_load_page(fmc, fmc->banks_to_read[i], fmc->pages_to_read[i], data, spare);
        address_space_write(&s->downstream_as, s->data3_sec_addr + i * 0xC,
                            MEMTXATTRS_UNSPECIFIED, spare, 0xC);
    }
}

/* The transfer block: the first word in data2 followed by the three section addresses. */
static uint32_t adm_find_tf(S5L8900ADMState *s)
{
    uint8_t buf[0x2000];

    if (s->tf) {
        return s->tf;
    }
    address_space_read(&s->downstream_as, s->data2_sec_addr, MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));
    for (uint32_t o = 0; o + 16 <= sizeof(buf); o += 4) {
        if (ldl_be_p(buf + o + 4) == s->data1_sec_addr && ldl_be_p(buf + o + 8) == s->data2_sec_addr &&
            ldl_be_p(buf + o + 12) == s->data3_sec_addr) {
            return s->tf = o;
        }
    }
    return ADM_TF_V17;   /* not written yet */
}

/*
 * Where the per-page lists start: a byte of bank per page from +0x44, then a
 * big-endian page number per page. Firmware-17 lists 512 pages (pages at
 * +0x244), firmware-14 1024 (+0x444); each puts its transfer block at its own
 * offset in data2, which tells them apart.
 */
static uint32_t adm_pages_off(S5L8900ADMState *s)
{
    return ADM_CMD_BANKS + (s->tf == ADM_TF_V14 ? 0x400 : 0x200);
}

static void adm_run_command(S5L8900ADMState *s)
{
    S5L8900FMCState *fmc = s->fmc;
    hwaddr cmdblk = s->data2_sec_addr + adm_find_tf(s);
    const hwaddr pages = adm_pages_off(s);
    uint8_t raw[4];

    address_space_read(&s->downstream_as, cmdblk + ADM_CMD_OFFSET, MEMTXATTRS_UNSPECIFIED, raw, 4);
    uint32_t cmd = ldl_le_p(raw);
    uint16_t num_pages = adm_read_be16(s, cmdblk + ADM_CMD_NPAGES);

    if (!fmc) {
        return;
    }
    /* The firmware's completion mailbox (ADM +0x30..+0x3C, which the driver
     * copies after the interrupt): class, completion code, per-page status. */
    memset(s->result, 0, sizeof(s->result));
    if (getenv("IT_FMC_TRACE")) {
        fprintf(stderr, "[adm] command 0x%x, %u pages, ce %u, first bank %u page %u\n", cmd, num_pages,
                adm_read_u8(s, cmdblk + ADM_CMD_CE), adm_read_u8(s, cmdblk + ADM_CMD_BANKS),
                adm_read_be32(s, cmdblk + pages));
    }
    if (num_pages > FMC_MAX_LIST) {
        qemu_log_mask(LOG_GUEST_ERROR, "[adm] %u pages exceeds the list\n", num_pages);
        num_pages = FMC_MAX_LIST;
    }

    switch (cmd) {
    case ADM_CMD_READ_SEQ: {
        /* The same page from every bank (eight on the N45, four on the M68), row after row. */
        uint32_t page = adm_read_be32(s, cmdblk + pages), nb = fmc->banks;
        fmc->reading_multiple_pages = true;
        for (unsigned op = 0; op < num_pages / nb; op++, page++) {
            for (unsigned i = 0; i < nb; i++) {
                fmc->pages_to_read[op * nb + i] = page;
                fmc->banks_to_read[op * nb + i] = i;
            }
        }
        fmc->fmdnum = num_pages * FMC_BYTES_PER_PAGE;
        fmc->cur_bank_reading = -1;
        adm_report_spares(s, num_pages);
        break;
    }
    case ADM_CMD_READ:
        if (num_pages == 1) {
            uint8_t bank = adm_read_u8(s, cmdblk + ADM_CMD_BANKS);
            uint32_t page = adm_read_be32(s, cmdblk + pages);
            fmc->reading_multiple_pages = false;
            s5l8900_fmc_set_bank(fmc, bank);
            fmc->fmdnum = FMC_BYTES_PER_PAGE - 1;
            fmc->reading_spare = false;
            fmc->fmaddr0 = page << 16;
            fmc->fmaddr1 = (page >> 16) & 0xFF;
            fmc->cmd = FMC_CMD_READ;
            /* The spare goes straight into the data3 section. */
            s5l8900_fmc_buffer_page(fmc, page);
            address_space_write(&s->downstream_as, s->data3_sec_addr, MEMTXATTRS_UNSPECIFIED,
                                fmc->page_spare_buffer, FMC_BYTES_PER_SPARE);
            /*
             * An erased page (every data and spare bit set) is reported as
             * clean, status 0xFE, which the driver turns into the FTL's
             * "found clean page" (e00002e5): that is how the FTL finds the
             * end of what it programmed in a block.
             */
            if (s5l8900_fmc_page_erased(fmc->page_buffer, fmc->page_spare_buffer)) {
                s->result[2] = ADM_STATUS_CLEAN;
            }
        } else if (num_pages > 1) {
            fmc->reading_multiple_pages = true;
            for (unsigned i = 0; i < num_pages; i++) {
                fmc->pages_to_read[i] = adm_read_be32(s, cmdblk + pages + 4 * i);
                fmc->banks_to_read[i] = adm_read_u8(s, cmdblk + ADM_CMD_BANKS + i);
            }
            fmc->fmdnum = num_pages * FMC_BYTES_PER_PAGE;
            fmc->cur_bank_reading = -1;
            adm_report_spares(s, num_pages);
        }
        break;
    case ADM_CMD_WRITE_SEQ: {
        /*
         * Multi-bank program, READ_SEQ's twin: the same page on every bank,
         * num_pages / banks rows, each page's FTL metadata at data3 +
         * 0xC * index. The 1.x FTL flushes its context and full log rows this
         * way; devos50's model had no case for it, so those were dropped.
         */
        uint32_t page = adm_read_be32(s, cmdblk + pages), nb = fmc->banks;
        unsigned n = num_pages - num_pages % nb;
        for (unsigned i = 0; i < n; i++) {
            fmc->pages_to_read[i] = page + i / nb;
            fmc->banks_to_read[i] = i % nb;
        }
        address_space_read(&s->downstream_as, s->data3_sec_addr, MEMTXATTRS_UNSPECIFIED,
                           fmc->prog_spares, n * FMC_META_BYTES);
        s5l8900_fmc_start_program(fmc, n);
        break;
    }
    case ADM_CMD_WRITE: {
        /* One page; its metadata (the first 12 spare bytes) sits in data3,
         * where reads report it. */
        fmc->banks_to_read[0] = adm_read_u8(s, cmdblk + ADM_CMD_BANKS);
        fmc->pages_to_read[0] = adm_read_be32(s, cmdblk + pages);
        address_space_read(&s->downstream_as, s->data3_sec_addr, MEMTXATTRS_UNSPECIFIED,
                           fmc->prog_spares[0], FMC_META_BYTES);
        s5l8900_fmc_start_program(fmc, 1);
        break;
    }
    case ADM_CMD_ERASE: {
        /* _fmcPerformErase: the block's first page at +0x244, its bank (chip
         * enable) at +0x34, one block per command. */
        uint32_t bank = adm_read_u8(s, cmdblk + ADM_CMD_CE);
        uint32_t page = adm_read_be32(s, cmdblk + pages);
        if (bank < FMC_NUM_BANKS) {
            s5l8900_fmc_erase_block(fmc, bank, page / FMC_PAGES_PER_BLOCK);
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "[adm] unrecognised command 0x%x\n", cmd);
        break;
    }
}

static uint64_t s5l8900_adm_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8900ADMState *s = opaque;

    switch (offset) {
    case ADM_CTRL:
        return 0x2;    /* ready */
    case ADM_CTRL2:
        return 0x10;   /* upload finished */
    case ADM_RESULT ... ADM_RESULT + 0xC:
        return s->result[(offset - ADM_RESULT) / 4];
    default:
        return 0;
    }
}

static void s5l8900_adm_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    S5L8900ADMState *s = opaque;

    switch (offset) {
    case ADM_CTRL:
        if (value == 0x3) {
            /* Start-up: the firmware reports itself in data2 and the bank ids in data3. */
            uint32_t started = 0x50;
            uint32_t ids[8];
            for (int i = 0; i < 8; i++) {
                ids[i] = s->fmc && i < (int)s->fmc->banks ? FMC_CHIP_ID : 0;
            }
            address_space_write(&s->downstream_as, s->data2_sec_addr, MEMTXATTRS_UNSPECIFIED,
                                &started, sizeof(started));
            address_space_write(&s->downstream_as, s->data3_sec_addr, MEMTXATTRS_UNSPECIFIED,
                                ids, sizeof(ids));
        }
        break;
    case ADM_CTRL2:
        if (value == 0x2) {
            adm_run_command(s);
            qemu_irq_raise(s->irq);
        }
        if ((value & 0x2) == 0) {
            qemu_irq_lower(s->irq);
        }
        break;
    case ADM_CODE_SEC_ADDR:
        s->code_sec_addr = value;
        break;
    case ADM_DATA1_SEC_ADDR:
        s->data1_sec_addr = value;
        break;
    case ADM_DATA2_SEC_ADDR:
        s->data2_sec_addr = value;
        s->tf = 0;
        break;
    case ADM_DATA3_SEC_ADDR:
        s->data3_sec_addr = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps s5l8900_adm_ops = {
    .read = s5l8900_adm_read,
    .write = s5l8900_adm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_adm_realize(DeviceState *dev, Error **errp)
{
    S5L8900ADMState *s = S5L8900_ADM(dev);

    if (!s->downstream) {
        error_setg(errp, "ADM 'downstream' link not set");
        return;
    }
    address_space_init(&s->downstream_as, s->downstream, "adm-downstream");
}

static void s5l8900_adm_reset(DeviceState *dev)
{
    S5L8900ADMState *s = S5L8900_ADM(dev);

    s->code_sec_addr = s->data1_sec_addr = s->data2_sec_addr = s->data3_sec_addr = 0;
    s->tf = 0;
    qemu_irq_lower(s->irq);
}

static const Property s5l8900_adm_properties[] = {
    DEFINE_PROP_LINK("downstream", S5L8900ADMState, downstream,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void s5l8900_adm_init(Object *obj)
{
    S5L8900ADMState *s = S5L8900_ADM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8900_adm_ops, s, TYPE_S5L8900_ADM, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void s5l8900_adm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = s5l8900_adm_realize;
    device_class_set_legacy_reset(dc, s5l8900_adm_reset);
    device_class_set_props(dc, s5l8900_adm_properties);
}

static const TypeInfo s5l8900_adm_type_info = {
    .name = TYPE_S5L8900_ADM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8900ADMState),
    .instance_init = s5l8900_adm_init,
    .class_init = s5l8900_adm_class_init,
};

static void s5l8900_adm_register_types(void)
{
    type_register_static(&s5l8900_adm_type_info);
}

type_init(s5l8900_adm_register_types)
