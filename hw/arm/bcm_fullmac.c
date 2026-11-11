/* Shared Broadcom CDC/BDC firmware model, used by SDIO and USB HSIC.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/net/bcm_fullmac.h"
#include "net/checksum.h"
#include "trace.h"

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

static void bcm_fullmac_send(BCMFullmacState *s, unsigned channel,
                             const uint8_t *data, uint32_t len)
{
    s->send(s->opaque, channel, data, len);
}

/*
 * How much BDC header this driver expects in front of an 802.3 frame. Nothing
 * is known until the first control command has been seen, and the only frames
 * that can arrive before that are ones the model itself sends after the driver
 * has talked to it, so the 2.1.1 default is never actually used blind.
 */
static unsigned sdio_bdc_hdrlen(BCMFullmacState *s)
{
    return s->bdc_hdrlen ? s->bdc_hdrlen : BDC_HDRLEN;
}

/*
 * Deliver a firmware event. The frame is an ordinary 802.3 packet on the data
 * channel whose ethertype the driver recognizes; everything from the bcmeth
 * header inwards is big endian.
 *
 * The layout is pinned by what handleEventPacket checks: three OUI bytes at
 * packet offset 0x13 and a usr_subtype of 1 at 0x16, with the event message
 * itself at 0x18.
 */
static void bcm_fullmac_event_data(BCMFullmacState *s, uint32_t event_type,
                                   uint32_t status, uint16_t msg_flags,
                                   const uint8_t *data, uint32_t data_len)
{
    const uint8_t *our_mac = s->mac;
    unsigned bdclen = sdio_bdc_hdrlen(s);
    uint8_t frame[BDC_MAX_HDRLEN + 14 + 10 + WL_EVENT_MSG_LEN +
                  ESCAN_RESULTS_FIXED + BSS_INFO_TOTAL];
    /* wl_event_msg is 48 bytes through ifidx/bsscfgidx; payload starts
     * there. Keep four trailing bytes AFTER the payload as well: native
     * N42 handleEventPacket (807d185e..1860) rejects length <= end of data.
     * Empty events have always retained this padding for older guests. */
    assert(data_len <= ESCAN_RESULTS_FIXED + BSS_INFO_TOTAL);
    uint32_t framelen = bdclen + 14 + 10 + WL_EVENT_MSG_LEN + data_len;
    uint8_t *p = frame;

    memset(frame, 0, sizeof(frame));

    /* BDC header: four bytes, plus the two of padding 2.1.1's driver skips. */
    *p++ = BDC_PROTO_VER << 4;
    *p++ = 0; /* priority */
    *p++ = 0; /* flags2 */
    *p++ = 0; /* data offset, in 4-byte words */
    for (unsigned i = BDC_HDRLEN_STD; i < bdclen; i++) {
        *p++ = 0;
    }

    uint8_t *eth = p;
    memcpy(p, our_mac, 6);
    p += 6; /* destination: us */
    memset(p, 0, 6);
    p += 6; /* source: the dongle itself */
    stw_be_p(p, ETHER_TYPE_BRCM);
    p += 2;

    /* bcmeth header. */
    stw_be_p(p, BCMETH_SUBTYPE);
    p += 2;
    uint8_t *lenp = p;
    p += 2;   /* filled in below */
    *p++ = 0; /* version */
    *p++ = BCMETH_OUI_0;
    *p++ = BCMETH_OUI_1;
    *p++ = BCMETH_OUI_2;
    stw_be_p(p, BCMETH_USR_SUBTYPE);
    p += 2;

    /* The length field counts everything from the version byte onwards. */
    stw_be_p(lenp, (frame + framelen) - (lenp + 2));

    /* wl_event_msg, all big endian. */
    stw_be_p(p + 0x00, 1); /* version */
    stw_be_p(p + 0x02, msg_flags);
    stl_be_p(p + 0x04, event_type);
    stl_be_p(p + 0x08, status);
    stl_be_p(p + 0x0c, 0); /* reason */
    stl_be_p(p + 0x10, 0); /* auth type */
    stl_be_p(p + 0x14, data_len);
    memcpy(p + 0x18, our_mac, 6);
    g_strlcpy((char *)p + 0x1e, "en0", 16);
    if (data_len) {
        memcpy(p + 48, data, data_len);
    }

    /* The driver sees the frame from the ethernet header onwards, so its
     * offsets 0x13 and 0x16 are relative to eth, not to the BDC header. */
    assert(eth[0x13] == BCMETH_OUI_0 && eth[0x16] == 0 && eth[0x17] == 1);
    trace_sdio(
        "[SDIO] sending event %u, status %u, flags 0x%04x (%u byte frame)\n",
        event_type, status, msg_flags, framelen);

    /*
     * 2.1.1's driver dispatches receive by channel and answers channel 1 with
     * "WTF?? Got an event packet!!!" before dropping it, so its events have to
     * arrive on the data channel and be recognized by their ethertype.
     * 3.1.3's rxPacket has a real sixteen entry jump table on the channel
     * (VA 0xc07191d6): 0 control, 1 event, 2 data, 3 superframe, 15, and
     * "ignore" for the rest - and its handleDataPacket has no ethertype test
     * for BRCM frames at all, so an event on channel 2 is dropped in silence.
     */
    bcm_fullmac_send(
        s, bdclen == BDC_HDRLEN_STD ? BCM_FULLMAC_EVENT : BCM_FULLMAC_DATA,
        frame, framelen);
}

