#include "hw/arm/ipod_touch_sdio.h"
#include "qemu/log.h"

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
    backplane_read(s, SDPCM_CORE_BASE + off, buf, sizeof(buf));
    return ldl_le_p(buf);
}

static void sdpcm_reg_write(IPodTouchSDIOState *s, uint32_t off, uint32_t val)
{
    uint8_t buf[4];
    stl_le_p(buf, val);
    backplane_write(s, SDPCM_CORE_BASE + off, buf, sizeof(buf));
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
    if (s->dongle_started) {
        return;
    }
    s->dongle_started = true;
    sdpcm_reg_write(s, SDPCM_TOHOSTMAILBOXDATA,
                    HMB_DATA_DEVREADY | HMB_DATA_FWREADY |
                    (SDPCM_PROT_VERSION << HMB_DATA_VERSION_SHIFT));
    printf("[SDIO] dongle announced ready\n");
    sdpcm_raise(s, I_HMB_HOST_INT);
}

static void sdpcm_queue_frame(IPodTouchSDIOState *s, uint8_t *data, uint32_t len)
{
    SDPCMFrame *f = g_new0(SDPCMFrame, 1);
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
 * How much BDC header this driver expects in front of an 802.3 frame. Nothing
 * is known until the first control command has been seen, and the only frames
 * that can arrive before that are ones the model itself sends after the driver
 * has talked to it, so the 2.1.1 default is never actually used blind.
 */
static unsigned sdio_bdc_hdrlen(IPodTouchSDIOState *s)
{
    return s->bdc_hdrlen ? s->bdc_hdrlen : BDC_HDRLEN;
}

/*
 * Deliver a firmware event. The frame is an ordinary 802.3 packet on the data
 * channel whose ethertype the driver recognises; everything from the bcmeth
 * header inwards is big endian.
 *
 * The layout is pinned by what handleEventPacket checks: three OUI bytes at
 * packet offset 0x13 and a usr_subtype of 1 at 0x16, with the event message
 * itself at 0x18.
 */
static void sdpcm_send_event(IPodTouchSDIOState *s, uint32_t event_type,
                             uint32_t status, uint16_t msg_flags)
{
    static const uint8_t our_mac[6] = { 0x00, 0x23, 0x32, 0x6e, 0xaa, 0x10 };
    unsigned bdclen = sdio_bdc_hdrlen(s);
    uint8_t frame[BDC_MAX_HDRLEN + 14 + 10 + WL_EVENT_MSG_LEN];
    uint32_t framelen = bdclen + 14 + 10 + WL_EVENT_MSG_LEN;
    uint8_t *p = frame;

    memset(frame, 0, sizeof(frame));

    /* BDC header: four bytes, plus the two of padding 2.1.1's driver skips. */
    *p++ = BDC_PROTO_VER << 4;
    *p++ = 0;                       /* priority */
    *p++ = 0;                       /* flags2 */
    *p++ = 0;                       /* data offset, in 4-byte words */
    for (unsigned i = BDC_HDRLEN_STD; i < bdclen; i++) {
        *p++ = 0;
    }

    uint8_t *eth = p;
    memcpy(p, our_mac, 6); p += 6;  /* destination: us */
    memset(p, 0, 6); p += 6;        /* source: the dongle itself */
    stw_be_p(p, ETHER_TYPE_BRCM); p += 2;

    /* bcmeth header. */
    stw_be_p(p, BCMETH_SUBTYPE); p += 2;
    uint8_t *lenp = p; p += 2;      /* filled in below */
    *p++ = 0;                       /* version */
    *p++ = BCMETH_OUI_0;
    *p++ = BCMETH_OUI_1;
    *p++ = BCMETH_OUI_2;
    stw_be_p(p, BCMETH_USR_SUBTYPE); p += 2;

    /* The length field counts everything from the version byte onwards. */
    stw_be_p(lenp, (frame + framelen) - (lenp + 2));

    /* wl_event_msg, all big endian. */
    stw_be_p(p + 0x00, 1);          /* version */
    stw_be_p(p + 0x02, msg_flags);
    stl_be_p(p + 0x04, event_type);
    stl_be_p(p + 0x08, status);
    stl_be_p(p + 0x0c, 0);          /* reason */
    stl_be_p(p + 0x10, 0);          /* auth type */
    stl_be_p(p + 0x14, 0);          /* datalen: no payload follows */
    memcpy(p + 0x18, our_mac, 6);
    g_strlcpy((char *)p + 0x1e, "en0", 16);

    /* The driver sees the frame from the ethernet header onwards, so its
     * offsets 0x13 and 0x16 are relative to eth, not to the BDC header. */
    assert(eth[0x13] == BCMETH_OUI_0 && eth[0x16] == 0 && eth[0x17] == 1);
    printf("[SDIO] sending event %u, status %u, flags 0x%04x (%u byte frame)\n",
           event_type, status, msg_flags, framelen);

    /*
     * 2.1.1's driver dispatches receive by channel and answers channel 1 with
     * "WTF?? Got an event packet!!!" before dropping it, so its events have to
     * arrive on the data channel and be recognised by their ethertype.
     * 3.1.3's rxPacket has a real sixteen entry jump table on the channel
     * (VA 0xc07191d6): 0 control, 1 event, 2 data, 3 superframe, 15, and
     * "ignore" for the rest - and its handleDataPacket has no ethertype test
     * for BRCM frames at all, so an event on channel 2 is dropped in silence.
     */
    sdpcm_send(s, bdclen == BDC_HDRLEN_STD ? SDPCM_EVENT_CHANNEL
                                           : SDPCM_DATA_CHANNEL,
               frame, framelen);
}

/* The BSSID and SSID the model pretends to be associated with. */
static const uint8_t fake_bssid[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x01 };
#define FAKE_SSID "qemu-ios"

/* Describe the network the model claims to be on, open and on channel 6. */
static void fill_bss_info(uint8_t *bi)
{
    static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96 };

    stl_le_p(bi + BSS_INFO_OFF_VERSION, BSS_INFO_VERSION);
    stl_le_p(bi + BSS_INFO_OFF_LENGTH, BSS_INFO_TOTAL);
    memcpy(bi + BSS_INFO_OFF_BSSID, fake_bssid, sizeof(fake_bssid));
    stw_le_p(bi + BSS_INFO_OFF_BEACON, 100);
    stw_le_p(bi + BSS_INFO_OFF_CAPABILITY, BSS_CAP_ESS);   /* open: no privacy */
    bi[BSS_INFO_OFF_SSID_LEN] = strlen(FAKE_SSID);
    memcpy(bi + BSS_INFO_OFF_SSID, FAKE_SSID, strlen(FAKE_SSID));
    stl_le_p(bi + BSS_INFO_OFF_RATE_COUNT, sizeof(rates));
    memcpy(bi + BSS_INFO_OFF_RATES, rates, sizeof(rates));
    bi[BSS_INFO_OFF_CHANNEL] = 6;
    stw_le_p(bi + BSS_INFO_OFF_RSSI, (uint16_t)(int16_t)-45);
    bi[BSS_INFO_OFF_PHY_NOISE] = (uint8_t)(int8_t)-92;

    /*
     * The fixed SSID and channel fields on their own leave the driver logging
     * an empty SSID and channel 0, so the beacon object is reading the
     * information elements that follow the structure instead. Append the three
     * a real beacon would start with.
     */
    stw_le_p(bi + BSS_INFO_OFF_IE_OFFSET, BSS_INFO_LEN);
    stl_le_p(bi + BSS_INFO_OFF_IE_LENGTH, BSS_INFO_IE_LEN);

    uint8_t *ie = bi + BSS_INFO_LEN;
    *ie++ = 0x00;                                  /* SSID */
    *ie++ = strlen(FAKE_SSID);
    memcpy(ie, FAKE_SSID, strlen(FAKE_SSID)); ie += strlen(FAKE_SSID);
    *ie++ = 0x01;                                  /* supported rates */
    *ie++ = sizeof(rates);
    memcpy(ie, rates, sizeof(rates)); ie += sizeof(rates);
    *ie++ = 0x03;                                  /* DS parameter set */
    *ie++ = 0x01;
    *ie++ = 6;                                     /* channel */
}

