/*
 * Marvell 88W8686 SDIO Wi-Fi card (iPod touch 1G). See include/hw/arm/mrvl8686.h.
 *
 * What AppleMRVL868x-69 (iPhone OS 1.1, kernelcache 3A101a) does with it,
 * which is what this answers:
 *
 *   probe: power up, read the I/O port (0x00-0x02) and the card status (0x20,
 *     IO_RDY), send the 0x980-byte helper in 64-byte writes of {length, data}
 *     ending with a zero length, then read the card's EEPROM through the
 *     helper: 16-byte GETMEM requests (0x14, 512 bytes at an offset) while the
 *     helper asks for 16 bytes (0x10/0x11 = 0x10), each answered by UL_RDY,
 *     an error byte at 0x36 and the chunk length at 0x34/0x35. The EEPROM's
 *     records give the Wi-Fi MAC (key 2, written to the nub's
 *     local-mac-address) and the TX calibration (key 1, tx-calibration).
 *   start: the 120 KiB main program, one block at a time as the helper asks
 *     (0x10/0x11): a 16-byte header, then the block; a block that fails its
 *     CRC is reported by bit 0 of the next request. After the last header the
 *     firmware writes FIRMWARE_OK (0xfedc) to 0x34/0x35.
 *   then: host commands and data frames on the I/O port behind a 4-byte
 *     {length, type} header, the card's packets fetched on UP_LD with their
 *     length at 0x34/0x35, DN_LD when the card takes the next one, both in
 *     the host interrupt status (0x05, write 0 to clear) under its mask (0x04).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "hw/arm/mrvl8686.h"
#include "trace.h"
#include "hw/trace-printf.h"

static void G_GNUC_PRINTF(1, 2) mrvl_trace(const char *fmt, ...)
{
    if (trace_event_get_state_backends(TRACE_MRVL8686_LOG)) {
        va_list ap;
        va_start(ap, fmt);
        g_autofree char *msg = g_strdup_vprintf(fmt, ap);
        va_end(ap);
        trace_mrvl8686_log(g_strchomp(msg));
    }
}

/* mrvl8686 core: begin (tests/ipod/test_mrvl8686.py compiles this part) */

static void mrvl_card_send(Mrvl8686Card *c, const uint8_t *frame, uint32_t len);

/* The firmware image's block CRC: CRC-32/MPEG-2 without the init/xorout,
 * stored big endian (checked against every block of the 3A101a image). */
static uint32_t mrvl_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0;
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; i++) {
            crc = crc & 0x80000000u ? crc << 1 ^ 0x04c11db7u : crc << 1;
        }
    }
    return crc;
}

/* Put the head of the upload queue where the host looks for it. */
static void mrvl_upq_kick(Mrvl8686Card *c)
{
    if (c->stage == MRVL_STAGE_FIRMWARE && c->upq_count) {
        c->scratch = c->upq_len[c->upq_head];
        c->hint_status |= MRVL_HINT_UP_LD;
    }
}

/* Queue a packet for the host; the payload after the SDIO header is the caller's. */
static uint8_t *mrvl_upq_push(Mrvl8686Card *c, uint16_t type, uint32_t len)
{
    if (c->upq_count == MRVL_UPQ_SLOTS || len + MRVL_SDIO_HDR > MRVL_UPQ_BYTES) {
        return NULL;
    }
    unsigned slot = (c->upq_head + c->upq_count++) % MRVL_UPQ_SLOTS;
    uint8_t *p = c->upq[slot];
    memset(p, 0, MRVL_UPQ_BYTES);
    c->upq_len[slot] = len + MRVL_SDIO_HDR;
    stw_le_p(p, len + MRVL_SDIO_HDR);
    stw_le_p(p + 2, type);
    if (c->upq_count == 1) {
        mrvl_upq_kick(c);
    }
    return p + MRVL_SDIO_HDR;
}

static void mrvl_event(Mrvl8686Card *c, uint32_t id)
{
    uint8_t *p = mrvl_upq_push(c, MRVL_TYPE_EVENT, 4);
    if (p) {
        stl_le_p(p, id);
    }
}