static void sdpcm_send_event(BCMFullmacState *s, uint32_t event_type,
                             uint32_t status, uint16_t msg_flags)
{
    bcm_fullmac_event_data(s, event_type, status, msg_flags, NULL, 0);
}

/* The BSSID and SSID the model pretends to be associated with. The BSSID is
 * the "bssid" property (s->bssid); this is its default. */
const uint8_t bcm_fullmac_default_bssid[6] = {0x02, 0x00, 0x5e,
                                              0x10, 0x00, 0x01};
#define FAKE_SSID "qemu-ios"

/* Describe the network the model claims to be on, open and on channel 6. */
static void fill_bss_info(BCMFullmacState *s, uint8_t *bi)
{
    static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96};

    stl_le_p(bi + BSS_INFO_OFF_VERSION, BSS_INFO_VERSION);
    stl_le_p(bi + BSS_INFO_OFF_LENGTH, BSS_INFO_TOTAL);
    memcpy(bi + BSS_INFO_OFF_BSSID, s->bssid, sizeof(s->bssid));
    stw_le_p(bi + BSS_INFO_OFF_BEACON, 100);
    stw_le_p(bi + BSS_INFO_OFF_CAPABILITY, BSS_CAP_ESS); /* open: no privacy */
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
     * a real beacon would start with, and the 802.11d country element an access
     * point with its regulatory domain set sends: Setup Assistant (5.x and 6.x)
     * guesses the country, and from it the languages its first page offers
     * over the Welcome animation, from the country of the access points a Wi-Fi
     * scan finds (WiFiNetworkGet11dCountryCodeFromIe) where the SIM gives none.
     */
    stw_le_p(bi + BSS_INFO_OFF_IE_OFFSET, BSS_INFO_LEN);
    stl_le_p(bi + BSS_INFO_OFF_IE_LENGTH, BSS_INFO_IE_LEN);

    uint8_t *ie = bi + BSS_INFO_LEN;
    *ie++ = 0x00; /* SSID */
    *ie++ = strlen(FAKE_SSID);
    memcpy(ie, FAKE_SSID, strlen(FAKE_SSID));
    ie += strlen(FAKE_SSID);
    *ie++ = 0x01; /* supported rates */
    *ie++ = sizeof(rates);
    memcpy(ie, rates, sizeof(rates));
    ie += sizeof(rates);
    *ie++ = 0x03; /* DS parameter set */
    *ie++ = 0x01;
    *ie++ = 6;    /* channel */
    *ie++ = 0x07; /* country */
    *ie++ = 6;
    *ie++ = s->country[0];
    *ie++ = s->country[1];
    *ie++ = ' '; /* any environment */
    *ie++ = 1;   /* channels 1 to 11 */
    *ie++ = 11;
    *ie++ = 20; /* at 20 dBm */
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
static void fill_iscan_results(BCMFullmacState *s, uint8_t *p)
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
    fill_bss_info(s, p + ISCAN_OFF_BSS);
    trace_sdio("[SDIO] iscanresults: 1 BSS, scan complete\n");
}

/*
 * A real scan takes seconds of radio time. Reporting it complete in the same
 * breath as the request arrives means the event can overtake the driver's own
 * bookkeeping for the scan it just started, so the completion is armed on a
 * timer instead.
 */