/*
 * Answer get_var iscanresults with one real BSS - the same network
 * fill_bss_info describes, so a scan result and a later WLC_GET_BSS_INFO agree.
 *
 * status has to be SUCCESS rather than PARTIAL or the scan manager keeps
 * polling, and version/buflen/count all have to be consistent: a zeroed reply
 * decodes as a successful scan that found nothing, which is what left the
 * network list spinning.
 */
static void fill_iscan_results(IPodTouchSDIOState *s, uint8_t *p)
{
    /*
     * One BSS, reported as a completed scan. Answering PARTIAL instead - which
     * is what an incremental scan would really do, and which handleIScanResult
     * has a branch for - wedges Preferences the moment the Wi-Fi pane opens:
     * the driver never comes back for the second batch, so the pane hangs
     * mid-push. SUCCESS in one shot is the sequence this build tolerates.
     */
    s->iscan_reported = true;
    stl_le_p(p + ISCAN_OFF_STATUS, WL_SCAN_RESULTS_SUCCESS);
    stl_le_p(p + ISCAN_OFF_BUFLEN, ISCAN_RESULTS_FIXED + BSS_INFO_TOTAL);
    stl_le_p(p + ISCAN_OFF_VERSION, BSS_INFO_VERSION);
    stl_le_p(p + ISCAN_OFF_COUNT, 1);
    fill_bss_info(p + ISCAN_OFF_BSS);
    printf("[SDIO] iscanresults: 1 BSS, scan complete\n");
}