/* The access point's beacon IEs: SSID, 802.11b/g rates, DS parameter set. */
static uint32_t mrvl_ap_ies(uint8_t *p)
{
    static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    uint8_t *q = p;
    *q++ = 0;
    *q++ = strlen(MRVL_AP_SSID);
    memcpy(q, MRVL_AP_SSID, strlen(MRVL_AP_SSID));
    q += strlen(MRVL_AP_SSID);
    *q++ = 1;
    *q++ = sizeof(rates);
    memcpy(q, rates, sizeof(rates));
    q += sizeof(rates);
    *q++ = 3;
    *q++ = 1;
    *q++ = MRVL_AP_CHANNEL;
    return q - p;
}

/*
 * Does a scan request cover the access point? The request is {BSS type,
 * BSSID} then TLVs; a channel list without channel 6 cannot hear it, and a
 * directed scan for another SSID gets no probe response from it.
 */
static bool mrvl_scan_hears_ap(Mrvl8686Card *c, const uint8_t *body, uint32_t n)
{
    bool channel_ok = true;
    for (uint32_t off = 7; off + 4 <= n;) {
        uint16_t type = lduw_le_p(body + off), len = lduw_le_p(body + off + 2);
        const uint8_t *v = body + off + 4;
        if (off + 4 + len > n) {
            break;
        }
        if (type == MRVL_TLV_SSID && len &&
            (len != strlen(MRVL_AP_SSID) || memcmp(v, MRVL_AP_SSID, len))) {
            return false;
        }
        if (type == MRVL_TLV_CHANLIST) {
            channel_ok = false;
            for (uint32_t i = 0; i + MRVL_CHANLIST_ENTRY <= len; i += MRVL_CHANLIST_ENTRY) {
                channel_ok |= v[i + 1] == MRVL_AP_CHANNEL;
            }
        }
        off += 4 + len;
    }
    return channel_ok;
}

/* Scan response: {size of the BSS descriptions, count}, the descriptions
 * (length, BSSID, RSSI, TSF, beacon interval, capability, IEs), a TSF TLV. */
static uint32_t mrvl_scan_response(Mrvl8686Card *c, uint8_t *r, bool hit)
{
    uint8_t *e = r + 3;
    uint32_t n = 0;
    if (hit) {
        memcpy(e + 2, c->bssid, 6);
        e[8] = 45;                               /* -45 dBm */
        stl_le_p(e + 9, 0x00100000);           /* TSF */
        stw_le_p(e + 17, 100);                 /* beacon interval (TU) */
        stw_le_p(e + 19, 0x0001);              /* ESS, open */
        n = 21 + mrvl_ap_ies(e + 21);
        stw_le_p(e, n - 2);
    }
    stw_le_p(r, n);
    r[2] = hit;
    uint8_t *t = e + n;
    stw_le_p(t, MRVL_TLV_TSF);
    stw_le_p(t + 2, 8 * hit);
    if (hit) {
        stl_le_p(t + 4, 0x00100000);
    }
    return 3 + n + 4 + 8 * hit;
}