static void sdio_scan_complete(void *opaque)
{
    BCMFullmacState *s = (BCMFullmacState *)opaque;

    if (s->escan_pending) {
        uint8_t result[ESCAN_RESULTS_FIXED + BSS_INFO_TOTAL] = {0};
        s->escan_pending = false;
        s->iscan_reported = true;
        stl_le_p(result + 4, 1); /* escan result version, not BSS version */
        stw_le_p(result + 8, s->escan_sync_id);
        if (!s->escan_status) {
            stl_le_p(result, sizeof(result));
            stw_le_p(result + 10, 1);
            fill_bss_info(s, result + ESCAN_RESULTS_FIXED);
            bcm_fullmac_event_data(s, WLC_E_ESCAN_RESULT, ESCAN_STATUS_PARTIAL,
                                   0, result, sizeof(result));
        }
        stl_le_p(result, ESCAN_RESULTS_FIXED);
        stw_le_p(result + 10, 0);
        bcm_fullmac_event_data(s, WLC_E_ESCAN_RESULT, s->escan_status, 0,
                               result, ESCAN_RESULTS_FIXED);
    } else {
        sdpcm_send_event(s, WLC_E_SCAN_COMPLETE, 0, 0);
    }
}

/* Broadcom escan v1: little-endian version/action/sync_id followed by
 * scan parameters. Native 11A465 sends this instead of legacy iscan. */
static bool bcm_escan_request(BCMFullmacState *s, const uint8_t *data,
                              uint32_t len)
{
    if (len < 8 || ldl_le_p(data) != 1) {
        return false;
    }
    unsigned action = lduw_le_p(data + 4);
    if (action != 1 && action != 3) {
        return false;
    }
    if (action == 3 && !s->escan_pending) {
        return true;
    }
    if (action == 1) {
        s->escan_sync_id = lduw_le_p(data + 6);
    }
    s->escan_status = action == 3 ? ESCAN_STATUS_ABORT : 0;
    s->escan_pending = true;
    s->iscan_reported = false;
    if (s->scan_timer) {
        timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                     SCAN_COMPLETE_DELAY_NS);
    }
    return true;
}

static void bcm_fullmac_credit_map(BCMFullmacState *s)
{
    if (s->fws_supported && (s->fws_tlv & 4)) {
        const uint8_t credits[6] = {8, 8, 8, 8, 8, 8};
        bcm_fullmac_event_data(s, 74, 0, 0, credits, sizeof(credits));
    }
}

/*
 * The event chain a dongle reports on its way into a BSS. The driver's own
 * dispatch table decides what it believes: WLC_E_SET_SSID with status 0 is a
 * successful join, WLC_E_LINK with the link flag set is the carrier coming up.
 */