/*
 * A real scan takes seconds of radio time. Reporting it complete in the same
 * breath as the request arrives means the event can overtake the driver's own
 * bookkeeping for the scan it just started, so the completion is armed on a
 * timer instead.
 */
static void sdio_scan_complete(void *opaque)
{
    IPodTouchSDIOState *s = (IPodTouchSDIOState *)opaque;

    sdpcm_send_event(s, WLC_E_SCAN_COMPLETE, 0, 0);
}

/*
 * The event chain a dongle reports on its way into a BSS. The driver's own
 * dispatch table decides what it believes: WLC_E_SET_SSID with status 0 is a
 * successful join, WLC_E_LINK with the link flag set is the carrier coming up.
 */
static void sdio_send_assoc_events(IPodTouchSDIOState *s)
{
    sdpcm_send_event(s, WLC_E_AUTH, 0, 0);
    sdpcm_send_event(s, WLC_E_ASSOC, 0, 0);
    sdpcm_send_event(s, WLC_E_SET_SSID, 0, 0);
    sdpcm_send_event(s, WLC_E_LINK, 0, WLC_EVENT_MSG_LINK);
}

/*
 * A real join. The driver picked this network out of the scan results and is
 * asking the dongle to associate with it; reporting the association back as
 * events is what actual hardware does, so the driver drives its own state
 * machine and nothing has to be asserted on a timer.
 */
static void sdio_handle_set_ssid(IPodTouchSDIOState *s, const uint8_t *payload,
                                 uint32_t len)
{
    char ssid[WLC_SSID_MAX + 1] = { 0 };
    uint32_t n = 0;

    if (len >= 4) {
        n = ldl_le_p(payload);
        n = MIN(n, MIN(len - 4, WLC_SSID_MAX));
        memcpy(ssid, payload + 4, n);
    }

    /* A zero-length SSID is a disassociate request, not a join. */
    if (n == 0) {
        printf("[SDIO] WLC_SET_SSID with no SSID: leaving the network\n");
        sdpcm_send_event(s, WLC_E_LINK, 0, 0);
        return;
    }

    printf("[SDIO] WLC_SET_SSID '%s': associating\n", ssid);
    sdio_send_assoc_events(s);
}

/*
 * Answer one CDC control request. Everything is acknowledged with a zeroed
 * payload for now; the point is to get the driver past initDongle and to see
 * in the log which commands it actually depends on.
 */
