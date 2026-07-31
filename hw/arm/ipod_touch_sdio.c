#include "hw/arm/ipod_touch_sdio.h"

/*
 * Every register access and every command is worth seeing while the dongle
 * emulation is being built, and is pure noise once it works - and the volume
 * is high enough to slow the guest down measurably. Set IPOD_SDIO_TRACE=1.
 */
static int sdio_trace_enabled = -1;

static void G_GNUC_PRINTF(1, 2) trace_sdio(const char *fmt, ...)
{
    if (sdio_trace_enabled < 0) {
        const char *v = getenv("IPOD_SDIO_TRACE");
        sdio_trace_enabled = (v && *v && *v != '0');
    }
    if (!sdio_trace_enabled) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* Little-endian 24-bit CIS pointer, as the CCCR and FBRs store them. */
static void put_cis_ptr(uint8_t *dst, uint32_t offset)
{
    dst[0] = offset & 0xff;
    dst[1] = (offset >> 8) & 0xff;
    dst[2] = (offset >> 16) & 0xff;
}

/*
 * Lay out function 0's address space: the CCCR, one FBR per I/O function and
 * a tuple chain for each. IOSDIOFamily reads the MANFID tuple to build the
 * SDIOManufacturerId/SDIOProductId properties that AppleBCM4325's personality
 * matches on.
 */
static void ipod_touch_sdio_build_cia(IPodTouchSDIOState *s)
{
    /* Matches wifiaddr in the stock n72ap NOR's nvram. */
    static const uint8_t wlan_mac[6] = { 0x00, 0x23, 0x32, 0x6e, 0xaa, 0x10 };
    uint8_t *r = s->registers;

    r[CCCR_REVISION] = 0x11;     /* CCCR 1.10, SDIO 1.10 */
    r[CCCR_SD_REVISION] = 0x01;
    r[CCCR_CARD_CAPS] = 0x17;    /* direct commands, read wait, 4-bit interrupts */
    r[CCCR_HIGH_SPEED] = 0x01;
    put_cis_ptr(&r[CCCR_CIS_PTR], CIS_COMMON_OFFSET);

    /* Common tuple chain: who we are, then the function 0 extension. */
    uint8_t *cis = &r[CIS_COMMON_OFFSET];
    *cis++ = CIS_MANUFACTURER_ID;
    *cis++ = 0x04;
    *cis++ = BCM4325_MANUFACTURER & 0xff;
    *cis++ = (BCM4325_MANUFACTURER >> 8) & 0xff;
    *cis++ = BCM4325_PRODUCT_ID & 0xff;
    *cis++ = (BCM4325_PRODUCT_ID >> 8) & 0xff;
    *cis++ = CIS_FUNCTION_EXTENSION;
    *cis++ = 0x04;
    *cis++ = 0x00;               /* extension type 0: common */
    *cis++ = 0x00;               /* max block size 512 */
    *cis++ = 0x02;
    *cis++ = 0x32;               /* max transfer rate 25 MHz */

    /*
     * AppleBCM4325 gets its MAC address from here. Its parser walks this chain
     * looking for a CISTPL_FUNCE whose extension type is 4 and whose next byte
     * is 6, and copies the six bytes that follow; if it finds nothing it ends
     * up comparing an all-zero address against its reject constant and gives
     * up with "unable to obtain MAC address, can't proceed any further".
     */
    *cis++ = CIS_FUNCTION_EXTENSION;
    *cis++ = 0x08;
    *cis++ = 0x04;               /* extension type 4: MAC address */
    *cis++ = 0x06;               /* address length */
    for (unsigned i = 0; i < 6; i++) {
        *cis++ = wlan_mac[i];
    }

    *cis++ = CIS_END;

    for (unsigned fn = 1; fn <= BCM4325_FUNCTIONS; fn++) {
        uint8_t *fbr = &r[FBR_BASE(fn)];
        fbr[FBR_IFACE_CODE] = 0x00;  /* no standard SDIO interface */
        put_cis_ptr(&fbr[FBR_CIS_PTR], CIS_FUNC_OFFSET(fn));

        uint8_t *fcis = &r[CIS_FUNC_OFFSET(fn)];
        *fcis++ = CIS_FUNCTION_EXTENSION;
        *fcis++ = 0x2a;              /* the type 1 extension is a fixed 42 bytes */
        *fcis++ = 0x01;              /* extension type 1: per function */
        memset(fcis, 0, 0x29);
        fcis[0x0b] = 0x00;           /* max block size 512 */
        fcis[0x0c] = 0x02;
        fcis += 0x29;
        *fcis++ = CIS_END;
    }
}

/*
 * Function 1 reaches the whole chip through a sliding window: SBADDRLOW/MID/HIGH
 * supply address bits 8 and up, and the low 15 bits of the CMD52/CMD53 address
 * are the offset within it. Bit 15 asks for 32-bit accesses and is not part of
 * the address.
 *
 * Backing it lazily, one page at a time, keeps the sparse core register space
 * and the firmware image in the same store, which matters because the driver
 * reads back everything it writes.
 */
static uint8_t *backplane_page(IPodTouchSDIOState *s, uint32_t sb_addr)
{
    gpointer key = GUINT_TO_POINTER(sb_addr >> BACKPLANE_PAGE_BITS);
    uint8_t *page = g_hash_table_lookup(s->backplane, key);

    if (!page) {
        page = g_malloc0(BACKPLANE_PAGE_SIZE);
        g_hash_table_insert(s->backplane, key, page);
    }
    return page;
}

static uint32_t backplane_addr(IPodTouchSDIOState *s, uint32_t addr)
{
    return s->sb_window | (addr & SB_OFFSET_MASK);
}

static void backplane_write(IPodTouchSDIOState *s, uint32_t sb_addr,
                            const uint8_t *buf, uint32_t len)
{
    while (len) {
        uint32_t off = sb_addr & (BACKPLANE_PAGE_SIZE - 1);
        uint32_t n = MIN(len, BACKPLANE_PAGE_SIZE - off);
        memcpy(backplane_page(s, sb_addr) + off, buf, n);
        sb_addr += n;
        buf += n;
        len -= n;
    }
}

static void backplane_read(IPodTouchSDIOState *s, uint32_t sb_addr,
                           uint8_t *buf, uint32_t len)
{
    while (len) {
        uint32_t off = sb_addr & (BACKPLANE_PAGE_SIZE - 1);
        uint32_t n = MIN(len, BACKPLANE_PAGE_SIZE - off);
        memcpy(buf, backplane_page(s, sb_addr) + off, n);
        sb_addr += n;
        buf += n;
        len -= n;
    }
}

static void trigger_irq(void *opaque)
{
    IPodTouchSDIOState *s = (IPodTouchSDIOState *)opaque;
    s->irq_reg = 0x2;
    qemu_irq_raise(s->irq);
}

void sdio_exec_cmd(IPodTouchSDIOState *s)
{
    uint32_t cmd_type = s->cmd & 0x3f;
    uint32_t addr = (s->arg >> 9) & 0x1ffff;
    uint32_t func = (s->arg >> 28) & 0x7;
    trace_sdio("SDIO CMD: %d, ADDR: %d, FUNC: %d\n", cmd_type, addr, func);
    if(cmd_type == 0x3) {
        // CMD3 - SEND_RELATIVE_ADDR. R6 carries the address in the top half.
        s->resp0 = (SDIO_RCA << 16);
    }
    else if(cmd_type == 0x5) {
        // CMD5 - IO_SEND_OP_COND. The R4 response is how the controller learns
        // a card is there at all; without it enumerateSlot times out.
        if(s->card_present) {
            s->resp0 = R4_CARD_READY | (BCM4325_FUNCTIONS << R4_NUM_FUNCS_SHIFT)
                       | R4_IO_OCR;
        } else {
            s->resp0 = 0;
        }
    }
    else if(cmd_type == 0x7) {
        // select card - ignore
    }
    else if(cmd_type == 0x34) {
        // CMD52 - read/write from a register
        bool is_write = (s->arg >> 31) != 0;
        if(is_write) {
            uint8_t data = s->arg & 0xFF;
            if(func == 0x1 && addr >= SDIOD_CORE_BASE) {
                s->sdiod_regs[addr - SDIOD_CORE_BASE] = data;
                if(addr >= SBSDIO_SBADDRLOW && addr <= SBSDIO_SBADDRHIGH) {
                    unsigned shift = 8 + 8 * (addr - SBSDIO_SBADDRLOW);
                    uint32_t before = s->sb_window;
                    s->sb_window &= ~(0xffu << shift);
                    s->sb_window |= (uint32_t)data << shift;
                    (void)before;
                }
            }
            else if(func == 0x1) {
                uint8_t byte = data;
                backplane_write(s, backplane_addr(s, addr), &byte, 1);
            }
            else {
                s->registers[addr] = data;
                if(addr == 0x2) { s->registers[0x3] = data; } // if we write to register 2, we also write the same result to register 3 (this is the enabled functions register)
            }
            trace_sdio("SDIO: Executing cmd52 by writing 0x%02x to register 0x%05x (func %d)\n", data, addr, func);
        } else {
            if(addr == 0x1000e) {
                // misc register
                s->resp0 = (1 << 6) /* enable ALP clock */ | (1 << 7); /* enable HT clock */
            }
            else if(addr == 0x2020) {
                // some indication that packets are ready??
                s->resp0 = (1 << 6);
            }
            else if(func == 0x1 && addr >= SDIOD_CORE_BASE) {
                s->resp0 = s->sdiod_regs[addr - SDIOD_CORE_BASE];
            }
            else if(func == 0x1) {
                uint8_t byte = 0;
                backplane_read(s, backplane_addr(s, addr), &byte, 1);
                s->resp0 = byte;
            }
            else {
                trace_sdio("SDIO: Executing cmd52 by reading from 0x%02x (value: 0x%02x)\n", addr, s->registers[addr]);
                s->resp0 = s->registers[addr];
            }
        }
    }
    else if(cmd_type == 0x35) {
        // CMD53 - block transfer
        bool is_write = (s->arg >> 31) != 0;
        uint32_t xfer_len = s->blklen * s->numblk;
        uint32_t sb_addr = backplane_addr(s, addr);
        trace_sdio("SDIO: Executing cmd53 func %x with block size %d and %d blocks (reg address: 0x%08x, backplane address: 0x%08x, destination address: 0x%08x, write? %d)\n", func, s->blklen, s->numblk, addr, sb_addr, s->baddr, is_write);

        if(is_write) {
            if(func == 0x1) {
                g_autofree uint8_t *buf = g_malloc(xfer_len);
                cpu_physical_memory_read(s->baddr, buf, xfer_len);
                backplane_write(s, sb_addr, buf, xfer_len);
                /* Enough of a heartbeat to tell a running firmware download
                 * apart from a wedged one, without tracing every access. */
                s->fw_bytes += xfer_len;
                if(s->fw_bytes - s->fw_bytes_logged >= 65536) {
                    s->fw_bytes_logged = s->fw_bytes;
                    printf("[SDIO] %u KiB written to the backplane (now at 0x%08x)\n",
                           s->fw_bytes >> 10, sb_addr);
                }
            }
            else if(func == 0x2) {
                if(!s->func2_seen) {
                    s->func2_seen = true;
                    printf("[SDIO] first SDPCM frame on function 2 (%u bytes)\n", xfer_len);
                }
                // this is a BCM4325 command - add a frame to the queue and schedule the IRQ request to indicate that the command has been completed
                BCM4325FrameHeaderPacket *frame_header = calloc(sizeof(BCM4325FrameHeaderPacket), sizeof(uint8_t *));
                uint16_t length = s->blklen * s->numblk;
                frame_header->frame_length = length;
                frame_header->checksum = length ^ 0xffff;
                g_queue_push_tail(s->rx_fifo, frame_header);

                timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 100);
            }
        } else {
            if(func == 0x1) {
                g_autofree uint8_t *buf = g_malloc(xfer_len);
                backplane_read(s, sb_addr, buf, xfer_len);
                cpu_physical_memory_write(s->baddr, buf, xfer_len);
            }
            else if(func == 0x2) {
                // we're reading a frame
                BCM4325FrameHeaderPacket *frame_header = (BCM4325FrameHeaderPacket *) g_queue_pop_head(s->rx_fifo);

                if(!frame_header) {
                    // create an empty frame
                    frame_header = calloc(sizeof(BCM4325FrameHeaderPacket), sizeof(uint8_t *));

		    frame_header->frame_length = s->blklen  * s->numblk;
		    frame_header->checksum = (s->blklen * s->numblk) ^ 0xffff;
                }
                cpu_physical_memory_write(s->baddr, frame_header, sizeof(BCM4325FrameHeaderPacket));
            }
            
        }

        // toggle IRQ register
        s->irq_reg = 0x1;
        qemu_irq_raise(s->irq);
        //printf("Raised IRQ\n");
    }
    else {
        hw_error("Unknown SDIO command %d", cmd_type);
    }
}

