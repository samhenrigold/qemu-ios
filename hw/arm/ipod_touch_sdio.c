#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "migration/qemu-file-types.h"
#include "hw/arm/ipod_touch_sdio.h"
#include "hw/arm/mrvl8686.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "net/checksum.h"
#include "trace.h"
#include "hw/trace-printf.h"

/*
 * Every register access and every command is worth seeing while the dongle
 * emulation is being built, and is pure noise once it works - and the volume
 * is high enough to slow the guest down measurably. Use -trace ipod_touch_sdio_log.
 */
static void G_GNUC_PRINTF(1, 2) trace_sdio(const char *fmt, ...)
{
    if (!trace_event_get_state_backends(TRACE_IPOD_TOUCH_SDIO_LOG)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    g_autofree char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    trace_ipod_touch_sdio_log(g_strchomp(msg));
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
static unsigned sdio_functions(IPodTouchSDIOState *s)
{
    return s->chip.functions ? s->chip.functions : BCM4325_FUNCTIONS;
}

static void ipod_touch_sdio_build_cia(IPodTouchSDIOState *s)
{
    const BCMSDIOChip *chip = &s->chip;
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
    *cis++ = chip->manfid & 0xff;
    *cis++ = (chip->manfid >> 8) & 0xff;
    *cis++ = chip->prodid & 0xff;
    *cis++ = (chip->prodid >> 8) & 0xff;
    if (chip->vers1[0]) {
        /*
         * CISTPL_VERS_1: IOSDIOFamily publishes the strings as
         * IOSDIOManufacturer, IOSDIOProduct, IOSDIOProductInfo0/1, and
         * AppleBCMWLAN's IOSDIOStringContains picks the board personality
         * from the last two.
         */
        uint8_t *len = &cis[1];
        *cis++ = CIS_VERS_1;
        cis++;
        *cis++ = 0x01;           /* major */
        *cis++ = 0x00;           /* minor */
        for (unsigned i = 0; i < ARRAY_SIZE(chip->vers1) && chip->vers1[i]; i++) {
            size_t n = strlen(chip->vers1[i]) + 1;
            memcpy(cis, chip->vers1[i], n);
            cis += n;
        }
        *cis++ = 0xff;
        *len = cis - len - 1;
    }
    if (!chip->no_common_funce) {
        *cis++ = CIS_FUNCTION_EXTENSION;
        *cis++ = 0x04;
        *cis++ = 0x00;           /* extension type 0: common */
        *cis++ = 0x00;           /* max block size 512 */
        *cis++ = 0x02;
        *cis++ = 0x32;           /* max transfer rate 25 MHz */
    }

    /*
     * AppleBCM4325 gets its MAC address from here. Its parser walks this chain
     * looking for a CISTPL_FUNCE whose extension type is 4 and whose next byte
     * is 6, and copies the six bytes that follow; if it finds nothing it ends
     * up comparing an all-zero address against its reject constant and gives
     * up with "unable to obtain MAC address, can't proceed any further".
     */
    if (!chip->no_mac_funce) {
        *cis++ = CIS_FUNCTION_EXTENSION;
        *cis++ = 0x08;
        *cis++ = 0x04;           /* extension type 4: MAC address */
        *cis++ = 0x06;           /* address length */
        for (unsigned i = 0; i < 6; i++) {
            *cis++ = chip->mac[i];
        }
    }

    if (chip->has_bt_mac) {
        /* Apple's combo-card OTP: a vendor tuple containing type 3, five
         * 16-bit words (header plus six address bytes). Stock AppleBCM4325's
         * parseAppleConfigData uses this instead of synthesizing BT from WLAN. */
        *cis++ = 0x80;
        *cis++ = 11;
        *cis++ = 0x81;
        stw_le_p(cis, 3); cis += 2;
        stw_le_p(cis, 5); cis += 2;
        memcpy(cis, chip->bt_mac, sizeof(chip->bt_mac));
        cis += sizeof(chip->bt_mac);
    }

    *cis++ = CIS_END;

    for (unsigned fn = 1; fn <= sdio_functions(s); fn++) {
        uint8_t *fbr = &r[FBR_BASE(fn)];
        /* 1.x's IOSDIOFamily makes a nub only for a function with a nonzero code. */
        fbr[FBR_IFACE_CODE] = chip->fbr_iface;
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

    if (!page && g_hash_table_size(s->backplane) >= BACKPLANE_MAX_PAGES) {
        /* Past the cap every new page shares one scratch page. */
        qemu_log_mask(LOG_GUEST_ERROR, "[SDIO] backplane store full at 0x%08x\n", sb_addr);
        if (!s->backplane_spill) {
            s->backplane_spill = g_malloc0(BACKPLANE_PAGE_SIZE);
        }
        return s->backplane_spill;
    }
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
    s->irq_reg |= s->irq_pending;
    s->irq_pending = 0;
    qemu_irq_raise(s->irq);
}

/*
 * Interrupts are raised during the register write that causes them. Deferring
 * them through the timer was tried, on the theory that the guest was losing
 * the wakeup and waiting out a timeout - the firmware download runs at about
 * twenty seconds per 4 KB block, which looks exactly like that. It is not:
 * with a 20 us delay the driver instead fails its GPIO writes with an I/O
 * timeout and the download gets slower still. Whatever is slow, this is not
 * it.
 */
static void raise_irq_soon(IPodTouchSDIOState *s, uint32_t bits)
{
    s->irq_reg |= bits;
    qemu_irq_raise(s->irq);
}

/* --- the SDIO device core's mailbox ------------------------------------- */

static uint32_t sdpcm_reg_read(IPodTouchSDIOState *s, uint32_t off)
{
    uint8_t buf[4];
    backplane_read(s, s->chip.sdiod_base + off, buf, sizeof(buf));
    return ldl_le_p(buf);
}

static void sdpcm_reg_write(IPodTouchSDIOState *s, uint32_t off, uint32_t val)
{
    uint8_t buf[4];
    stl_le_p(buf, val);
    backplane_write(s, s->chip.sdiod_base + off, buf, sizeof(buf));
}

static void sdpcm_raise(IPodTouchSDIOState *s, uint32_t intbits)
{
    sdpcm_reg_write(s, SDPCM_INTSTATUS, sdpcm_reg_read(s, SDPCM_INTSTATUS) | intbits);
    if (sdpcm_reg_read(s, SDPCM_HOSTINTMASK) & intbits) {
        raise_irq_soon(s, 0x2);
    }
}

/*
 * Announce the dongle. The driver takes the core out of reset and then expects
 * a host mailbox interrupt whose data word says the firmware is up, with the
 * SDPCM protocol version in bits 16-23. A mismatch there is only a warning on
 * its side, so this is a safe place to be approximately right.
 */
static void sdpcm_announce_ready(IPodTouchSDIOState *s)
{
    if (s->fullmac.dongle_started) {
        return;
    }
    s->fullmac.dongle_started = true;
    sdpcm_reg_write(s, SDPCM_TOHOSTMAILBOXDATA,
                    HMB_DATA_DEVREADY | HMB_DATA_FWREADY |
                    (SDPCM_PROT_VERSION << HMB_DATA_VERSION_SHIFT));
    trace_sdio("[SDIO] dongle announced ready\n");
    sdpcm_raise(s, I_HMB_HOST_INT);
}

static void sdpcm_queue_frame(IPodTouchSDIOState *s, uint8_t *data, uint32_t len)
{
    SDPCMFrame *f;

    /* Replies and events the host never reads would queue without end. */
    if (g_queue_get_length(s->rx_fifo) >= SDPCM_RX_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "[SDIO] host queue full, frame dropped\n");
        g_free(data);
        return;
    }
    f = g_new0(SDPCMFrame, 1);
    f->data = data;
    f->len = len;
    g_queue_push_tail(s->rx_fifo, f);
    sdpcm_raise(s, I_HMB_FRAME_IND);
}

/*
 * Build a frame for the host: hardware tag, software header, then the payload
 * the caller has already laid out.
 */
static void sdpcm_send(IPodTouchSDIOState *s, uint8_t channel,
                       const uint8_t *payload, uint32_t payload_len)
{
    /*
     * payload_len needs no guard: every caller bounds it already. sdpcm_send_event
     * passes a host-constant frame length, sdio_net_receive is capped at 1600
     * bytes by its own size test, and sdpcm_handle_cdc clamps to CDC_MAX_PAYLOAD
     * on a get and to the received frame length on a set.
     */
    uint32_t len = SDPCM_HDRLEN + payload_len;
    uint8_t *buf = g_malloc0(len);

    stw_le_p(buf, len);
    stw_le_p(buf + 2, ~len & 0xffff);
    buf[4] = s->tx_seq++;
    buf[5] = channel & SDPCM_CHANNEL_MASK;
    buf[6] = 0;                      /* no hint about the next frame */
    buf[7] = SDPCM_HDRLEN;           /* payload starts right after the header */
    buf[8] = 0;                      /* no flow control */
    buf[9] = s->rx_seq + 8;          /* credit: how far ahead the host may run */
    memcpy(buf + SDPCM_HDRLEN, payload, payload_len);

    sdpcm_queue_frame(s, buf, len);
}

/*
 * Store into the backplane, giving the few registers that are not plain memory
 * their behavior. Everything else is backed by the page store so the driver's
 * read-back verification passes.
 */
static void backplane_store(IPodTouchSDIOState *s, uint32_t sb_addr,
                            const uint8_t *buf, uint32_t len)
{
    bool in_core = sb_addr >= s->chip.sdiod_base &&
                   sb_addr < s->chip.sdiod_base + SDPCM_CORE_SIZE;
    uint32_t off = sb_addr - s->chip.sdiod_base;

    if (in_core && off == SDPCM_INTSTATUS && len >= 4) {
        /* Write one to clear. */
        uint32_t clear = ldl_le_p(buf);
        sdpcm_reg_write(s, SDPCM_INTSTATUS,
                        sdpcm_reg_read(s, SDPCM_INTSTATUS) & ~clear);
        return;
    }

    backplane_write(s, sb_addr, buf, len);

    if (sb_addr == CHIPCOMMON_CORECTL) {
        sdpcm_announce_ready(s);
    } else if (in_core && off == SDPCM_HOSTINTMASK && len >= 4) {
        /* The mask arrives after the core is started, so re-evaluate. */
        sdpcm_announce_ready(s);
        sdpcm_raise(s, sdpcm_reg_read(s, SDPCM_INTSTATUS) & ldl_le_p(buf));
    } else if (in_core && off == SDPCM_TOSBMAILBOX && len >= 4 &&
               (ldl_le_p(buf) & SMB_INT_ACK)) {
        sdpcm_reg_write(s, SDPCM_INTSTATUS,
                        sdpcm_reg_read(s, SDPCM_INTSTATUS) & ~I_HMB_HOST_INT);
    }
}

static void sdio_fullmac_send(void *opaque, unsigned channel,
                              const uint8_t *data, uint32_t len)
{
    sdpcm_send(opaque, channel, data, len);
}

static bool sdio_fullmac_can_receive(void *opaque)
{
    IPodTouchSDIOState *s = opaque;
    return g_queue_get_length(s->rx_fifo) < 16;
}

void ipod_touch_sdio_setup_net(IPodTouchSDIOState *s)
{
    bcm_fullmac_setup_net(&s->fullmac, DEVICE(s), TYPE_IPOD_TOUCH_SDIO, "wifi0");
}

/* One frame written by the host on function 2. */
static void sdpcm_receive(IPodTouchSDIOState *s, const uint8_t *buf, uint32_t len)
{
    if (s->rx_log < 16) {
        s->rx_log++;
        trace_sdio("[SDIO] host frame (%u bytes):", len);
        for (unsigned i = 0; i < MIN(len, 24u); i++) {
            trace_sdio(" %02x", buf[i]);
        }
        trace_sdio("\n");
    }

    if (len < SDPCM_HDRLEN) {
        return;
    }

    uint16_t framelen = lduw_le_p(buf);
    uint16_t check = lduw_le_p(buf + 2);
    if ((framelen ^ check) != 0xffff) {
        trace_sdio("[SDIO] SDPCM frame with a bad tag: len %u check %04x\n",
               framelen, check);
        return;
    }

    uint8_t channel = buf[5] & SDPCM_CHANNEL_MASK;
    uint8_t doff = buf[7];
    s->rx_seq = buf[4];

    if (doff < SDPCM_HDRLEN || doff > framelen || framelen > len) {
        trace_sdio("[SDIO] SDPCM frame with an unusable offset: doff %u len %u\n",
               doff, framelen);
        return;
    }

    switch (channel) {
    case SDPCM_CONTROL_CHANNEL:
        if (framelen - doff >= CDC_HDRLEN) {
            bcm_fullmac_cdc(&s->fullmac, buf + doff, framelen - doff, 0);
        }
        break;
    case SDPCM_DATA_CHANNEL:
        bcm_fullmac_tx_data(&s->fullmac, buf + doff, framelen - doff);
        /*
         * The host learns its credit (how many frames it may send) only from
         * the header of a frame coming back, and a data frame gets no answer:
         * eight in a row with nothing to the host in between (ARP probes,
         * IPv6 ND and mDNS after a join, with no one on the network to
         * reply) spent the window, the next control command could not be
         * sent, and after 5 s AppleBCMWLANCmdManager's queue check reset the
         * chip ("Cmd Queue stall", logState "Tx: seq N, credit N"), which
         * dropped the join (LightTouchMac smoke #40). A dongle that frees the
         * buffer returns the credit; with nothing else queued, in a
         * header-only frame on the event channel: 5.x's rxPackets (9B206
         * 0x80a23f52) takes the credit from every header and passes over a
         * header-only frame there (on the data channel it is "memory
         * allocation error"). Drivers that take events on the data channel
         * (2.1.1) keep the bare window.
         */
        if (g_queue_is_empty(s->rx_fifo) && s->fullmac.bdc_hdrlen == BDC_HDRLEN_STD) {
            sdpcm_send(s, SDPCM_EVENT_CHANNEL, buf, 0);
        }
        break;
    default:
        trace_sdio("[SDIO] SDPCM frame on unhandled channel %u\n", channel);
        break;
    }
}

/*
 * Everything the host does once the dongle has announced itself, bounded so it
 * cannot run away. This is the window that matters while the control channel
 * is being brought up, and the firmware download that precedes it would drown
 * it out.
 */
static void trace_post_ready(IPodTouchSDIOState *s, const char *what,
                             uint32_t func, uint32_t addr, uint32_t len,
                             bool is_write)
{
    if (!s->fullmac.dongle_started || s->ready_log >= 6000) {
        return;
    }
    s->ready_log++;
    trace_sdio("[SDIO] %s func %u %s addr 0x%05x len %u\n", what,
           func, is_write ? "write" : "read", addr, len);
}

static void sdio_exec_cmd(IPodTouchSDIOState *s);

/*
 * Move a CMD53 payload between the card and guest memory: one contiguous
 * buffer at baddr for the iPod's controller, or the scatter list an IOP
 * command carries (ipod_touch_sdio_command).
 */
static void sdio_dma(IPodTouchSDIOState *s, uint8_t *buf, uint32_t len,
                     bool to_guest)
{
    if (s->hbuf) {
        memcpy(to_guest ? s->hbuf : buf, to_guest ? buf : s->hbuf, len);
        return;
    }
    if (!s->sg) {
        address_space_rw(&address_space_memory, s->baddr, MEMTXATTRS_UNSPECIFIED,
                         buf, len, to_guest);
        return;
    }
    for (unsigned i = 0; i < s->sg_count && len; i++) {
        uint32_t n = MIN(len, s->sg[2 * i + 1]);
        address_space_rw(&address_space_memory, s->sg[2 * i], MEMTXATTRS_UNSPECIFIED,
                         buf, n, to_guest);
        buf += n;
        len -= n;
    }
    if (len) {
        qemu_log_mask(LOG_GUEST_ERROR, "[SDIO] scatter list %u bytes short\n",
                      len);
    }
}

uint32_t ipod_touch_sdio_command(IPodTouchSDIOState *s, uint32_t cmd,
                                 uint32_t arg, uint32_t blklen,
                                 uint32_t numblk, const uint32_t *sg,
                                 unsigned sg_count)
{
    s->cmd = cmd;
    s->arg = arg;
    s->blklen = blklen;
    s->numblk = numblk;
    s->sg = sg;
    s->sg_count = sg_count;
    s->resp0 = 0;
    sdio_exec_cmd(s);
    s->sg = NULL;
    return s->resp0;
}

uint32_t ipod_touch_sdio_command_buf(IPodTouchSDIOState *s, uint32_t cmd,
                                     uint32_t arg, uint32_t blklen,
                                     uint32_t numblk, uint8_t *buf)
{
    uint32_t resp;

    s->hbuf = buf;
    resp = ipod_touch_sdio_command(s, cmd, arg, blklen, numblk, NULL, 0);
    s->hbuf = NULL;
    return resp;
}

bool ipod_touch_sdio_card_irq(IPodTouchSDIOState *s)
{
    return s->fullmac.dongle_started &&
           (sdpcm_reg_read(s, SDPCM_INTSTATUS) &
            sdpcm_reg_read(s, SDPCM_HOSTINTMASK));
}

void ipod_touch_sdio_set_chip(IPodTouchSDIOState *s, const BCMSDIOChip *chip)
{
    uint8_t chipid[4];

    if (chip != &s->chip) {
        s->chip = *chip;
    }
    memcpy(s->fullmac.mac, chip->mac, sizeof(s->fullmac.mac));
    s->fullmac.fw_version = chip->fw_version;
    stl_le_p(chipid, chip->chipid);
    backplane_write(s, CHIPCOMMON_BASE, chipid, sizeof(chipid));
    ipod_touch_sdio_build_cia(s);
}

static void sdio_exec_cmd(IPodTouchSDIOState *s)
{
    uint32_t cmd_type = s->cmd & 0x3f;
    uint32_t addr = (s->arg >> 9) & 0x1ffff;
    uint32_t func = (s->arg >> 28) & 0x7;
    trace_sdio("SDIO CMD: %d, ADDR: %d, FUNC: %d\n", cmd_type, addr, func);
    if (cmd_type == 0x34 || cmd_type == 0x35) {
        trace_post_ready(s, cmd_type == 0x34 ? "cmd52" : "cmd53", func, addr,
                         cmd_type == 0x34 ? 1 : s->blklen * s->numblk,
                         (s->arg >> 31) != 0);
    }
    if(cmd_type == 0x3) {
        // CMD3 - SEND_RELATIVE_ADDR. R6 carries the address in the top half.
        s->resp0 = (SDIO_RCA << 16);
    }
    else if(cmd_type == 0x5) {
        // CMD5 - IO_SEND_OP_COND. The R4 response is how the controller learns
        // a card is there at all; without it enumerateSlot times out.
        if(s->card_present) {
            s->resp0 = R4_CARD_READY | (sdio_functions(s) << R4_NUM_FUNCS_SHIFT)
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
            if(func == 0x1 && s->mrvl) {
                mrvl8686_writeb(s->mrvl, addr, data);
            }
            else if(func == 0x0 && s->mrvl && addr == CCCR_IO_ABORT && (data & CCCR_IO_ABORT_RES)) {
                mrvl8686_card_reset(s->mrvl);
            }
            else if(func == 0x1 && addr >= SDIOD_CORE_BASE) {
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
                backplane_store(s, backplane_addr(s, addr), &byte, 1);
            }
            else {
                /*
                 * No IT_IDX here, and none on the sdiod_regs accesses above:
                 * addr is (arg >> 9) & 0x1ffff, so the guest can express
                 * 0..0x1ffff and both arrays are sized to cover exactly that
                 * (registers[0x20000]; sdiod_regs[0x10000] indexed from
                 * SDIOD_CORE_BASE == 0x10000). A clamp would be dead code, and
                 * the bound that matters is the array declaration -- if either
                 * is ever shrunk, these four sites need a real guard.
                 */
                s->registers[addr] = data;
                if(addr == 0x2) { s->registers[0x3] = data; } // if we write to register 2, we also write the same result to register 3 (this is the enabled functions register)
            }
            trace_sdio("SDIO: Executing cmd52 by writing 0x%02x to register 0x%05x (func %d)\n", data, addr, func);
        } else {
            if(func == 0x1 && s->mrvl) {
                s->resp0 = mrvl8686_readb(s->mrvl, addr);
            }
            else if(func == 0x0 && s->mrvl && addr == CCCR_INT_PENDING) {
                s->resp0 = mrvl8686_irq_pending(s->mrvl) ? CCCR_INT_PENDING_FN1 : 0;
            }
            else if(addr == 0x1000e) {
                // misc register
                s->resp0 = (1 << 6) /* enable ALP clock */ | (1 << 7); /* enable HT clock */
            }
            else if(func == 0x1 && addr >= SDIOD_CORE_BASE) {
                s->resp0 = s->sdiod_regs[addr - SDIOD_CORE_BASE];
            }
            else if(func == 0x1) {
                uint8_t byte = 0;
                backplane_read(s, backplane_addr(s, addr), &byte, 1);
                s->resp0 = byte;
            }
            else if(func == 0x0 && addr == CCCR_INT_PENDING) {
                /* How the driver finds out which function raised the card
                 * interrupt. Derive it rather than latch it, so acknowledging
                 * the dongle's mailbox clears this too. */
                uint32_t pending = sdpcm_reg_read(s, SDPCM_INTSTATUS) &
                                   sdpcm_reg_read(s, SDPCM_HOSTINTMASK);
                s->resp0 = pending ? (CCCR_INT_PENDING_FN1 | CCCR_INT_PENDING_FN2) : 0;
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

        /*
         * blklen and numblk are both unvalidated 32-bit MMIO writes and their
         * product feeds g_malloc below, so a guest could ask for a 4 GiB
         * allocation (or overflow the multiply outright). The real controller
         * caps a block at 2048 bytes and 511 blocks; be generous but finite.
         */
        if (s->blklen > 2048 || s->numblk > 511 ||
            (s->blklen && xfer_len / s->blklen != s->numblk)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "[SDIO] rejecting CMD53 with blklen %u numblk %u\n",
                          s->blklen, s->numblk);
            return;
        }
        trace_sdio("SDIO: Executing cmd53 func %x with block size %d and %d blocks (reg address: 0x%08x, backplane address: 0x%08x, destination address: 0x%08x, write? %d)\n", func, s->blklen, s->numblk, addr, sb_addr, s->baddr, is_write);

        if(func == 0x1 && s->mrvl) {
            g_autofree uint8_t *buf = g_malloc0(xfer_len);
            if (is_write) {
                sdio_dma(s, buf, xfer_len, false);
                mrvl8686_write(s->mrvl, addr, buf, xfer_len);
            } else {
                mrvl8686_read(s->mrvl, addr, buf, xfer_len);
                sdio_dma(s, buf, xfer_len, true);
            }
        }
        else if(is_write) {
            if(func == 0x1) {
                g_autofree uint8_t *buf = g_malloc(xfer_len);
                sdio_dma(s, buf, xfer_len, false);
                backplane_store(s, sb_addr, buf, xfer_len);
                /* Enough of a heartbeat to tell a running firmware download
                 * apart from a wedged one, without tracing every access. */
                s->fw_bytes += xfer_len;
                if(s->fw_bytes - s->fw_bytes_logged >= 65536) {
                    int64_t now = g_get_monotonic_time();
                    double secs = s->fw_last_us ? (now - s->fw_last_us) / 1e6 : 0;
                    s->fw_last_us = now;
                    s->fw_bytes_logged = s->fw_bytes;
                    trace_sdio("[SDIO] %u KiB written to the backplane (now at 0x%08x); "
                           "%.1fs for the last 64 KiB, %u register accesses "
                           "(%u of them polls of the interrupt status)\n",
                           s->fw_bytes >> 10, sb_addr, secs, s->mmio_ops, s->irq_polls);
                    s->mmio_ops = 0;
                    s->irq_polls = 0;
                }
            }
            else if(func == 0x2) {
                if(!s->func2_seen) {
                    s->func2_seen = true;
                    trace_sdio("[SDIO] first SDPCM frame on function 2 (%u bytes)\n", xfer_len);
                }
                g_autofree uint8_t *buf = g_malloc0(xfer_len);
                sdio_dma(s, buf, xfer_len, false);
                sdpcm_receive(s, buf, xfer_len);
            }
        } else {
            if(func == 0x1) {
                g_autofree uint8_t *buf = g_malloc(xfer_len);
                backplane_read(s, sb_addr, buf, xfer_len);
                sdio_dma(s, buf, xfer_len, true);
            }
            else if(func == 0x2) {
                /* Hand up one queued frame, zero-padded to whatever the host
                 * asked for. With nothing queued, a length of zero and its
                 * complement is the "no frame" answer. */
                /* A frame can be collected in more than one read - typically a
                 * first block to learn the length, then the remainder - so keep
                 * the head of the queue until it has all been handed over. */
                g_autofree uint8_t *buf = g_malloc0(xfer_len);
                SDPCMFrame *f = g_queue_peek_head(s->rx_fifo);
                if (s->func2_reads < 400) {
                    s->func2_reads++;
                    trace_sdio("[SDIO] function 2 read of %u bytes, %s", xfer_len,
                           f ? "handing up" : "nothing queued");
                    if (f) {
                        trace_sdio(" frame len %u chan %u, %u already taken",
                               f->len, f->data[5] & SDPCM_CHANNEL_MASK,
                               f->read_off);
                    }
                    trace_sdio("\n");
                }

                if (f) {
                    uint32_t left = f->len - f->read_off;
                    uint32_t n = MIN(left, xfer_len);
                    memcpy(buf, f->data + f->read_off, n);
                    f->read_off += n;
                    if (f->read_off >= f->len) {
                        g_queue_pop_head(s->rx_fifo);
                        g_free(f->data);
                        g_free(f);
                    }
                    if (g_queue_is_empty(s->rx_fifo)) {
                        /* Nothing left to collect. Leaving the frame
                         * indication set makes the driver read again, get a
                         * zero-length frame and hand it to its command
                         * manager, which then complains that no command is
                         * pending - forever. */
                        sdpcm_reg_write(s, SDPCM_INTSTATUS,
                                        sdpcm_reg_read(s, SDPCM_INTSTATUS) &
                                        ~I_HMB_FRAME_IND);
                    }
                }
                /* With nothing queued the buffer stays all zeros, which is how
                 * the dongle says "no more frames". A well-formed tag claiming
                 * a length of zero is a different thing entirely: the driver
                 * accepts it as a frame and hands the empty result to its
                 * command manager. */
                sdio_dma(s, buf, xfer_len, true);
            }
            
        }

        /* Bit 0 is transfer complete and bit 1 is the card's own interrupt.
         * They must accumulate: a control frame queued during this very
         * transfer sets bit 1, and overwriting it here loses the only
         * indication the driver has that a reply is waiting. */
        raise_irq_soon(s, 0x1);
        //trace_sdio("Raised IRQ\n");
    }
    else {
        /* An unimplemented command is a timeout on real hardware, not a dead
         * machine. Leave resp0 alone and let the driver time out. */
        qemu_log_mask(LOG_UNIMP, "[SDIO] unimplemented command %d (arg 0x%08x)\n",
                      cmd_type, s->arg);
    }
}

static void ipod_touch_sdio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    trace_sdio("%s: writing 0x%08x to 0x%08x\n", __func__, (uint32_t)value, (uint32_t)addr);

    ((IPodTouchSDIOState *)opaque)->mmio_ops++;
    
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
            /* The driver writes back the status word it just read, so this is
             * write-one-to-clear. Only drop the line once nothing is left. */
            s->irq_reg &= ~(uint32_t)value;
            if (!s->irq_reg) {
                qemu_irq_lower(s->irq);
            }
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

    ((IPodTouchSDIOState *)opaque)->mmio_ops++;
    if (addr == SDIO_IRQ) {
        ((IPodTouchSDIOState *)opaque)->irq_polls++;
    }

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

/*
 * "bssid": the access point's BSSID, "aa:bb:cc:dd:ee:ff", settable at run
 * time (qom-set). The host changes it with the Wi-Fi position it serves
 * (docs/ipad1/location.md): locationd caches a position per BSSID, so a new
 * position has to arrive as a new, unknown access point. Migrated (subsection,
 * only when not the default) so a restored guest stays on the AP it joined.
 */
static char *sdio_get_bssid(Object *obj, Error **errp)
{
    const uint8_t *b = IPOD_TOUCH_SDIO(obj)->fullmac.bssid;
    return g_strdup_printf("%02x:%02x:%02x:%02x:%02x:%02x",
                           b[0], b[1], b[2], b[3], b[4], b[5]);
}

static void sdio_set_bssid(Object *obj, const char *value, Error **errp)
{
    unsigned b[6];
    char tail;

    if (sscanf(value, "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4],
               &b[5], &tail) != 6 || (b[0] | b[1] | b[2] | b[3] | b[4] | b[5]) > 0xff) {
        error_setg(errp, "bssid must be six hex octets separated by colons");
        return;
    }
    for (int i = 0; i < 6; i++) {
        IPOD_TOUCH_SDIO(obj)->fullmac.bssid[i] = b[i];
    }
}

/*
 * "country": the access point's 802.11d country, two letters ("US", the
 * default). Light Touch sets the Mac's region, where the Mac and so the
 * network it shares are.
 */
static char *sdio_get_country(Object *obj, Error **errp)
{
    return g_strdup(IPOD_TOUCH_SDIO(obj)->fullmac.country);
}

static void sdio_set_country(Object *obj, const char *value, Error **errp)
{
    if (strlen(value) != 2 || !g_ascii_isalpha(value[0]) || !g_ascii_isalpha(value[1])) {
        error_setg(errp, "country must be two letters (an ISO 3166 code)");
        return;
    }
    IPOD_TOUCH_SDIO(obj)->fullmac.country[0] = g_ascii_toupper(value[0]);
    IPOD_TOUCH_SDIO(obj)->fullmac.country[1] = g_ascii_toupper(value[1]);
}

/* DAT1 from a card that signals its own interrupt (the Marvell): the
 * controller latches the card-interrupt status bit on the edge. */
static void sdio_card_irq(void *opaque, int n, int level)
{
    IPodTouchSDIOState *s = opaque;
    trace_sdio("[SDIO] card irq %d (was %d, irq_reg 0x%x mask 0x%x)\n", level, s->card_irq_level, s->irq_reg, s->irq_mask);
    if (level && !s->card_irq_level) {
        raise_irq_soon(s, 0x2);
    }
    s->card_irq_level = level;
}

static void ipod_touch_sdio_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    IPodTouchSDIOState *s = IPOD_TOUCH_SDIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* The iPod touch 2G's BCM4325; the iPad swaps in its 4329. */
    static const BCMSDIOChip bcm4325 = {
        .manfid = BCM4325_MANUFACTURER, .prodid = BCM4325_PRODUCT_ID,
        .chipid = CHIPCOMMON_CHIPID, .sdiod_base = SDPCM_CORE_BASE,
        /* Matches wifiaddr in the stock n72ap NOR's nvram. */
        .mac = { 0x00, 0x23, 0x32, 0x6e, 0xaa, 0x10 },
    };

    s->backplane = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    s->sb_window = CHIPCOMMON_BASE;
    bcm_fullmac_init(&s->fullmac, sdio_fullmac_send,
                     sdio_fullmac_can_receive, s);
    object_property_add_str(obj, "bssid", sdio_get_bssid, sdio_set_bssid);
    object_property_add_str(obj, "country", sdio_get_country, sdio_set_country);
    ipod_touch_sdio_set_chip(s, &bcm4325);

    qdev_init_gpio_in_named(dev, sdio_card_irq, "card-irq", 1);
    memory_region_init_io(&s->iomem, obj, &ipod_touch_sdio_ops, s, TYPE_IPOD_TOUCH_SDIO, 4096);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, trigger_irq, s);

    s->rx_fifo = g_queue_new();
}

/*
 * A machine reset is a power cycle for the card: the dongle's RAM, the SDIO
 * core and the SDPCM session go. Without this a rebooted guest finds a card
 * that claims its firmware is already up, never completes a download, and
 * AppleBCMWLAN panics after its 60 s watchdog.
 */
static void ipod_touch_sdio_reset(DeviceState *dev)
{
    IPodTouchSDIOState *s = IPOD_TOUCH_SDIO(dev);
    SDPCMFrame *f;

    /*
     * The card is its own chip on its own rail: hibernating the AP leaves it
     * powered, and 3.1.3's AppleBCM4325 talks to it again on wake without a
     * firmware download. ponytail: the host controller's registers are kept
     * too; the kernel reprograms them as it resumes.
     */
    if (s->ap_waking) {
        s->ap_waking = false;
        return;
    }
    s->cmd = s->arg = s->state = s->stac = s->csr = 0;
    s->resp0 = s->resp1 = s->resp2 = s->resp3 = 0;
    s->irq_reg = s->irq_pending = s->irq_mask = 0;
    s->baddr = s->blklen = s->numblk = 0;
    qemu_irq_lower(s->irq);
    timer_del(s->irq_timer);
    bcm_fullmac_reset(&s->fullmac);
    while ((f = g_queue_pop_head(s->rx_fifo))) {
        g_free(f->data);
        g_free(f);
    }
    g_hash_table_remove_all(s->backplane);
    s->sb_window = CHIPCOMMON_BASE;
    s->fw_bytes = s->fw_bytes_logged = 0;
    s->func2_seen = false;
    s->tx_seq = s->rx_seq = 0;
    memset(s->sdiod_regs, 0, sizeof(s->sdiod_regs));
    memset(s->registers, 0, sizeof(s->registers));
    ipod_touch_sdio_set_chip(s, &s->chip);
    s->card_irq_level = false;
    if (s->mrvl) {
        mrvl8686_card_reset(s->mrvl);
    }
}

#include "ipod-sdio-state.h"

static const VMStateInfo vmstate_sdio_backplane = {
    .name = "sdio-backplane", .get = sdio_get_backplane, .put = sdio_put_backplane,
};
static const VMStateInfo vmstate_sdio_frames = {
    .name = "sdio-frames", .get = sdio_get_frames, .put = sdio_put_frames,
};

static int sdio_post_load(void *opaque, int version_id)
{
    IPodTouchSDIOState *s = opaque;
    if ((s->fullmac.cdc_hdrlen && s->fullmac.cdc_hdrlen != 12 && s->fullmac.cdc_hdrlen != 16) ||
        (s->fullmac.bdc_hdrlen && s->fullmac.bdc_hdrlen != 4 && s->fullmac.bdc_hdrlen != 6)) return -EINVAL;
    if (s->fullmac.escan_pending &&
        s->fullmac.escan_status != 0 && s->fullmac.escan_status != ESCAN_STATUS_ABORT) {
        return -EINVAL;
    }
    qemu_set_irq(s->irq, s->irq_reg != 0);
    return 0;
}

static bool sdio_bssid_needed(void *opaque)
{
    return memcmp(((IPodTouchSDIOState *)opaque)->fullmac.bssid, bcm_fullmac_default_bssid, sizeof(bcm_fullmac_default_bssid));
}

static const VMStateDescription vmstate_sdio_bssid = {
    .name = TYPE_IPOD_TOUCH_SDIO "/bssid",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sdio_bssid_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(fullmac.bssid, IPodTouchSDIOState, 6),
        VMSTATE_END_OF_LIST()
    },
};