/*
 * How long this build's CDC header is, latched from the request.
 *
 * The driver always sends the whole buffer it is asking about - a get_var
 * carries the iovar name in a payload_len sized buffer - so the bytes between
 * the start of the CDC header and that payload are the header itself.
 * AppleBCM4325 (2.1.1) gives twelve, AppleBCMWLAN (3.1.3) sixteen. Anything
 * else is a request that did not carry its whole buffer, so it is ignored and
 * whatever was latched before stands; a running dongle does not change form.
 */
static unsigned cdc_hdrlen(IPodTouchSDIOState *s, uint32_t len,
                           uint32_t payload_len)
{
    if (len >= payload_len) {
        uint32_t hdr = len - payload_len;
        if (hdr == CDC_HDRLEN || hdr == CDC_HDRLEN_STATUS) {
            if (s->cdc_hdrlen != hdr) {
                s->cdc_hdrlen = hdr;
                /* The two headers moved together: the driver that wants the
                 * status word is the one that skips four bytes of BDC. */
                s->bdc_hdrlen = hdr == CDC_HDRLEN_STATUS ? BDC_HDRLEN_STD
                                                         : BDC_HDRLEN;
                printf("[SDIO] CDC header is %u bytes, BDC header is %u\n",
                       s->cdc_hdrlen, s->bdc_hdrlen);
            }
        }
    }
    return s->cdc_hdrlen ? s->cdc_hdrlen : CDC_HDRLEN;
}

static void sdpcm_handle_cdc(IPodTouchSDIOState *s, const uint8_t *cdc,
                             uint32_t len)
{
    uint32_t cmd = ldl_le_p(cdc);
    uint32_t payload_len = ldl_le_p(cdc + 4);
    uint32_t flags = ldl_le_p(cdc + 8);
    unsigned hdrlen = cdc_hdrlen(s, len, payload_len);

    /* WLC_GET_VAR and WLC_SET_VAR carry a NUL-terminated iovar name at the
     * start of the payload, which is the only way to tell one from another. */
    const char *iovar = "";
    if ((cmd == WLC_GET_VAR || cmd == WLC_SET_VAR) && len > hdrlen) {
        const char *p = (const char *)cdc + hdrlen;
        size_t max = len - hdrlen;
        if (memchr(p, 0, max)) {
            iovar = p;
        }
    }
    printf("[SDIO] CDC command %u (%s), %u bytes, flags 0x%08x%s%s\n", cmd,
           (flags & CDC_DCMD_SET) ? "set" : "get", payload_len, flags,
           *iovar ? " iovar " : "", iovar);

    /*
     * On a set, the payload is what arrived and cannot be longer than the
     * frame. On a get it is the size of the buffer the driver is waiting to
     * have filled, and the reply has to be exactly that long: the command
     * manager asserts on it (AppleBCM4325CmdManager.cpp:213) and then reads
     * its own uninitialised buffer. Clamping both ways answered every get
     * with a few bytes - a 1148 byte WLC_GET_BSS_INFO came back as zero - and
     * that is why the joined BSS had a garbage BSSID and an empty SSID.
     */
    if (flags & CDC_DCMD_SET) {
        if (payload_len > len - hdrlen) {
            payload_len = len > hdrlen ? len - hdrlen : 0;
        }
    } else if (payload_len > CDC_MAX_PAYLOAD) {
        payload_len = CDC_MAX_PAYLOAD;
    }

    /* The length field is checked against what the command expects, not
     * against the request - AppleBCM4325CmdManager.cpp:445 asserts on it. */
    g_autofree uint8_t *reply = g_malloc0(hdrlen + payload_len);
    stl_le_p(reply, cmd);
    stl_le_p(reply + 4, payload_len);
    stl_le_p(reply + 8, flags & ~CDC_DCMD_ERROR);  /* same id, no error */
    if (hdrlen >= CDC_HDRLEN_STATUS) {
        stl_le_p(reply + CDC_OFF_STATUS, 0);       /* the command succeeded */
    }

    /* A few gets have to carry something believable once the model claims to
     * be associated - an all-zero BSSID reads as "not associated". */
    if (!(flags & CDC_DCMD_SET)) {
        if (cmd == WLC_GET_BSS_INFO && payload_len >= 4 + BSS_INFO_TOTAL) {
            /* Four byte buffer length, then the structure itself. */
            stl_le_p(reply + hdrlen, BSS_INFO_TOTAL);
            fill_bss_info(reply + hdrlen + 4);
        } else if (cmd == WLC_GET_BSSID && payload_len >= sizeof(fake_bssid)) {
            memcpy(reply + hdrlen, fake_bssid, sizeof(fake_bssid));
        } else if (cmd == WLC_GET_RSSI && payload_len >= 4) {
            /* Polled to drive the status bar's signal bars; zero reads as no
             * signal, which is why the icon showed empty. */
            stl_le_p(reply + hdrlen, (uint32_t)(int32_t)-45);
        } else if (cmd == WLC_GET_RATE && payload_len >= 4) {
            stl_le_p(reply + hdrlen, 108);   /* 54 Mbit, in 500 kbit units */
        } else if (cmd == WLC_GET_VAR && g_str_equal(iovar, "iscanresults") &&
                   payload_len >= ISCAN_TOTAL) {
            fill_iscan_results(s, reply + hdrlen);
        } else if (cmd == WLC_GET_SSID && payload_len >= 4) {
            /* wlc_ssid_t: a length word then up to 32 bytes. */
            uint32_t n = MIN(strlen(FAKE_SSID), payload_len - 4);
            stl_le_p(reply + hdrlen, n);
            memcpy(reply + hdrlen + 4, FAKE_SSID, n);
        }
    }

    sdpcm_send(s, SDPCM_CONTROL_CHANNEL, reply, hdrlen + payload_len);

    /* The driver arms a scan and then waits: it re-sends this every fifteen
     * seconds and never asks for results on its own. Tell it the scan is over
     * so it comes back for them. */
    if (cmd == WLC_SET_VAR && g_str_equal(iovar, "iscan")) {
        s->iscan_reported = false;
        if (s->scan_timer) {
            timer_mod(s->scan_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      SCAN_COMPLETE_DELAY_NS);
        }
    }

    /* The driver joining a network it found in the scan results. */
    if ((flags & CDC_DCMD_SET) && cmd == WLC_SET_SSID) {
        sdio_handle_set_ssid(s, cdc + hdrlen,
                             len > hdrlen ? len - hdrlen : 0);
    }

}