static void sdio_send_assoc_events(BCMFullmacState *s)
{
    s->associated = true;
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
static void sdio_handle_set_ssid(BCMFullmacState *s, const uint8_t *payload,
                                 uint32_t len)
{
    char ssid[WLC_SSID_MAX + 1] = {0};
    uint32_t n = 0;

    if (len >= 4) {
        n = ldl_le_p(payload);
        n = MIN(n, MIN(len - 4, WLC_SSID_MAX));
        memcpy(ssid, payload + 4, n);
    }

    /* A zero-length SSID is a disassociate request, not a join. */
    if (n == 0) {
        trace_sdio("[SDIO] WLC_SET_SSID with no SSID: leaving the network\n");
        s->associated = false;
        sdpcm_send_event(s, WLC_E_LINK, 0, 0);
        return;
    }

    trace_sdio("[SDIO] WLC_SET_SSID '%s': associating\n", ssid);
    sdio_send_assoc_events(s);
}

/*
 * Join without waiting to be asked. iOS only scans while the Wi-Fi pane is open
 * or when it already knows a network, so a device that has never been taken
 * through Settings sits with the radio up and never associates - no scan, no
 * WLC_SET_SSID, no link, no DHCP. That is the state every fresh image and every
 * app-shipped image is in, and there is no UI to tap on a headless run, so the
 * model asserts the association itself. Set IT_WIFI_AUTOJOIN=0 to require a
 * real driver-driven join instead; any other value is the delay in seconds
 * (fractions allowed), counted from when the host's network interface is up
 * (sdpcm_handle_cdc).
 */
static void sdio_autojoin(void *opaque)
{
    BCMFullmacState *s = (BCMFullmacState *)opaque;

    if (s->associated) {
        return;
    }
    /*
     * Not in the middle of a scan: the driver's scan manager is busy until it
     * has the results, and a join reported then is lost for good ("Can't add
     * beacon, scan manager is busy", no BSS object, no link, and the model
     * thinking itself associated). 5.x's first wifid scan lands about when
     * this timer does, so a fresh device missed the join now and then
     * (LightTouchMac smoke #40).
     */
    if (!s->iscan_reported) {
        trace_sdio("[SDIO] a scan is outstanding: joining after it\n");
        timer_mod(s->join_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                     2 * NANOSECONDS_PER_SECOND);
        return;
    }
    trace_sdio("[SDIO] no join requested: associating with '%s' anyway\n",
               FAKE_SSID);
    sdio_send_assoc_events(s);
}

static void sdio_arm_autojoin(BCMFullmacState *s)
{
    const char *v = getenv("IT_WIFI_AUTOJOIN");
    double delay = 10;

    /* Fractions too: tests/ipad1/regress.py's wifi-early check joins 10 ms
     * after the arming point, the order a heavily loaded host produces. */
    if (v && (!*v || *v == '0') && strtod(v, NULL) <= 0) {
        return;
    }
    if (s->associated || !s->join_timer) {
        return;
    }
    if (v && strtod(v, NULL) > 0) {
        delay = strtod(v, NULL);
    }
    timer_mod(s->join_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                 (int64_t)(delay * NANOSECONDS_PER_SECOND));
}

/*
 * How long this build's CDC header is, latched from the request.
 *
 * The legacy SDIO driver sends the whole buffer it is asking about - a get_var
 * carries the iovar name in a payload_len sized buffer - so the bytes between
 * the start of the CDC header and that payload are the header itself.
 * AppleBCM4325 (2.1.1) gives twelve, AppleBCMWLAN (3.1.3) sixteen. Anything
 * else is a request that did not carry its whole buffer, so it is ignored and
 * whatever was latched before stands; a running dongle does not change form.
 */
static unsigned cdc_hdrlen(BCMFullmacState *s, uint32_t len,
                           uint32_t payload_len, unsigned known_hdrlen)
{
    if (known_hdrlen || len >= payload_len) {
        uint32_t hdr = known_hdrlen ? known_hdrlen : len - payload_len;
        if (hdr == CDC_HDRLEN || hdr == CDC_HDRLEN_STATUS) {
            if (s->cdc_hdrlen != hdr) {
                s->cdc_hdrlen = hdr;
                /* The two headers moved together: the driver that wants the
                 * status word is the one that skips four bytes of BDC. */
                s->bdc_hdrlen =
                    hdr == CDC_HDRLEN_STATUS ? BDC_HDRLEN_STD : BDC_HDRLEN;
                trace_sdio("[SDIO] CDC header is %u bytes, BDC header is %u\n",
                           s->cdc_hdrlen, s->bdc_hdrlen);
            }
        }
    }
    return s->cdc_hdrlen ? s->cdc_hdrlen : CDC_HDRLEN;
}

void bcm_fullmac_cdc(BCMFullmacState *s, const uint8_t *cdc, uint32_t len,
                     unsigned known_hdrlen)
{
    assert(!known_hdrlen || known_hdrlen == CDC_HDRLEN ||
           known_hdrlen == CDC_HDRLEN_STATUS);
    if (len < MAX(CDC_HDRLEN, known_hdrlen)) {
        return;
    }
    uint32_t cmd = ldl_le_p(cdc);
    uint32_t len_field = ldl_le_p(cdc + 4);
    uint32_t payload_len = len_field;
    uint32_t flags = ldl_le_p(cdc + 8);
    /*
     * 6.x's AppleBCMWLANCore splits the length word the way later dhd does:
     * the output buffer in bits 0-15, what the request carries in 16-31
     * (a "ver" get is 0x00040104: four bytes of name, 260 back). Earlier
     * drivers send one length, never above 64 KiB.
     */
    uint32_t in_len = len_field >> 16 ? len_field >> 16 : len_field;
    if (len_field >> 16) {
        payload_len = (flags & CDC_DCMD_SET) ? in_len : (len_field & 0xffff);
    }
    /* Only the 16-byte-header driver (status word, four bytes of BDC) splits the length; its frames can carry
     * slack past in_len, which must not read as the 12-byte form. */
    unsigned hdrlen =
        cdc_hdrlen(s, len_field >> 16 ? CDC_HDRLEN_STATUS + in_len : len,
                   in_len, known_hdrlen);

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
    trace_sdio("[SDIO] CDC command %u (%s), %u bytes, flags 0x%08x%s%s\n", cmd,
               (flags & CDC_DCMD_SET) ? "set" : "get", payload_len, flags,
               *iovar ? " iovar " : "", iovar);

    /*
     * On a set, the payload is what arrived and cannot be longer than the
     * frame. On a get it is the size of the buffer the driver is waiting to
     * have filled, and the reply has to be exactly that long: the command
     * manager asserts on it (AppleBCM4325CmdManager.cpp:213) and then reads
     * its own uninitialized buffer. Clamping both ways answered every get
     * with a few bytes - a 1148 byte WLC_GET_BSS_INFO came back as zero - and
     * that is why the joined BSS had a garbage BSSID and an empty SSID.
     */
    if (flags & CDC_DCMD_SET) {
        if (payload_len > (len > hdrlen ? len - hdrlen : 0)) {
            payload_len = len > hdrlen ? len - hdrlen : 0;
        }
    } else if (payload_len > CDC_MAX_PAYLOAD) {
        payload_len = CDC_MAX_PAYLOAD;
    }

    /* The length field is checked against what the command expects, not
     * against the request - AppleBCM4325CmdManager.cpp:445 asserts on it. */
    g_autofree uint8_t *reply = g_malloc0(hdrlen + payload_len);
    stl_le_p(reply, cmd);
    stl_le_p(reply + 4, len_field >> 16 ? (len_field & 0xffff0000) | payload_len
                                        : payload_len);
    stl_le_p(reply + 8, flags & ~CDC_DCMD_ERROR); /* same id, no error */
    if (hdrlen >= CDC_HDRLEN_STATUS) {
        stl_le_p(reply + CDC_OFF_STATUS, 0); /* the command succeeded */
    }

    /* A few gets have to carry something believable once the model claims to
     * be associated - an all-zero BSSID reads as "not associated". */
    if (!(flags & CDC_DCMD_SET)) {
        if (cmd == WLC_GET_BSS_INFO && payload_len >= 4 + BSS_INFO_TOTAL) {
            /* Four byte buffer length, then the structure itself. */
            stl_le_p(reply + hdrlen, BSS_INFO_TOTAL);
            fill_bss_info(s, reply + hdrlen + 4);
        } else if (cmd == WLC_GET_BSSID && payload_len >= sizeof(s->bssid)) {
            memcpy(reply + hdrlen, s->bssid, sizeof(s->bssid));
        } else if (cmd == WLC_GET_RSSI && payload_len >= 4) {
            /* Polled to drive the status bar's signal bars; zero reads as no
             * signal, which is why the icon showed empty. */
            stl_le_p(reply + hdrlen, (uint32_t)(int32_t)-45);
        } else if (cmd == WLC_GET_RATE && payload_len >= 4) {
            stl_le_p(reply + hdrlen, 108); /* 54 Mbit, in 500 kbit units */
        } else if (cmd == WLC_GET_VAR && g_str_equal(iovar, "iscanresults") &&
                   payload_len >= ISCAN_TOTAL) {
            fill_iscan_results(s, reply + hdrlen);
        } else if (cmd == WLC_GET_VAR && g_str_equal(iovar, "counters") &&
                   payload_len >= WL_CNT_OFF_RXBEACONMBSS + 4 &&
                   s->associated) {
            /*
             * wl_cnt_t's rxbeaconmbss, beacons heard from our BSS: the AP
             * beacons every 100 TU. 5.x's AppleBCMWLANCore::updateCounters
             * (9B206 0x80591676) reads it as RxBeacons, and checkForBeaconLoss
             * disconnects ("Null Rx Beacon count") once it and every receive
             * counter have stood still for six polls: with it left at zero, a
             * quiet minute (Setup Assistant, an idle home screen) dropped the
             * join. ponytail: the other counters stay zero.
             */
            stl_le_p(reply + hdrlen + WL_CNT_OFF_RXBEACONMBSS,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
                         (1024 * 100 * 1000));
        } else if (cmd == WLC_GET_VAR && g_str_equal(iovar, "cur_etheraddr") &&
                   payload_len >= sizeof(s->mac)) {
            memcpy(reply + hdrlen, s->mac, sizeof(s->mac));
        } else if (cmd == WLC_GET_VAR && g_str_equal(iovar, "ver") &&
                   s->fw_version && payload_len) {
            /* initFirmware logs it as "BCMWLAN Firmware Version: %s". */
            strncpy((char *)reply + hdrlen, s->fw_version, payload_len - 1);
        } else if (cmd == WLC_GET_VAR && g_str_equal(iovar, "cap") &&
                   payload_len) {
            /*
             * The firmware's feature words. 6.x's AppleBCMWLANCore halts with
             * "AP mode unsupported" when "ap" is missing; earlier drivers
             * never ask. ponytail: a 4.221-era BCM4329 list, not read off a unit.
             */
            strncpy(
                (char *)reply + hdrlen,
                "ap sta wme 802.11d 802.11h rm cqa cac dualband ampdu ampdu_tx ampdu_rx amsdurx",
                payload_len - 1);
        } else if (cmd == WLC_GET_SSID && payload_len >= 4) {
            /* wlc_ssid_t: a length word then up to 32 bytes. */
            uint32_t n = MIN(strlen(FAKE_SSID), payload_len - 4);
            stl_le_p(reply + hdrlen, n);
            memcpy(reply + hdrlen + 4, FAKE_SSID, n);
        }
    }

    if ((flags & CDC_DCMD_SET) && cmd == WLC_SET_VAR &&
        g_str_equal(iovar, "escan")) {
        unsigned name_len = strlen(iovar) + 1;
        if (payload_len < name_len ||
            !bcm_escan_request(s, cdc + hdrlen + name_len,
                               payload_len - name_len)) {
            stl_le_p(reply + 8, flags | CDC_DCMD_ERROR);
            if (hdrlen >= CDC_HDRLEN_STATUS) {
                stl_le_p(reply + CDC_OFF_STATUS,
                         (uint32_t)-2); /* BCME_BADARG */
            }
        }
    }

    bcm_fullmac_send(s, BCM_FULLMAC_CONTROL, reply, hdrlen + payload_len);

    /* The driver arms a scan and then waits: it re-sends this every fifteen
     * seconds and never asks for results on its own. Tell it the scan is over
     * so it comes back for them. */
    if (cmd == WLC_SET_VAR && g_str_equal(iovar, "iscan")) {
        s->escan_pending = false;
        s->iscan_reported = false;
        if (s->scan_timer) {
            timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                         SCAN_COMPLETE_DELAY_NS);
        }
    }

    /* The offload engine's switches: a NUL-terminated name, then a 32-bit value. */
    if ((flags & CDC_DCMD_SET) && cmd == WLC_SET_VAR && *iovar &&
        payload_len >= strlen(iovar) + 1 + 4) {
        uint32_t value = ldl_le_p(cdc + hdrlen + strlen(iovar) + 1);
        if (g_str_equal(iovar, "tlv") || g_str_equal(iovar, "bus:txglom")) {
            trace_sdio("[SDIO] firmware signalling %s = 0x%08x\n", iovar,
                       value);
        }
        if (g_str_equal(iovar, "tlv") && s->fws_supported) {
            bool enable = (value & 4) && !(s->fws_tlv & 4);
            s->fws_tlv = value;
            if (enable) {
                /* Six finite per-FIFO budgets, replenished on completion.
                 * Native AppleBCMWLANCore consumes this six-byte event 74. */
                bcm_fullmac_credit_map(s);
            }
        }
        if (g_str_equal(iovar, "cur_etheraddr") &&
            payload_len >= strlen(iovar) + 1 + sizeof(s->mac)) {
            memcpy(s->mac, cdc + hdrlen + strlen(iovar) + 1, sizeof(s->mac));
        } else if (g_str_equal(iovar, "toe")) {
            s->toe = value;
        } else if (g_str_equal(iovar, "toe_ol")) {
            s->toe_ol = value;
        }
    }

    /* The driver joining a network it found in the scan results. */
    if ((flags & CDC_DCMD_SET) && cmd == WLC_SET_SSID) {
        sdio_handle_set_ssid(s, cdc + hdrlen, len > hdrlen ? len - hdrlen : 0);
    }

    /*
     * A real dongle reports a join only after the host asked for one, and the
     * host can only ask through its network interface, so a join never lands
     * before that interface exists. The auto-join stands in for the host's
     * request and has to keep that order. WLC_UP is not the point: the driver
     * sends it in the middle of initFirmware and attaches its IO80211Interface
     * only after the rest of the init iovars. A join reported in between goes
     * through bringUpLink into AppleBCMWLAN::setLinkState with no interface
     * (8F190: IO80211Interface's link-state getter at 0x80656f7c reads +0xc4
     * of NULL), and a 10 s clock from WLC_UP gets there whenever host load
     * slows the guest (LightTouchMac matrix 09-29, 8F190 and 8G4 run 2).
     * mcast_list is the first thing the host sends that needs the interface:
     * IOEthernetController::setMulticastList, called once the network stack
     * has attached it (3.1.3 AppleBCM4325, 4.2-4.3 AppleBCMWLAN and 5.x
     * AppleBCMWLANCore all send it before they could join).
     * Firmware coming up is not in a BSS: after the driver's watchdog reset
     * (a command-queue stall under host load re-downloads the firmware
     * without a machine reset) the join has to happen again, and the
     * interface from before is still attached.
     */
    if (cmd == WLC_UP) {
        s->associated = false;
        if (s->host_netif) {
            sdio_arm_autojoin(s);
        }
    }
    if (cmd == WLC_SET_VAR && g_str_equal(iovar, "mcast_list") &&
        !s->host_netif) {
        s->host_netif = true;
        sdio_arm_autojoin(s);
    }
}