static bool sdio_card_irq_needed(void *opaque)
{
    return ((IPodTouchSDIOState *)opaque)->mrvl != NULL;
}

static const VMStateDescription vmstate_sdio_card_irq = {
    .name = TYPE_IPOD_TOUCH_SDIO "/card-irq",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sdio_card_irq_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(card_irq_level, IPodTouchSDIOState),
        VMSTATE_END_OF_LIST()
    },
};

/* Only a machine saved before the host attached its interface carries this;
 * a missing subsection (and every snapshot older than the field) reads as a
 * host that is up, which is what a snapshot of a running guest is. */
static int sdio_pre_load(void *opaque)
{
    IPodTouchSDIOState *s = opaque;
    s->fullmac.host_netif = true;
    s->fullmac.escan_pending = false;
    s->fullmac.escan_sync_id = 0;
    s->fullmac.escan_status = 0;
    return 0;
}

static bool sdio_netif_needed(void *opaque)
{
    return !((IPodTouchSDIOState *)opaque)->fullmac.host_netif;
}

static const VMStateDescription vmstate_sdio_netif = {
    .name = TYPE_IPOD_TOUCH_SDIO "/host_netif",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sdio_netif_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(fullmac.host_netif, IPodTouchSDIOState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sdio_toe_needed(void *opaque)
{
    IPodTouchSDIOState *s = opaque;
    return s->fullmac.toe || s->fullmac.toe_ol;
}

static const VMStateDescription vmstate_sdio_toe = {
    .name = TYPE_IPOD_TOUCH_SDIO "/toe",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sdio_toe_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(fullmac.toe, IPodTouchSDIOState),
        VMSTATE_UINT32(fullmac.toe_ol, IPodTouchSDIOState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sdio_escan_needed(void *opaque)
{
    return ((IPodTouchSDIOState *)opaque)->fullmac.escan_pending;
}

static const VMStateDescription vmstate_sdio_escan = {
    .name = TYPE_IPOD_TOUCH_SDIO "/escan",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sdio_escan_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(fullmac.escan_pending, IPodTouchSDIOState),
        VMSTATE_UINT16(fullmac.escan_sync_id, IPodTouchSDIOState),
        VMSTATE_UINT32(fullmac.escan_status, IPodTouchSDIOState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_ipod_touch_sdio = {
    .name = TYPE_IPOD_TOUCH_SDIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = sdio_pre_load,
    .post_load = sdio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmd, IPodTouchSDIOState),
        VMSTATE_UINT32(arg, IPodTouchSDIOState),
        VMSTATE_UINT32(state, IPodTouchSDIOState),
        VMSTATE_UINT32(stac, IPodTouchSDIOState),
        VMSTATE_UINT32(csr, IPodTouchSDIOState),
        VMSTATE_UINT32(resp0, IPodTouchSDIOState),
        VMSTATE_UINT32(resp1, IPodTouchSDIOState),
        VMSTATE_UINT32(resp2, IPodTouchSDIOState),
        VMSTATE_UINT32(resp3, IPodTouchSDIOState),
        VMSTATE_UINT32(irq_reg, IPodTouchSDIOState),
        VMSTATE_UINT32(irq_pending, IPodTouchSDIOState),
        VMSTATE_UINT32(irq_mask, IPodTouchSDIOState),
        VMSTATE_UINT32(baddr, IPodTouchSDIOState),
        VMSTATE_UINT32(blklen, IPodTouchSDIOState),
        VMSTATE_UINT32(numblk, IPodTouchSDIOState),
        VMSTATE_UINT32(sb_window, IPodTouchSDIOState),
        VMSTATE_UINT32(fullmac.cdc_hdrlen, IPodTouchSDIOState),
        VMSTATE_UINT32(fullmac.bdc_hdrlen, IPodTouchSDIOState),
        VMSTATE_UINT8(tx_seq, IPodTouchSDIOState),
        VMSTATE_UINT8(rx_seq, IPodTouchSDIOState),
        VMSTATE_BOOL(card_present, IPodTouchSDIOState),
        VMSTATE_BOOL(fullmac.dongle_started, IPodTouchSDIOState),
        VMSTATE_BOOL(fullmac.associated, IPodTouchSDIOState),
        VMSTATE_BOOL(func2_seen, IPodTouchSDIOState),
        VMSTATE_BOOL(fullmac.iscan_reported, IPodTouchSDIOState),
        VMSTATE_UINT8_ARRAY(sdiod_regs, IPodTouchSDIOState, SDIOD_CORE_SIZE),
        VMSTATE_UINT8_ARRAY(registers, IPodTouchSDIOState, 0x20000),
        VMSTATE_TIMER_PTR(irq_timer, IPodTouchSDIOState),
        VMSTATE_TIMER_PTR(fullmac.scan_timer, IPodTouchSDIOState),
        VMSTATE_TIMER_PTR(fullmac.join_timer, IPodTouchSDIOState),
        VMSTATE_SINGLE(backplane, IPodTouchSDIOState, 1, vmstate_sdio_backplane, GHashTable *),
        VMSTATE_SINGLE(rx_fifo, IPodTouchSDIOState, 1, vmstate_sdio_frames, GQueue *),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sdio_bssid,
        &vmstate_sdio_card_irq,
        &vmstate_sdio_netif,
        &vmstate_sdio_toe,
        &vmstate_sdio_escan,
        NULL
    },
};

static const Property ipod_touch_sdio_props[] = {
    DEFINE_PROP_LINK("mrvl", IPodTouchSDIOState, mrvl, TYPE_MRVL8686, Mrvl8686State *),
};

static void ipod_touch_sdio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, ipod_touch_sdio_props);
    dc->vmsd = &vmstate_ipod_touch_sdio;
    device_class_set_legacy_reset(dc, ipod_touch_sdio_reset);
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