/*
 * Store into the backplane, giving the few registers that are not plain memory
 * their behaviour. Everything else is backed by the page store so the driver's
 * read-back verification passes.
 */
static void backplane_store(IPodTouchSDIOState *s, uint32_t sb_addr,
                            const uint8_t *buf, uint32_t len)
{
    bool in_core = sb_addr >= SDPCM_CORE_BASE &&
                   sb_addr < SDPCM_CORE_BASE + SDPCM_CORE_SIZE;
    uint32_t off = sb_addr - SDPCM_CORE_BASE;

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
    } else if (in_core && off == SDPCM_HOSTINTMASK) {
        /* The mask arrives after the core is started, so re-evaluate. */
        sdpcm_announce_ready(s);
        sdpcm_raise(s, sdpcm_reg_read(s, SDPCM_INTSTATUS) & ldl_le_p(buf));
    } else if (in_core && off == SDPCM_TOSBMAILBOX && len >= 4 &&
               (ldl_le_p(buf) & SMB_INT_ACK)) {
        sdpcm_reg_write(s, SDPCM_INTSTATUS,
                        sdpcm_reg_read(s, SDPCM_INTSTATUS) & ~I_HMB_HOST_INT);
    }
}

/* --- the 802.3 data path ------------------------------------------------ */

static void hexdump_frame(const char *what, const uint8_t *p, uint32_t len)
{
    printf("[SDIO] %s (%u bytes):", what, len);
    for (unsigned i = 0; i < MIN(len, 42u); i++) {
        printf(" %02x", p[i]);
    }
    printf("\n");
}

/*
 * A data frame the driver handed down. The BDC header is four bytes plus
 * dataOffset four-byte words of padding; the driver's own receive path skips
 * six, so a dataOffset of zero would be four here and does not have to match.
 * Both are accepted, and the ethertype is used to tell which was meant.
 */