/* --- the 802.3 data path ------------------------------------------------ */

static void hexdump_frame(const char *what, const uint8_t *p, uint32_t len)
{
    trace_sdio("[SDIO] %s (%u bytes):", what, len);
    for (unsigned i = 0; i < MIN(len, 42u); i++) {
        trace_sdio(" %02x", p[i]);
    }
    trace_sdio("\n");
}

/* Broadcom firmware signalling sits between BDC and Ethernet. PKTTAG is
 * opaque except for its FIFO and status bits; echo its identity in TXSTATUS
 * and return exactly the credit consumed by the completed packet. */
static bool bcm_fws_tag(const uint8_t *data, unsigned len, uint32_t *tag,
                        bool *present)
{
    *present = false;
    for (unsigned off = 0; off < len;) {
        unsigned type = data[off++];
        if (type == 255) { /* one-byte alignment filler */
            continue;
        }
        if (off == len || data[off] > len - off - 1) {
            return false;
        }
        unsigned size = data[off++];
        if (type == 5) {
            if (size != 4 || *present) {
                return false;
            }
            *tag = ldl_le_p(data + off);
            if (((*tag >> 24) & 7) >= 6) {
                return false;
            }
            *present = true;
        }
        off += size;
    }
    return true;
}

static void bcm_fws_complete(BCMFullmacState *s, uint32_t tag)
{
    /* Four BDC bytes, six TXSTATUS bytes, eight FIFO_CREDITBACK bytes,
     * then two single-byte fillers. dataOffset counts four-byte words. */
    uint8_t reply[20] = {BDC_PROTO_VER << 4, 0, 0, 4, 4, 4};
    stl_le_p(reply + 6, tag & ~0x78000000U); /* status 0: success */
    reply[10] = 11;
    reply[11] = 6;
    reply[12 + ((tag >> 24) & 7)] = 1;
    reply[18] = reply[19] = 255;
    bcm_fullmac_send(s, BCM_FULLMAC_DATA, reply, sizeof(reply));
}