static void ipod_touch_sdio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    trace_sdio("%s: writing 0x%08x to 0x%08x\n", __func__, (uint32_t)value, (uint32_t)addr);
    
    IPodTouchSDIOState *s = (struct IPodTouchSDIOState *) opaque;

    switch(addr) {
        case SDIO_CMD:
            s->cmd = value;
            if(value & (1 << 31)) { // execute bit is set
                sdio_exec_cmd(s);
            }
            break;
        case SDIO_ARGU:
            s->arg = value;
            break;
        case SDIO_STATE:
            s->state = value;
            break;
        case SDIO_STAC:
            s->stac = value;
            break;
        case SDIO_CSR:
            s->csr = value;
            break;
        case SDIO_IRQ:
            qemu_irq_lower(s->irq);
            break;
        case SDIO_IRQMASK:
            s->irq_mask = value;
            break;
        case SDIO_BADDR:
            s->baddr = value;
            break;
        case SDIO_BLKLEN:
            s->blklen = value;
            break;
        case SDIO_NUMBLK:
            s->numblk = value;
            break;
        default:
            break;
    }
}

static uint64_t ipod_touch_sdio_read(void *opaque, hwaddr addr, unsigned size)
{
    trace_sdio("%s: offset = 0x%08x\n", __func__, (uint32_t)addr);

    IPodTouchSDIOState *s = (struct IPodTouchSDIOState *) opaque;

    switch (addr) {
        case SDIO_CMD:
            return s->cmd;
        case SDIO_ARGU:
            return s->arg;
        case SDIO_STATE:
            return s->state;
        case SDIO_STAC:
            return s->stac;
        case SDIO_DSTA:
            return (1 << 0) | (1 << 4) ; // 0x1 indicates that the SDIO is ready for a CMD, (1 << 4) that the command is complete
        case SDIO_RESP0:
            return s->resp0;
        case SDIO_RESP1:
            return s->resp1 == 0 ? 0x7465 : s->resp1;
        case SDIO_RESP2:
            return s->resp2 == 0 ? 0x7374 : s->resp2;
        case SDIO_RESP3:
            return s->resp3 == 0 ? 0xFFFF : s->resp3;
        case SDIO_CSR:
            return s->csr;
        case SDIO_IRQ:
            return s->irq_reg;
        case SDIO_IRQMASK:
            return s->irq_mask;
        case SDIO_BADDR:
            return s->baddr;
        case SDIO_BLKLEN:
            return s->blklen;
        case SDIO_NUMBLK:
            return s->numblk;
        default:
            break;
    }

    return 0;
}

static const MemoryRegionOps ipod_touch_sdio_ops = {
    .read = ipod_touch_sdio_read,
    .write = ipod_touch_sdio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_sdio_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    IPodTouchSDIOState *s = IPOD_TOUCH_SDIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->backplane = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    s->sb_window = CHIPCOMMON_BASE;
    uint8_t chipid[4];
    stl_le_p(chipid, CHIPCOMMON_CHIPID);
    backplane_write(s, CHIPCOMMON_BASE, chipid, sizeof(chipid));

    ipod_touch_sdio_build_cia(s);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_sdio_ops, s, TYPE_IPOD_TOUCH_SDIO, 4096);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, trigger_irq, s);

    s->rx_fifo = g_queue_new();
}

static void ipod_touch_sdio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
}

static const TypeInfo ipod_touch_sdio_type_info = {
    .name = TYPE_IPOD_TOUCH_SDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchSDIOState),
    .instance_init = ipod_touch_sdio_init,
    .class_init = ipod_touch_sdio_class_init,
};

static void ipod_touch_sdio_register_types(void)
{
    type_register_static(&ipod_touch_sdio_type_info);
}

type_init(ipod_touch_sdio_register_types)