static void sdio_tx_data(IPodTouchSDIOState *s, const uint8_t *payload,
                         uint32_t len)
{
    if (len < BDC_HDRLEN_STD + 14) {
        printf("[SDIO] data frame too short to be 802.3: %u bytes\n", len);
        return;
    }

    uint32_t hdrlen = 4 + 4 * payload[3];
    /* Pick whichever offset leaves a plausible ethernet header. A frame from
     * the guest is either ARP, IPv4 or IPv6 in practice. */
    static const uint32_t candidates[] = { 4, 6, 8 };
    uint32_t chosen = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(candidates); i++) {
        uint32_t off = candidates[i];
        if (off + 14 > len) {
            continue;
        }
        uint16_t et = lduw_be_p(payload + off + 12);
        if (et == 0x0806 || et == 0x0800 || et == 0x86dd || et == 0x888e) {
            chosen = off;
            break;
        }
    }
    if (!chosen) {
        chosen = MIN(hdrlen, len - 14);
    }

    if (s->tx_log < 24) {
        s->tx_log++;
        printf("[SDIO] guest -> host, bdc %u bytes (dataOffset byte %u), "
               "ethertype 0x%04x\n", chosen, payload[3],
               lduw_be_p(payload + chosen + 12));
        hexdump_frame("guest frame", payload + chosen, len - chosen);
    }

    if (!s->nic) {
        return;
    }
    qemu_send_packet(qemu_get_queue(s->nic), payload + chosen, len - chosen);
}

/* A frame from the network backend, wrapped for the driver's receive path. */
static ssize_t sdio_net_receive(NetClientState *nc, const uint8_t *buf,
                                size_t size)
{
    IPodTouchSDIOState *s = qemu_get_nic_opaque(nc);

    if (!s->dongle_started || size < 14 || size > 1600) {
        return size;
    }

    unsigned bdclen = sdio_bdc_hdrlen(s);
    g_autofree uint8_t *frame = g_malloc0(bdclen + size);
    frame[0] = BDC_PROTO_VER << 4;
    frame[1] = 0;                    /* priority */
    frame[2] = 0;                    /* flags2 */
    frame[3] = 0;                    /* dataOffset */
    memcpy(frame + bdclen, buf, size);

    if (s->host_rx_log < 24) {
        s->host_rx_log++;
        hexdump_frame("host -> guest", buf, size);
    }

    sdpcm_send(s, SDPCM_DATA_CHANNEL, frame, bdclen + size);
    return size;
}

static bool sdio_net_can_receive(NetClientState *nc)
{
    IPodTouchSDIOState *s = qemu_get_nic_opaque(nc);

    /* Before the dongle is up there is nothing to hand a frame to, and an
     * unbounded queue of them would be handed over all at once later. */
    return s->dongle_started && g_queue_get_length(s->rx_fifo) < 16;
}

static NetClientInfo sdio_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = sdio_net_can_receive,
    .receive = sdio_net_receive,
};

/*
 * Bind to a -netdev by id. The device is created by the board rather than by
 * the user, so there is no "netdev" property to set on the command line; the
 * id is fixed and the absence of a backend is not an error.
 */