/*
 * A data frame the driver handed down. The BDC header is four bytes plus
 * dataOffset four-byte words of padding; the driver's own receive path skips
 * six, so a dataOffset of zero would be four here and does not have to match.
 * Both are accepted, and the ethertype is used to tell which was meant.
 */
void bcm_fullmac_tx_data(BCMFullmacState *s, const uint8_t *payload,
                         uint32_t len)
{
    if (len < BDC_HDRLEN_STD + 14) {
        trace_sdio("[SDIO] data frame too short to be 802.3: %u bytes\n", len);
        return;
    }

    uint32_t hdrlen = 4 + 4 * payload[3];
    uint32_t tag = 0;
    bool tagged = false;
    bool signalling = s->fws_supported && (s->fws_tlv & 4);
    if (signalling && (hdrlen > len - 14 ||
                       !bcm_fws_tag(payload + 4, hdrlen - 4, &tag, &tagged))) {
        trace_sdio("[SDIO] malformed firmware signalling header\n");
        return;
    }
    /* Pick whichever offset leaves a plausible ethernet header. A frame from
     * the guest is either ARP, IPv4 or IPv6 in practice. */
    static const uint32_t candidates[] = {4, 6, 8};
    uint32_t chosen = 0;
    for (unsigned i = 0; !signalling && i < ARRAY_SIZE(candidates); i++) {
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
        trace_sdio("[SDIO] guest -> host, bdc %u bytes (dataOffset byte %u), "
                   "ethertype 0x%04x\n",
                   chosen, payload[3], lduw_be_p(payload + chosen + 12));
        hexdump_frame("guest frame", payload + chosen, len - chosen);
    }

    if (!s->nic) {
        return;
    }
    if (s->toe && (s->toe_ol & TOE_TX_CSUM_OL)) {
        /* The host left the checksums to the dongle: IPv4 header 0, TCP/UDP only the pseudo-header sum. */
        g_autofree uint8_t *frame = g_memdup2(payload + chosen, len - chosen);
        net_checksum_calculate(frame, len - chosen, CSUM_ALL);
        qemu_send_packet(qemu_get_queue(s->nic), frame, len - chosen);
    } else {
        qemu_send_packet(qemu_get_queue(s->nic), payload + chosen,
                         len - chosen);
    }
    if (tagged) {
        bcm_fws_complete(s, tag);
    }
}