static void mrvl_handle_cmd(Mrvl8686Card *c, const uint8_t *cmd, uint32_t n)
{
    if (n < MRVL_CMD_HDR) {
        return;
    }
    uint16_t code = lduw_le_p(cmd), size = MIN(lduw_le_p(cmd + 2), n);
    const uint8_t *body = cmd + MRVL_CMD_HDR;
    uint32_t blen = size > MRVL_CMD_HDR ? size - MRVL_CMD_HDR : 0;

    mrvl_trace("[MRVL] command 0x%04x size %u seq %u\n", code, size, lduw_le_p(cmd + 4));
    /* Neither the sleep confirm nor deep sleep is answered: the firmware sleeps. */
    if (code == MRVL_CMD_802_11_PS_MODE && blen >= 2 && lduw_le_p(body) == MRVL_PS_SLEEP_CONFIRM) {
        return;
    }
    if (code == MRVL_CMD_802_11_DEEP_SLEEP) {
        c->asleep = true;
        return;
    }

    uint8_t r[MRVL_UPQ_BYTES - MRVL_SDIO_HDR];
    uint32_t rlen = MIN(size, sizeof(r));
    memset(r, 0, sizeof(r));
    memcpy(r, cmd, rlen);
    uint8_t *rb = r + MRVL_CMD_HDR;

    switch (code) {
    case MRVL_CMD_GET_HW_SPEC:
        rlen = MAX(rlen, MRVL_CMD_HDR + 38);
        stw_le_p(rb + 0, 0x0002);              /* host interface version */
        stw_le_p(rb + 2, 0x0004);              /* hardware version */
        stw_le_p(rb + 4, 1);                   /* TxPDs */
        stw_le_p(rb + 6, 32);                  /* multicast addresses */
        memcpy(rb + 8, c->mac, 6);
        stw_le_p(rb + 14, 0x10);               /* region: FCC */
        stw_le_p(rb + 16, 1);                  /* antennas */
        stl_le_p(rb + 18, 0x18094603);         /* release 9.70.3.p24 */
        break;
    case MRVL_CMD_802_11_SCAN: {
        bool hit = mrvl_scan_hears_ap(c, body, blen);
        memset(rb, 0, sizeof(r) - MRVL_CMD_HDR);
        rlen = MRVL_CMD_HDR + mrvl_scan_response(c, rb, hit);
        mrvl_trace("[MRVL] scan: %s\n", hit ? "the access point answers" : "nothing on these channels");
        break;
    }
    case MRVL_CMD_802_11_AUTHENTICATE:
        rlen = MAX(rlen, MRVL_CMD_HDR + 8);
        rb[7] = blen >= 6 && !memcmp(body, c->bssid, 6) ? 0 : 1;
        break;
    case MRVL_CMD_802_11_ASSOCIATE:
    case MRVL_CMD_802_11_ASSOCIATE_OLD: {
        bool ours = blen >= 6 && !memcmp(body, c->bssid, 6);
        memset(rb, 0, sizeof(r) - MRVL_CMD_HDR);
        stw_le_p(rb + 0, 0x0001);              /* capability */
        stw_le_p(rb + 2, ours ? 0 : 1);        /* IEEE status */
        stw_le_p(rb + 4, ours ? 0xc001 : 0);   /* AID 1 */
        rlen = MRVL_CMD_HDR + 6 + mrvl_ap_ies(rb + 6);
        c->associated = ours;
        /* Both requests are answered as the old one: the driver only
         * handles 0x8012 ("Unhandled Command Packet" for 0x8050). */
        code = MRVL_CMD_802_11_ASSOCIATE_OLD;
        mrvl_trace("[MRVL] associate: %s\n", ours ? "joined" : "unknown BSSID");
        break;
    }
    case MRVL_CMD_802_11_DEAUTHENTICATE:
        c->associated = false;
        break;
    case MRVL_CMD_802_11_RSSI:
        rlen = MAX(rlen, MRVL_CMD_HDR + 8);
        stw_le_p(rb + 0, 47);                  /* SNR */
        stw_le_p(rb + 2, 92);                  /* noise floor, -dBm */
        stw_le_p(rb + 4, 47);
        stw_le_p(rb + 6, 92);
        break;
    case MRVL_CMD_802_11_MAC_ADDR:
        rlen = MAX(rlen, MRVL_CMD_HDR + 8);
        if (!lduw_le_p(rb)) {
            memcpy(rb + 2, c->mac, 6);
        }
        break;
    case MRVL_CMD_802_11_RF_TX_POWER:
        rlen = MAX(rlen, MRVL_CMD_HDR + 6);
        if (!lduw_le_p(rb)) {
            stw_le_p(rb + 2, 13);
            rb[4] = 18;
            rb[5] = 5;
        }
        break;
    case MRVL_CMD_802_11_BG_SCAN_QUERY:
        rlen = MRVL_CMD_HDR + 4 + 3;             /* nothing found */
        memset(rb, 0, 7);
        break;
    default:
        break;                                   /* accepted as sent */
    }
    stw_le_p(r, code | MRVL_CMD_RESP);
    stw_le_p(r + 2, rlen);
    stw_le_p(r + 6, 0);                        /* result: success */
    uint8_t *p = mrvl_upq_push(c, MRVL_TYPE_CMD, rlen);
    if (p) {
        memcpy(p, r, rlen);
    }
}

/* A data frame from the host: a TxPD, then the Ethernet frame it points at. */
static void mrvl_handle_tx(Mrvl8686Card *c, const uint8_t *pd, uint32_t n)
{
    if (n < MRVL_TXPD_LEN) {
        return;
    }
    uint32_t loc = ldl_le_p(pd + 8), len = lduw_le_p(pd + 12);
    if (loc > n || len > n - loc || len < 14 || !c->associated) {
        return;
    }
    c->tx_frames++;
    mrvl_card_send(c, pd + loc, len);
}