void ipod_touch_sdio_setup_net(IPodTouchSDIOState *s)
{
    static const uint8_t wlan_mac[6] = { 0x00, 0x23, 0x32, 0x6e, 0xaa, 0x10 };
    NetClientState *peer = qemu_find_netdev("wifi0");

    if (!peer) {
        return;
    }
    memcpy(s->conf.macaddr.a, wlan_mac, sizeof(wlan_mac));
    s->conf.peers.ncs[0] = peer;
    s->conf.peers.queues = 1;
    s->nic = qemu_new_nic(&sdio_net_info, &s->conf, TYPE_IPOD_TOUCH_SDIO,
                          "wifi", &DEVICE(s)->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    printf("[SDIO] network backend 'wifi0' attached\n");
}

/* One frame written by the host on function 2. */
static void sdpcm_receive(IPodTouchSDIOState *s, const uint8_t *buf, uint32_t len)
{
    if (s->rx_log < 16) {
        s->rx_log++;
        printf("[SDIO] host frame (%u bytes):", len);
        for (unsigned i = 0; i < MIN(len, 24u); i++) {
            printf(" %02x", buf[i]);
        }
        printf("\n");
    }

    if (len < SDPCM_HDRLEN) {
        return;
    }

    uint16_t framelen = lduw_le_p(buf);
    uint16_t check = lduw_le_p(buf + 2);
    if ((framelen ^ check) != 0xffff) {
        printf("[SDIO] SDPCM frame with a bad tag: len %u check %04x\n",
               framelen, check);
        return;
    }

    uint8_t channel = buf[5] & SDPCM_CHANNEL_MASK;
    uint8_t doff = buf[7];
    s->rx_seq = buf[4];

    if (doff < SDPCM_HDRLEN || doff > framelen || framelen > len) {
        printf("[SDIO] SDPCM frame with an unusable offset: doff %u len %u\n",
               doff, framelen);
        return;
    }

    switch (channel) {
    case SDPCM_CONTROL_CHANNEL:
        if (framelen - doff >= CDC_HDRLEN) {
            sdpcm_handle_cdc(s, buf + doff, framelen - doff);
        }
        break;
    case SDPCM_DATA_CHANNEL:
        sdio_tx_data(s, buf + doff, framelen - doff);
        break;
    default:
        printf("[SDIO] SDPCM frame on unhandled channel %u\n", channel);
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
    if (!s->dongle_started || s->ready_log >= 6000) {
        return;
    }
    s->ready_log++;
    printf("[SDIO] %s func %u %s addr 0x%05x len %u\n", what,
           func, is_write ? "write" : "read", addr, len);
}

void sdio_exec_cmd(IPodTouchSDIOState *s)
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
                backplane_store(s, backplane_addr(s, addr), &byte, 1);
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
        trace_sdio("SDIO: Executing cmd53 func %x with block size %d and %d blocks (reg address: 0x%08x, backplane address: 0x%08x, destination address: 0x%08x, write? %d)\n", func, s->blklen, s->numblk, addr, sb_addr, s->baddr, is_write);

        if(is_write) {
            if(func == 0x1) {
                g_autofree uint8_t *buf = g_malloc(xfer_len);
                cpu_physical_memory_read(s->baddr, buf, xfer_len);
                backplane_store(s, sb_addr, buf, xfer_len);
                /* Enough of a heartbeat to tell a running firmware download
                 * apart from a wedged one, without tracing every access. */
                s->fw_bytes += xfer_len;
                if(s->fw_bytes - s->fw_bytes_logged >= 65536) {
                    int64_t now = g_get_monotonic_time();
                    double secs = s->fw_last_us ? (now - s->fw_last_us) / 1e6 : 0;
                    s->fw_last_us = now;
                    s->fw_bytes_logged = s->fw_bytes;
                    printf("[SDIO] %u KiB written to the backplane (now at 0x%08x); "
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
                    printf("[SDIO] first SDPCM frame on function 2 (%u bytes)\n", xfer_len);
                }
                g_autofree uint8_t *buf = g_malloc0(xfer_len);
                cpu_physical_memory_read(s->baddr, buf, xfer_len);
                sdpcm_receive(s, buf, xfer_len);
            }
        } else {
            if(func == 0x1) {
                g_autofree uint8_t *buf = g_malloc(xfer_len);
                backplane_read(s, sb_addr, buf, xfer_len);
                cpu_physical_memory_write(s->baddr, buf, xfer_len);
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
                    printf("[SDIO] function 2 read of %u bytes, %s", xfer_len,
                           f ? "handing up" : "nothing queued");
                    if (f) {
                        printf(" frame len %u chan %u, %u already taken",
                               f->len, f->data[5] & SDPCM_CHANNEL_MASK,
                               f->read_off);
                    }
                    printf("\n");
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
                cpu_physical_memory_write(s->baddr, buf, xfer_len);
            }
            
        }

        /* Bit 0 is transfer complete and bit 1 is the card's own interrupt.
         * They must accumulate: a control frame queued during this very
         * transfer sets bit 1, and overwriting it here loses the only
         * indication the driver has that a reply is waiting. */
        raise_irq_soon(s, 0x1);
        //printf("Raised IRQ\n");
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
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdio_scan_complete, s);

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