/* A frame from the network backend, wrapped for the driver's receive path. */
static ssize_t sdio_net_receive(NetClientState *nc, const uint8_t *buf,
                                size_t size)
{
    BCMFullmacState *s = qemu_get_nic_opaque(nc);

    if (!s->dongle_started || size < 14 || size > 1600) {
        return size;
    }

    unsigned bdclen = sdio_bdc_hdrlen(s);
    g_autofree uint8_t *frame = g_malloc0(bdclen + size);
    frame[0] = BDC_PROTO_VER << 4;
    frame[1] = 0; /* priority */
    frame[2] = 0; /* flags2 */
    frame[3] = 0; /* dataOffset */
    memcpy(frame + bdclen, buf, size);

    if (s->host_rx_log < 24) {
        s->host_rx_log++;
        hexdump_frame("host -> guest", buf, size);
    }

    bcm_fullmac_send(s, BCM_FULLMAC_DATA, frame, bdclen + size);
    return size;
}

static bool sdio_net_can_receive(NetClientState *nc)
{
    BCMFullmacState *s = qemu_get_nic_opaque(nc);

    /* Before the dongle is up there is nothing to hand a frame to, and an
     * unbounded queue of them would be handed over all at once later. */
    return s->dongle_started && (!s->can_receive || s->can_receive(s->opaque));
}