/* A frame from the network: an RxPD and the frame as the radio has it,
 * 802.3 with an RFC 1042 SNAP header, which the driver takes back off. */
static bool mrvl_card_rx(Mrvl8686Card *c, const uint8_t *eth, uint32_t len)
{
    if (c->stage != MRVL_STAGE_FIRMWARE || !c->associated || c->asleep || len < 14) {
        return false;
    }
    uint16_t type = eth[12] << 8 | eth[13];
    bool snap = type >= 0x600;
    uint32_t plen = len + (snap ? 8 : 0);
    uint8_t *p = mrvl_upq_push(c, MRVL_TYPE_DATA, MRVL_RXPD_LEN + plen);
    if (!p) {
        return false;
    }
    stw_le_p(p + 0, 0x0001);                   /* status: OK */
    p[2] = 47;                                   /* SNR */
    stw_le_p(p + 4, plen);
    p[6] = 92;                                   /* noise floor */
    p[7] = 11;                                   /* 54 Mb/s */
    stl_le_p(p + 8, MRVL_RXPD_LEN);            /* packet location */
    uint8_t *f = p + MRVL_RXPD_LEN;
    if (snap) {
        memcpy(f, eth, 12);
        f[12] = (len - 14 + 8) >> 8;
        f[13] = len - 14 + 8;
        memcpy(f + 14, "\xaa\xaa\x03\x00\x00\x00", 6);
        memcpy(f + 20, eth + 12, len - 12);
    } else {
        memcpy(f, eth, len);
    }
    c->rx_frames++;
    return true;
}

static void mrvl_build_eeprom(Mrvl8686Card *c)
{
    static const uint8_t header[8] = { 0xde, 0xad, 0x00, 0x04, 0xbe, 0xef, 0xca, 0xfe };
    uint8_t *p = c->eeprom;
    memset(c->eeprom, 0xff, sizeof(c->eeprom));
    memcpy(p, header, 8);
    p += 8;
    /* Records: key and length (16-bit words, the 4-byte record header
     * included), big endian. Key 2: the Wi-Fi MAC. */
    *p++ = 0; *p++ = 2; *p++ = 0; *p++ = 5;
    memcpy(p, c->mac, 6);
    p += 6;
    /* Key 1: TX calibration, which the driver passes to the firmware
     * (CMD_802_11_CAL_DATA_EXT) unless it is all zeros or all ones. */
    *p++ = 0; *p++ = 1; *p++ = 0; *p++ = 2 + 0x80 / 2;
    for (int i = 0; i < 0x80; i++) {
        *p++ = 0x40 + (i & 0x1f);
    }
}

/* The unit's MAC, into the EEPROM the driver reads it from (key 2, then the nub's
 * local-mac-address) and the firmware's answers; all zeros (no "mac", i.e. no
 * machine wifi-mac) is an Apple-OUI placeholder, the same on every unit. */
static void mrvl_card_set_mac(Mrvl8686Card *c, const uint8_t *mac)
{
    static const uint8_t placeholder[6] = { 0x00, 0x1b, 0x63, 0x45, 0x1e, 0x01 };
    static const uint8_t unset[6];

    memcpy(c->mac, memcmp(mac, unset, 6) ? mac : placeholder, 6);
    mrvl_build_eeprom(c);
}

static void mrvl_card_reset(Mrvl8686Card *c)
{
    c->stage = MRVL_STAGE_BOOTROM;
    c->config = c->hint_mask = c->hint_status = c->scratch_err = 0;
    c->rd_base = c->scratch = 0;
    c->ul_rdy = c->fw_want_data = c->asleep = c->associated = false;
    c->helper_bytes = c->fw_bytes = c->fw_blocks = 0;
    c->getmem_len = 0;
    c->getmem_off = 0;
    c->upq_head = c->upq_count = 0;
}