static NetClientInfo sdio_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = sdio_net_can_receive,
    .receive = sdio_net_receive,
};

void bcm_fullmac_init(BCMFullmacState *s, BCMFullmacSend send,
                      BCMFullmacCanReceive can_receive, void *opaque)
{
    s->send = send;
    s->can_receive = can_receive;
    s->opaque = opaque;
    memcpy(s->bssid, bcm_fullmac_default_bssid, sizeof(s->bssid));
    memcpy(s->country, "US", 3);
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdio_scan_complete, s);
    s->join_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdio_autojoin, s);
    bcm_fullmac_reset(s);
}

void bcm_fullmac_reset(BCMFullmacState *s)
{
    timer_del(s->scan_timer);
    timer_del(s->join_timer);
    s->dongle_started = s->associated = s->host_netif = false;
    s->iscan_reported = true;
    s->escan_pending = false;
    s->escan_sync_id = 0;
    s->escan_status = 0;
    s->cdc_hdrlen = s->bdc_hdrlen = 0;
    s->toe = s->toe_ol = 0;
    s->fws_tlv = 0;
    s->tx_log = s->host_rx_log = 0;
}

void bcm_fullmac_cleanup(BCMFullmacState *s)
{
    timer_free(s->scan_timer);
    timer_free(s->join_timer);
    if (s->nic) {
        qemu_del_nic(s->nic);
    }
}

void bcm_fullmac_setup_net(BCMFullmacState *s, DeviceState *owner,
                           const char *model, const char *netdev)
{
    NetClientState *peer = qemu_find_netdev(netdev);
    if (!peer) {
        return;
    }
    memcpy(s->conf.macaddr.a, s->mac, sizeof(s->mac));
    s->conf.peers.ncs[0] = peer;
    s->conf.peers.queues = 1;
    s->nic = qemu_new_nic(&sdio_net_info, &s->conf, model, "wifi",
                          &owner->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}