static uint8_t mrvl_card_readb(Mrvl8686Card *c, uint32_t addr)
{
    switch (addr) {
    case MRVL_REG_IOPORT:
    case MRVL_REG_IOPORT + 1:
    case MRVL_REG_IOPORT + 2:
        return MRVL_IOPORT >> (8 * (addr - MRVL_REG_IOPORT));
    case MRVL_REG_CONFIG:
        return c->config;
    case MRVL_REG_HINT_MASK:
        return c->hint_mask;
    case MRVL_REG_HINT_STATUS:
        return c->hint_status;
    case MRVL_REG_RD_BASE:
        return c->rd_base;
    case MRVL_REG_RD_BASE + 1:
        return c->rd_base >> 8;
    case MRVL_REG_CARD_STATUS:
        return MRVL_STATUS_IO_RDY | MRVL_STATUS_CIS_RDY | MRVL_STATUS_DL_RDY |
               (c->ul_rdy || (c->stage == MRVL_STAGE_FIRMWARE && c->upq_count) ?
                MRVL_STATUS_UL_RDY : 0);
    case MRVL_REG_SCRATCH:
        return c->scratch;
    case MRVL_REG_SCRATCH + 1:
        return c->scratch >> 8;
    case MRVL_REG_SCRATCH_ERR:
        return c->scratch_err;
    default:
        return 0;
    }
}

static void mrvl_card_writeb(Mrvl8686Card *c, uint32_t addr, uint8_t val)
{
    switch (addr) {
    case MRVL_REG_CONFIG:
        c->config = val;
        if (c->asleep && (val & MRVL_CONFIG_HOST_PWR_UP)) {
            /* Awake: it says so, and takes packets again. */
            c->asleep = false;
            mrvl_event(c, MRVL_EVENT_DEEP_SLEEP_AWAKE);
            c->hint_status |= MRVL_HINT_DN_LD;
        }
        break;
    case MRVL_REG_HINT_MASK:
        c->hint_mask = val;
        break;
    case MRVL_REG_HINT_STATUS:
        c->hint_status &= val;
        break;
    default:
        break;
    }
}

/* The helper's view of a 16-byte request: an EEPROM read or a block header. */
static void mrvl_helper_request(Mrvl8686Card *c, const uint8_t *b)
{
    uint32_t cmd = ldl_le_p(b);

    if (cmd == MRVL_HELPER_GETMEM) {
        uint32_t off = ldl_le_p(b + 8), len = lduw_le_p(b + 6);
        c->getmem_off = MIN(off, MRVL_EEPROM_SIZE);
        c->getmem_len = MIN(len, MRVL_EEPROM_SIZE - c->getmem_off);
        c->scratch = c->getmem_len;
        c->scratch_err = 0;
        c->ul_rdy = true;
        return;
    }
    if (mrvl_crc32(b, 12) != ldl_be_p(b + 12)) {
        c->rd_base = MRVL_FW_HDR_LEN | 1;           /* CRC error */
        return;
    }
    if (cmd == MRVL_FW_CMD_LAST) {
        c->fw_blocks++;
        c->stage = MRVL_STAGE_FIRMWARE;
        c->rd_base = 0;
        c->scratch = MRVL_FIRMWARE_OK;
        mrvl_trace("[MRVL] firmware up: %u blocks, %u bytes\n", c->fw_blocks, c->fw_bytes);
        return;
    }
    uint32_t len = ldl_le_p(b + 8);
    if (cmd != MRVL_FW_CMD_DATA || len < 4 || len > 0x800) {
        c->rd_base = MRVL_FW_HDR_LEN | 1;
        return;
    }
    c->rd_base = len;
    c->fw_want_data = true;
}

static void mrvl_card_write(Mrvl8686Card *c, const uint8_t *buf, uint32_t len)
{
    switch (c->stage) {
    case MRVL_STAGE_BOOTROM:
        if (len >= 4) {
            uint32_t n = ldl_le_p(buf);
            if (!n) {
                mrvl_trace("[MRVL] helper up (%u bytes)\n", c->helper_bytes);
                c->stage = MRVL_STAGE_HELPER;
                c->rd_base = MRVL_FW_HDR_LEN;
            } else {
                c->helper_bytes = MIN(c->helper_bytes + MIN(n, len - 4), MRVL_HELPER_MAX);
            }
        }
        break;
    case MRVL_STAGE_HELPER:
        if (!c->fw_want_data) {
            if (len >= MRVL_FW_HDR_LEN) {
                mrvl_helper_request(c, buf);
            }
        } else if (len >= c->rd_base) {
            c->fw_want_data = false;
            if (mrvl_crc32(buf, c->rd_base - 4) != ldl_be_p(buf + c->rd_base - 4)) {
                c->rd_base = MRVL_FW_HDR_LEN | 1;
            } else {
                c->fw_bytes += c->rd_base;
                c->fw_blocks++;
                c->rd_base = MRVL_FW_HDR_LEN;
            }
        }
        break;
    case MRVL_STAGE_FIRMWARE:
        if (len >= MRVL_SDIO_HDR && !c->asleep) {
            uint32_t plen = MIN(lduw_le_p(buf), len);
            uint16_t type = lduw_le_p(buf + 2);
            if (plen >= MRVL_SDIO_HDR && type == MRVL_TYPE_CMD) {
                mrvl_handle_cmd(c, buf + MRVL_SDIO_HDR, plen - MRVL_SDIO_HDR);
            } else if (plen >= MRVL_SDIO_HDR && type == MRVL_TYPE_DATA) {
                mrvl_handle_tx(c, buf + MRVL_SDIO_HDR, plen - MRVL_SDIO_HDR);
            }
        }
        /* Ready for the next packet, unless that one put the firmware to sleep. */
        if (!c->asleep) {
            c->hint_status |= MRVL_HINT_DN_LD;
        }
        break;
    }
}

static void mrvl_card_read(Mrvl8686Card *c, uint8_t *buf, uint32_t len)
{
    memset(buf, 0, len);
    if (c->stage == MRVL_STAGE_HELPER && c->ul_rdy) {
        memcpy(buf, c->eeprom + c->getmem_off, MIN(len, c->getmem_len));
        c->ul_rdy = false;
    } else if (c->stage == MRVL_STAGE_FIRMWARE && c->upq_count) {
        memcpy(buf, c->upq[c->upq_head], MIN(len, c->upq_len[c->upq_head]));
        c->upq_head = (c->upq_head + 1) % MRVL_UPQ_SLOTS;
        c->upq_count--;
        mrvl_upq_kick(c);
    }
}

/* mrvl8686 core: end */

static void mrvl_card_send(Mrvl8686Card *c, const uint8_t *frame, uint32_t len)
{
    Mrvl8686State *s = container_of(c, Mrvl8686State, card);
    if (s->nic) {
        qemu_send_packet(qemu_get_queue(s->nic), frame, len);
    }
}

static void mrvl8686_update_irq(Mrvl8686State *s)
{
    qemu_set_irq(s->irq, mrvl8686_irq_pending(s));
}

bool mrvl8686_irq_pending(Mrvl8686State *s)
{
    return s->card.hint_status & s->card.hint_mask;
}

uint8_t mrvl8686_readb(Mrvl8686State *s, uint32_t addr)
{
    return mrvl_card_readb(&s->card, addr);
}

void mrvl8686_writeb(Mrvl8686State *s, uint32_t addr, uint8_t val)
{

    mrvl_card_writeb(&s->card, addr, val);
    if (addr == MRVL_REG_HINT_STATUS && mrvl8686_irq_pending(s)) {
        /* Still asserted after the host's clear (a packet queued behind the
         * one it handled): the host sees DAT1 go low and high again. */
        qemu_irq_lower(s->irq);
    }
    mrvl8686_update_irq(s);
}

void mrvl8686_read(Mrvl8686State *s, uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (addr != MRVL_IOPORT) {
        memset(buf, 0, len);
        return;
    }
    mrvl_card_read(&s->card, buf, len);
    mrvl8686_update_irq(s);
    if (s->nic && s->card.upq_count < MRVL_UPQ_SLOTS / 2) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

void mrvl8686_write(Mrvl8686State *s, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (addr == MRVL_IOPORT) {
        mrvl_card_write(&s->card, buf, len);
        mrvl8686_update_irq(s);
    }
}

void mrvl8686_card_reset(Mrvl8686State *s)
{
    mrvl_card_reset(&s->card);
    mrvl8686_update_irq(s);
}

static bool mrvl8686_can_receive(NetClientState *nc)
{
    Mrvl8686State *s = qemu_get_nic_opaque(nc);
    return s->card.associated && s->card.upq_count < MRVL_UPQ_SLOTS / 2;
}

static ssize_t mrvl8686_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    Mrvl8686State *s = qemu_get_nic_opaque(nc);
    if (size <= MRVL_UPQ_BYTES && mrvl_card_rx(&s->card, buf, size)) {
        mrvl8686_update_irq(s);
    }
    return size;
}

static NetClientInfo mrvl8686_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mrvl8686_can_receive,
    .receive = mrvl8686_receive,
};

void mrvl8686_setup_net(Mrvl8686State *s)
{
    NetClientState *peer = qemu_find_netdev("wifi0");
    if (!peer) {
        return;
    }
    memcpy(s->conf.macaddr.a, s->card.mac, 6);
    s->conf.peers.ncs[0] = peer;
    s->conf.peers.queues = 1;
    s->nic = qemu_new_nic(&mrvl8686_net_info, &s->conf, TYPE_MRVL8686, "wifi",
                          &DEVICE(s)->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void mrvl8686_init(Object *obj)
{
    Mrvl8686State *s = MRVL8686(obj);
    static const uint8_t bssid[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x01 };

    memcpy(s->card.bssid, bssid, 6);
    mrvl_card_reset(&s->card);
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
}

static void mrvl8686_realize(DeviceState *dev, Error **errp)
{
    Mrvl8686State *s = MRVL8686(dev);
    mrvl_card_set_mac(&s->card, s->conf.macaddr.a);
}

static const Property mrvl8686_properties[] = {
    DEFINE_PROP_MACADDR("mac", Mrvl8686State, conf.macaddr),
};

static int mrvl8686_post_load(void *opaque, int version_id)
{
    Mrvl8686State *s = opaque;
    Mrvl8686Card *c = &s->card;
    if (c->stage > MRVL_STAGE_FIRMWARE || c->upq_head >= MRVL_UPQ_SLOTS ||
        c->upq_count > MRVL_UPQ_SLOTS || c->getmem_off > MRVL_EEPROM_SIZE ||
        c->getmem_len > MRVL_EEPROM_SIZE - c->getmem_off) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < MRVL_UPQ_SLOTS; i++) {
        if (c->upq_len[i] > MRVL_UPQ_BYTES) {
            return -EINVAL;
        }
    }
    mrvl8686_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_mrvl8686 = {
    .name = TYPE_MRVL8686,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mrvl8686_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(card.stage, Mrvl8686State),
        VMSTATE_UINT8(card.config, Mrvl8686State),
        VMSTATE_UINT8(card.hint_mask, Mrvl8686State),
        VMSTATE_UINT8(card.hint_status, Mrvl8686State),
        VMSTATE_UINT8(card.scratch_err, Mrvl8686State),
        VMSTATE_UINT16(card.rd_base, Mrvl8686State),
        VMSTATE_UINT16(card.scratch, Mrvl8686State),
        VMSTATE_BOOL(card.ul_rdy, Mrvl8686State),
        VMSTATE_UINT32(card.helper_bytes, Mrvl8686State),
        VMSTATE_BOOL(card.fw_want_data, Mrvl8686State),
        VMSTATE_UINT32(card.fw_bytes, Mrvl8686State),
        VMSTATE_UINT32(card.fw_blocks, Mrvl8686State),
        VMSTATE_UINT16(card.getmem_len, Mrvl8686State),
        VMSTATE_UINT32(card.getmem_off, Mrvl8686State),
        VMSTATE_BOOL(card.asleep, Mrvl8686State),
        VMSTATE_BOOL(card.associated, Mrvl8686State),
        VMSTATE_UINT32(card.tx_frames, Mrvl8686State),
        VMSTATE_UINT32(card.rx_frames, Mrvl8686State),
        VMSTATE_UINT8_2DARRAY(card.upq, Mrvl8686State, MRVL_UPQ_SLOTS, MRVL_UPQ_BYTES),
        VMSTATE_UINT16_ARRAY(card.upq_len, Mrvl8686State, MRVL_UPQ_SLOTS),
        VMSTATE_UINT32(card.upq_head, Mrvl8686State),
        VMSTATE_UINT32(card.upq_count, Mrvl8686State),
        VMSTATE_END_OF_LIST()
    },
};

static void mrvl8686_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_mrvl8686;
    dc->realize = mrvl8686_realize;
    device_class_set_props(dc, mrvl8686_properties);
    dc->desc = "Marvell 88W8686 SDIO Wi-Fi card";
}

static const TypeInfo mrvl8686_type_info = {
    .name = TYPE_MRVL8686,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(Mrvl8686State),
    .instance_init = mrvl8686_init,
    .class_init = mrvl8686_class_init,
};

static void mrvl8686_register_types(void)
{
    type_register_static(&mrvl8686_type_info);
}

type_init(mrvl8686_register_types)
