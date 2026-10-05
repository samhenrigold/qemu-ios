/*
 * Fake cellular baseband core: H5 link, 27.010 basic mux, AT engine, and the small
 * network/SIM/call/SMS model behind it. What the host sends and parses is documented in
 * docs/baseband/commcenter-1.0.md and (calls/SMS byte level) in the qemu-ios-files
 * commcenter-1.0-calls-sms.md; this file answers exactly those parsers.
 *
 * Honest limitations, on purpose: no audio, no packet data (+CGACT fails, there is no
 * network behind it), no USSD, no STK, no cell broadcast bodies. Unknown AT commands
 * answer OK (traced with IOS_BB_TRACE) so CommCenter's init never stalls on a command
 * the model has not met; commands whose replies 1.0 actually parses are all here.
 *
 * Transport recap: the kernel's AppleReliableSerialLayer turns the baseband UART into
 * H5 (SLIP, SYNC/SYNC RESP/CONFIG/CONFIG RESP with cfg 0x17, reliable data type 14,
 * pure acks type 0, the kext's CRC) the moment it snoops "at+xtransportmode". CommCenter
 * then runs the 27.010 mux inside H5 (SABM DLCI 0..5, UIH carries "at<cmd>\r" lines).
 * Pre-H5 and pre-mux the same AT engine serves channel 0.
 */

#include "qemu/osdep.h"
#include "hw/misc/ios_baseband_core.h"

#define IOS_BB_TRACE_ENV "IOS_BB_TRACE"
#define TRACE(...) do { \
    if (getenv(IOS_BB_TRACE_ENV)) { \
        fprintf(stderr, "ios-bb: " __VA_ARGS__); \
    } \
} while (0)

/* ------------------------------------------------------------------ H5 (3-wire UART) */

#define H5_TYPE_ACK    0
#define H5_TYPE_DATA   14          /* what AppleReliableSerialLayer carries the stream in */
#define H5_TYPE_LINK   15
#define H5_CONFIG      0x17        /* window 7, no OOF, data-integrity CRC */
#define H5_RETX_MS     250
#define H5_MAX_PAYLOAD 1024        /* kernel rx buffer is 1507 bytes */

#define H5_LINK_SYNC       0x01
#define H5_LINK_SYNC_RESP  0x02
#define H5_LINK_CONFIG     0x03
#define H5_LINK_CONFIG_RESP 0x04

static uint16_t h5_crc_table[256];

/*
 * The kext's table is the reflected CCITT (0x8408) table with each entry byte-swapped,
 * run MSB-first, and the result bit-reversed. Not textbook H5; reproduced from the kext
 * (docs/baseband/commcenter-1.0.md, golden vector "123456789" -> 0xf689).
 */
static void h5_crc_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint16_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
        }
        h5_crc_table[i] = (uint16_t)((c >> 8) | (c << 8));
    }
}

uint16_t ios_bb_h5_crc(const uint8_t *p, size_t n)
{
    uint16_t c = 0xffff, r = 0;

    if (!h5_crc_table[1]) {
        h5_crc_init();
    }
    while (n--) {
        c = h5_crc_table[(*p++ ^ (c >> 8)) & 0xff] ^ (uint16_t)(c << 8);
    }
    for (int i = 0; i < 16; i++) {
        r = (r << 1) | ((c >> i) & 1);
    }
    return r;
}

/* One built H5 packet (header + payload [+ crc]); kept for retransmission. */
static void h5_send_wire(IosBbCore *bb, const uint8_t *pkt, unsigned n)
{
    uint8_t wire[2 * (4 + H5_MAX_PAYLOAD + 2) + 2];
    uint8_t *o = wire;
    unsigned i;

    *o++ = 0xc0;
    for (i = 0; i < n; i++) {
        if (pkt[i] == 0xc0) {
            *o++ = 0xdb;
            *o++ = 0xdc;
        } else if (pkt[i] == 0xdb) {
            *o++ = 0xdb;
            *o++ = 0xdd;
        } else {
            *o++ = pkt[i];
        }
    }
    *o++ = 0xc0;
    if (getenv(IOS_BB_TRACE_ENV) && atoi(getenv(IOS_BB_TRACE_ENV)) >= 4) {
        TRACE("h5 tx %u:", n);
        for (i = 0; i < n && i < 12; i++) {
            fprintf(stderr, " %02x", pkt[i]);
        }
        fprintf(stderr, "\n");
    }
    bb->out(bb->opaque, wire, o - wire);
}

/*
 * Build and send one H5 packet. Reliable data also lands in h5_unacked for
 * retransmission; the caller has made room (window 7).
 */
static void h5_send_pkt(IosBbCore *bb, int type, bool reliable, uint8_t seq,
                        const uint8_t *data, unsigned len)
{
    uint8_t pkt[4 + H5_MAX_PAYLOAD + 2];
    bool crc = bb->h5_crc && type != H5_TYPE_LINK;
    unsigned n = 4 + len;

    if (len > H5_MAX_PAYLOAD) {
        return;
    }
    pkt[0] = seq | (bb->h5_rx_next << 3) | (crc << 6) | (reliable << 7);
    pkt[1] = type | ((len & 0xf) << 4);
    pkt[2] = len >> 4;
    pkt[3] = ~(pkt[0] + pkt[1] + pkt[2]);
    memcpy(pkt + 4, data, len);
    if (crc) {
        uint16_t c = ios_bb_h5_crc(pkt, n);
        pkt[n++] = c >> 8;
        pkt[n++] = c;
    }
    h5_send_wire(bb, pkt, n);
    bb->h5_need_ack = false;
    bb->h5_last_tx_ms = bb->now_ms;
}

static void h5_send_ack(IosBbCore *bb)
{
    if (bb->h5_active) {
        h5_send_pkt(bb, H5_TYPE_ACK, false, 0, NULL, 0);
    }
    bb->h5_need_ack = false;
}

/* Queue a reliable data packet; sends what the window allows. */
static void h5_queue(IosBbCore *bb, const uint8_t *data, unsigned len)
{
    if (len > H5_MAX_PAYLOAD) {
        len = H5_MAX_PAYLOAD;
    }
    if (bb->h5_txq_len + 2u + len > sizeof(bb->h5_txq)) {
        TRACE("h5 tx queue overflow, dropping %u bytes\n", len);
        return;
    }
    bb->h5_txq[bb->h5_txq_len++] = len;
    bb->h5_txq[bb->h5_txq_len++] = len >> 8;
    memcpy(bb->h5_txq + bb->h5_txq_len, data, len);
    bb->h5_txq_len += len;
}

static void h5_send_one_unacked(IosBbCore *bb, const uint8_t *data, unsigned len)
{
    IosBbH5Pkt *p = &bb->h5_unacked[bb->h5_nunacked++];

    p->seq = bb->h5_tx_seq;
    p->len = len;
    memcpy(p->data, data, len);
    h5_send_pkt(bb, H5_TYPE_DATA, true, bb->h5_tx_seq, data, len);
    bb->h5_tx_seq = (bb->h5_tx_seq + 1) & 7;
}

static void h5_pump(IosBbCore *bb)
{
    if (!bb->h5_active) {
        return;
    }
    while (bb->h5_nunacked < IOS_BB_H5_WINDOW && bb->h5_txq_len >= 2) {
        unsigned len = bb->h5_txq[0] | (bb->h5_txq[1] << 8);

        if (2u + len > bb->h5_txq_len) {
            break;
        }
        h5_send_one_unacked(bb, bb->h5_txq + 2, len);
        memmove(bb->h5_txq, bb->h5_txq + 2 + len, bb->h5_txq_len - 2 - len);
        bb->h5_txq_len -= 2 + len;
    }
}

static void h5_retransmit(IosBbCore *bb)
{
    for (unsigned i = 0; i < bb->h5_nunacked; i++) {
        h5_send_pkt(bb, H5_TYPE_DATA, true, bb->h5_unacked[i].seq,
                    bb->h5_unacked[i].data, bb->h5_unacked[i].len);
    }
}

/*
 * The payload of a reliable data packet: mux bytes once the mux is up, otherwise the
 * "default" channel's raw AT stream.
 */
static void h5_rx_payload(IosBbCore *bb, const uint8_t *data, unsigned len);

/* A whole SLIP-unframed H5 packet from the host. */
static void h5_rx_pkt(IosBbCore *bb, const uint8_t *p, unsigned n)
{
    unsigned len, type, seq, ack;

    if (n < 4 || (uint8_t)(p[0] + p[1] + p[2]) != (uint8_t)~p[3]) {
        TRACE("h5 bad header checksum\n");
        return;
    }
    len = (p[1] >> 4) | (p[2] << 4);
    if (len > 1500 || 4u + len + ((p[0] >> 6) & 1) * 2u != n) {
        TRACE("h5 bad length %u (n %u)\n", len, n);
        return;
    }
    if (p[0] & 0x40) {
        uint16_t c = ios_bb_h5_crc(p, 4 + len);
        uint16_t r = (p[4 + len] << 8) | p[4 + len + 1];
        /* 1.0's boot kernel sends it low byte first (its restore kernel, which the
         * notes were read from, high byte first): either is the same CRC. */
        if (c != r && c != (uint16_t)((r << 8) | (r >> 8))) {
            TRACE("h5 crc mismatch %04x != %04x\n", c, r);
            return;
        }
    }
    type = p[1] & 0xf;
    seq = p[0] & 7;
    ack = (p[0] >> 3) & 7;
    if (getenv(IOS_BB_TRACE_ENV) && atoi(getenv(IOS_BB_TRACE_ENV)) >= 3) {
        TRACE("h5 rx type %u seq %u ack %u rel %u len %u: %02x %02x %02x %02x\n", type, seq, ack, p[0] >> 7, len,
              len > 0 ? p[4] : 0, len > 1 ? p[5] : 0, len > 2 ? p[6] : 0, len > 3 ? p[7] : 0);
    }

    /* Any packet advances the window for whatever it acknowledges. */
    if (bb->h5_nunacked) {
        unsigned d = (ack - bb->h5_unacked[0].seq) & 7;

        if (d <= bb->h5_nunacked) {
            if (d) {
                memmove(bb->h5_unacked, bb->h5_unacked + d,
                        (bb->h5_nunacked - d) * sizeof(bb->h5_unacked[0]));
                bb->h5_nunacked -= d;
            }
            h5_pump(bb);
        }
    }

    if (type == H5_TYPE_LINK) {
        switch (p[4]) {
        case H5_LINK_SYNC:
            /* 01 7e */
            h5_send_pkt(bb, H5_TYPE_LINK, false, 0, (const uint8_t[]){ 0x02, 0x7d }, 2);
            break;
        case H5_LINK_CONFIG:
            /* 03 fc <cfg>. 1.0's kernel sends 03 fc alone (no configuration
             * field, so no data integrity check) and takes its send window from
             * the configuration in our response: without one it is 0 and it
             * waits forever "for remote window to open". Answer window 7. */
            bb->h5_crc = len >= 3 && (p[6] & 0x10);
            h5_send_pkt(bb, H5_TYPE_LINK, false, 0,
                        (const uint8_t[]){ 0x04, 0x7b, len >= 3 ? p[6] : IOS_BB_H5_WINDOW }, 3);
            bb->h5_active = true;
            TRACE("h5 active (crc %d)\n", bb->h5_crc);
            h5_pump(bb);
            break;
        default:
            break;
        }
        return;
    }
    if (!bb->h5_active) {
        return;
    }
    if (type == H5_TYPE_ACK) {
        return;
    }
    if (type != H5_TYPE_DATA || !(p[0] & 0x80)) {
        /* Data must be reliable; the kernel rejects ours otherwise. */
        return;
    }
    if (seq == bb->h5_rx_next) {
        bb->h5_rx_next = (seq + 1) & 7;
        h5_rx_payload(bb, p + 4, len);
    } else {
        /* Retransmission of something we already have: just re-ack. */
        bb->h5_need_ack = true;
    }
}

static void h5_input(IosBbCore *bb, const uint8_t *buf, size_t len)
{
    if (getenv(IOS_BB_TRACE_ENV) && atoi(getenv(IOS_BB_TRACE_ENV)) >= 4) {
        TRACE("h5 raw %zu:", len);
        for (size_t i = 0; i < len && i < 24; i++) {
            fprintf(stderr, " %02x", buf[i]);
        }
        fprintf(stderr, "\n");
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];

        if (!bb->h5_in_frame) {
            if (c == 0xc0) {
                if (bb->h5_rxlen) {
                    h5_rx_pkt(bb, bb->h5_rx, bb->h5_rxlen);
                }
                bb->h5_rxlen = 0;
                continue;
            }
            bb->h5_in_frame = true;
        }
        if (bb->h5_esc) {
            bb->h5_esc = false;
            if (c == 0xdc) {
                c = 0xc0;
            } else if (c == 0xdd) {
                c = 0xdb;
            } else {
                bb->h5_in_frame = false;
                bb->h5_rxlen = 0;
                continue;
            }
        } else if (c == 0xc0) {
            h5_rx_pkt(bb, bb->h5_rx, bb->h5_rxlen);
            bb->h5_rxlen = 0;
            bb->h5_in_frame = false;
            continue;
        } else if (c == 0xdb) {
            bb->h5_esc = true;
            continue;
        }
        if (bb->h5_rxlen < sizeof(bb->h5_rx)) {
            bb->h5_rx[bb->h5_rxlen++] = c;
        } else {
            bb->h5_rxlen = 0;
            bb->h5_in_frame = false;
        }
    }
}

/* ------------------------------------------------------------ 27.010 basic mode mux */

#define MX_FLAG   0xf9
#define MX_EA     0x01
#define MX_CR     0x02
#define MX_PF     0x10
#define MX_UIH    0xef
#define MX_UI     0x03
#define MX_SABM   0x2f
#define MX_UA     0x63
#define MX_DM     0x0f
#define MX_DISC   0x43

#define MX_INIT_FCS 0xff
#define MX_GOOD_FCS 0xcf

static uint8_t mx_fcs_table[256];

static void mx_fcs_init(void)
{
    /* TS 27.010: g(x) = x^8 + x^2 + x + 1, reflected 0xE0, init 0xFF. */
    for (int i = 0; i < 256; i++) {
        uint8_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (c >> 1) ^ 0xe0 : c >> 1;
        }
        mx_fcs_table[i] = c;
    }
}

static uint8_t mx_fcs(uint8_t fcs, const uint8_t *p, unsigned n)
{
    while (n--) {
        fcs = mx_fcs_table[fcs ^ *p++];
    }
    return fcs;
}

/* Control channel command type bytes (bit 1 = C/R of the control message). */
#define MX_CMD_MSC     0xe3
#define MX_CMD_NSC     0x0b
#define MX_CMD_TEST    0x13
#define MX_CMD_PSC     0x23
#define MX_CMD_RLS     0x2b
#define MX_CMD_FCOFF   0x33
#define MX_CMD_FCON    0x53
#define MX_CMD_CLD     0x63

static void h5_stream(IosBbCore *bb, const uint8_t *data, unsigned len);

/*
 * One finished frame out. Inside H5 each frame is its own packet (1.0, as the
 * modem stream found it); on a bare byte stream (SPI, the kernel's
 * AppleSerialMultiplexer, which asserts a leading 0xF9) frames carry their flags.
 */
/* Every frame between its own flags: the bare (SPI) stream, and inside H5 as well,
 * where 1.0's CommCenter frames its own with flags and fails ours without them
 * ("No trailing flag at end of frame"). */
static void mx_emit(IosBbCore *bb, uint8_t *f, unsigned n)
{
    memmove(f + 1, f, n);
    f[0] = f[n + 1] = MX_FLAG;
    n += 2;
    h5_stream(bb, f, n);
}

/* Send one mux frame (address, control, length, info, FCS) as one H5 packet. */
static void mx_send(IosBbCore *bb, unsigned dlci, bool cr, uint8_t ctrl,
                    const uint8_t *data, unsigned len)
{
    uint8_t f[5 + 1600 + 1 + 2];
    unsigned n = 0, hdr;
    uint8_t fcs;

    if (!mx_fcs_table[1]) {
        mx_fcs_init();
    }
    f[n++] = (dlci << 2) | (cr << 1) | MX_EA;
    f[n++] = ctrl;
    if (len < 128) {
        f[n++] = (len << 1) | MX_EA;
    } else {
        f[n++] = (len & 0x7f) << 1;
        f[n++] = len >> 7;
    }
    hdr = n;
    if (len) {
        memcpy(f + n, data, len);
        n += len;
    }
    /* 27.010: the UIH FCS covers address, control and length only; the
     * other frames' covers the information field too. */
    fcs = mx_fcs(MX_INIT_FCS, f, hdr);
    if ((ctrl & ~MX_PF) != MX_UIH) {
        fcs = mx_fcs(fcs, data, len);
    }
    f[n++] = 0xff - fcs;
    mx_emit(bb, f, n);
}

static void mx_send_frame_canned(IosBbCore *bb, unsigned dlci, uint8_t ctrl)
{
    /* S-frames carry a zero-length field in basic mode. */
    uint8_t f[5 + 2];
    unsigned n = 0;
    uint8_t fcs;

    if (!mx_fcs_table[1]) {
        mx_fcs_init();
    }
    /* 27.010 5.2.1.2: a responder's response carries C/R = 1. The 1.0 path keeps
     * the 0 it was tested with; the kernel mux (bare stream) gets the spec. */
    f[n++] = (dlci << 2) | (!bb->h5 ? MX_CR : 0) | MX_EA;
    f[n++] = ctrl;
    f[n++] = MX_EA;                        /* length 0 */
    fcs = mx_fcs(MX_INIT_FCS, f, n);
    f[n++] = 0xff - fcs;
    mx_emit(bb, f, n);
}

/* Our own V.24 status for a DLCI: RTC|RTR, plus DV (carrier) while it carries data. */
static void mx_msc(IosBbCore *bb, unsigned dlci, bool dv)
{
    uint8_t msc[4] = { MX_CMD_MSC, (2 << 1) | MX_EA, (dlci << 2) | MX_CR | MX_EA,
                       0x0d | (dv ? 0x80 : 0) };

    mx_send(bb, 0, false, MX_UIH, msc, sizeof(msc));
}

/*
 * DLCI 0 control command. We are the responder: echo TEST/MSC/FCON/FCOFF/RLS, ack PSC
 * (power save), ack CLD and drop the multiplexer (back to the AT-only stream), and answer
 * everything else with NSC echoing the unknown type byte.
 */
static void mx_control(IosBbCore *bb, const uint8_t *data, unsigned len)
{
    uint8_t type;
    unsigned clen = 0, dlen = 0;

    if (len < 2) {
        return;
    }
    type = data[0];
    clen = (data[1] >> 1) & 0x7f;
    TRACE("mux control type %02x len %u (%02x %02x)\n", type, clen,
          len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
    if (2u + clen > len) {
        return;
    }
    dlen = clen;
    data += 2;

    uint8_t resp[64];
    unsigned rn = 0;

    switch (type) {
    case MX_CMD_PSC:
        resp[rn++] = 0x21;                 /* PSC response */
        resp[rn++] = MX_EA;                /* length 0 */
        break;
    case MX_CMD_CLD:
        resp[rn++] = 0x61;                 /* CLD response */
        resp[rn++] = MX_EA;
        bb->mux_leave = true;               /* after this frame, AT-only again */
        break;
    case MX_CMD_TEST:
        resp[rn++] = 0x11;
        if (dlen > sizeof(resp) - 2) {
            dlen = sizeof(resp) - 2;
        }
        resp[rn++] = (dlen << 1) | MX_EA;
        memcpy(resp + rn, data, dlen);
        rn += dlen;
        break;
    case MX_CMD_MSC:
    case MX_CMD_RLS:
    case MX_CMD_FCON:
    case MX_CMD_FCOFF:
        /* Echo the command back with C/R clear. */
        resp[rn++] = type & ~MX_CR;
        if (dlen > sizeof(resp) - 2) {
            dlen = sizeof(resp) - 2;
        }
        resp[rn++] = (dlen << 1) | MX_EA;
        memcpy(resp + rn, data, dlen);
        rn += dlen;
        break;
    default:
        resp[rn++] = 0x09;                 /* NSC */
        resp[rn++] = (1 << 1) | MX_EA;
        resp[rn++] = type;
        break;
    }
    mx_send(bb, 0, false, MX_UIH, resp, rn);
    if (type == MX_CMD_MSC && dlen >= 2 && !bb->h5) {
        /*
         * The kernel mux (4.x, bare stream) waits for the modem's own MSC on every
         * DLCI too (its kReceivingModemBits state): RTR|RTC, ready, no carrier yet.
         */
        mx_msc(bb, data[0] >> 2, false);
    }
}

static void at_chan_input(IosBbCore *bb, int ch, const uint8_t *data, unsigned len);

static void mx_rx_frame(IosBbCore *bb, uint8_t addr, uint8_t ctrl,
                        const uint8_t *data, unsigned len)
{
    unsigned dlci = addr >> 2;

    switch (ctrl & ~MX_PF) {
    case MX_SABM:
        if (dlci < IOS_BB_MAX_CH) {
            bb->ch[dlci].open = true;
        }
        mx_send_frame_canned(bb, dlci, MX_UA | (ctrl & MX_PF));
        TRACE("mux SABM dlci %u -> UA\n", dlci);
        if (dlci == bb->xsim_ch && !bb->xsim_pushed) {
            /* The SIM model only moves after it sees the SIM, so poke it once
             * the sms/SIM channel exists. */
            bb->xsim_due_ms = bb->now_ms + 100;
        }
        break;
    case MX_DISC:
        TRACE("mux DISC dlci %u -> UA\n", dlci);
        if (dlci < IOS_BB_MAX_CH) {
            bb->ch[dlci].open = false;
            bb->ch[dlci].len = 0;
            bb->ch[dlci].sms_prompt = false;
            bb->ch[dlci].data_cid = 0;
        }
        mx_send_frame_canned(bb, dlci, MX_UA | (ctrl & MX_PF));
        break;
    case MX_DM:
    case MX_UA:
        TRACE("mux %s dlci %u from the AP\n", (ctrl & ~MX_PF) == MX_UA ? "UA" : "DM", dlci);
        break;                             /* we never initiate */
    case MX_UIH:
    case MX_UI:
        if (dlci == 0) {
            mx_control(bb, data, len);
        } else if (dlci < IOS_BB_MAX_CH && bb->ch[dlci].open) {
            at_chan_input(bb, dlci, data, len);
        } else if (dlci < IOS_BB_MAX_CH) {
            mx_send_frame_canned(bb, dlci, MX_DM | (ctrl & MX_PF));
        }
        break;
    default:
        break;
    }
}

static void mx_input(IosBbCore *bb, const uint8_t *buf, size_t len)
{
    IosBbMuxRx *m = &bb->muxrx;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];

        switch (m->state) {
        case IOS_BB_MX_SEARCH:
            if (c == MX_FLAG) {
                if (m->flags_run >= 1 && !(m->flags_run & 1)) {
                    /* Wake-up flags: answer flags with flags (power-save exit). */
                    uint8_t two[2] = { MX_FLAG, MX_FLAG };
                    h5_stream(bb, two, 2);
                }
                m->flags_run++;
                break;
            }
            m->flags_run = 0;
            m->state = IOS_BB_MX_ADDR;
            /* fall through */
        case IOS_BB_MX_ADDR:
            if (c == MX_FLAG) {
                m->state = IOS_BB_MX_SEARCH;
                m->flags_run = 1;
                break;
            }
            m->fcs = MX_INIT_FCS;
            m->fcs = mx_fcs_table[m->fcs ^ c];
            m->addr = c;
            m->state = (c & MX_EA) ? IOS_BB_MX_CTRL : IOS_BB_MX_ADDR;
            break;
        case IOS_BB_MX_CTRL:
            m->fcs = mx_fcs_table[m->fcs ^ c];
            m->ctrl = c;
            m->state = IOS_BB_MX_LEN0;
            break;
        case IOS_BB_MX_LEN0:
            m->fcs = mx_fcs_table[m->fcs ^ c];
            if (c & MX_EA) {
                m->len = c >> 1;
                m->cnt = 0;
                m->state = m->len ? IOS_BB_MX_DATA : IOS_BB_MX_FCS;
            } else {
                m->len = (c >> 1) & 0x7f;
                m->state = IOS_BB_MX_LEN1;
            }
            break;
        case IOS_BB_MX_LEN1:
            m->fcs = mx_fcs_table[m->fcs ^ c];
            m->len |= c << 7;
            m->cnt = 0;
            m->state = m->len ? IOS_BB_MX_DATA : IOS_BB_MX_FCS;
            break;
        case IOS_BB_MX_DATA:
            if (m->cnt < sizeof(m->buf)) {
                m->buf[m->cnt++] = c;
            }
            if (m->cnt >= m->len) {
                if ((m->ctrl & ~MX_PF) != MX_UIH) {
                    m->fcs = mx_fcs(m->fcs, m->buf, m->cnt);
                }
                m->state = IOS_BB_MX_FCS;
            }
            break;
        case IOS_BB_MX_FCS:
            if (mx_fcs_table[m->fcs ^ c] == MX_GOOD_FCS) {
                mx_rx_frame(bb, m->addr, m->ctrl, m->buf, m->cnt);
            } else {
                TRACE("mux bad FCS on a %u-byte frame (addr %02x ctrl %02x len %u fcs %02x): "
                      "%02x %02x %02x %02x %02x %02x\n", m->cnt, m->addr, m->ctrl, m->len, c,
                      m->buf[0], m->buf[1], m->buf[2], m->buf[3], m->buf[4], m->buf[5]);
            }
            m->state = IOS_BB_MX_SEARCH;
            m->flags_run = 0;
            break;
        default:
            m->state = IOS_BB_MX_SEARCH;
            break;
        }
        if (bb->mux_leave) {
            bb->mux_leave = false;
            bb->mux = false;
            memset(&bb->muxrx, 0, sizeof(bb->muxrx));
            TRACE("mux closed (CLD), back to AT\n");
        }
    }
}

/* ------------------------------------------------------------- channel byte streams */

/* Raw or H5-wrapped output of the AT stream, pre-mux. */
static void h5_stream(IosBbCore *bb, const uint8_t *data, unsigned len)
{
    if (!bb->h5) {
        bb->out(bb->opaque, data, len);
        return;
    }
    if (!bb->h5_active) {
        /* The kernel drops data before the link is Active; queue it. */
        h5_queue(bb, data, len);
        return;
    }
    h5_queue(bb, data, len);
    h5_pump(bb);
}

/* Channel write: muxed UIH once the multiplexer is up, otherwise channel 0's stream. */
static void chan_write(IosBbCore *bb, int ch, const char *s, unsigned len)
{
    if (getenv(IOS_BB_TRACE_ENV) && atoi(getenv(IOS_BB_TRACE_ENV)) >= 2) {
        fprintf(stderr, "%" PRId64 " ios-bb: ch%d < ", bb->now_ms, ch);
        for (unsigned i = 0; i < len && i < 120; i++) {
            fputc(s[i] == '\r' || s[i] == '\n' ? ' ' : s[i], stderr);
        }
        fputc('\n', stderr);
    }
    if (bb->mux && ch > 0) {
        mx_send(bb, ch, false, MX_UIH, (const uint8_t *)s, len);
    } else {
        h5_stream(bb, (const uint8_t *)s, len);
    }
}

static void chan_printf(IosBbCore *bb, int ch, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void chan_printf(IosBbCore *bb, int ch, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    chan_write(bb, ch, buf, n);
}

/* ------------------------------------------------------------------------ GSM 03.38 */

/*
 * Default alphabet, septet order, as UTF-8 (the Greek block needs two bytes).
 * Digits and letters land on their ASCII values; position 96 is the reserved
 * non-breaking space filler of TS 23.038 Table 6.1.
 */
static const char *const gsm7_main[128] = {
    /* 0..15 */
    "@", "\xc2\xa3", "$", "\xc2\xa5", "\xc3\xa8", "\xc3\xa9", "\xc3\xb9",
    "\xc3\xac", "\xc3\xb2", "\xc3\x87", "\n", "\xc3\x98", "\xc3\xb8", "\r",
    "\xc3\x85", "\xc3\xa5",
    /* 16..31: the Greek block */
    "\xce\x94", "_", "\xce\xa6", "\xce\x93", "\xce\x9b", "\xce\xa9",
    "\xce\xa0", "\xce\xa8", "\xce\xa3", "\xce\x98", "\xce\x9e", "\xc3\x86",
    "\xc3\xa6", "\xc3\x9f", "\xc3\x89", " ",
    /* 32..47 */
    " ", "!", "\"", "#", "\xc2\xa4", "%", "&", "'", "(", ")", "*", "+", ",",
    "-", ".", "/",
    /* 48..63 */
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=",
    ">", "?",
    /* 64..95 */
    "\xc2\xa1",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
    "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "\xc3\x84", "\xc3\x96", "\xc3\x91", "\xc2\xa7", "\xc2\xbf",
    /* 96..127 */
    "\xc2\xa0",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n",
    "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "\xc3\xa4", "\xc3\xb6", "\xc3\xbc", "\xc3\xb1", "\xc3\xa0",
};

/* 0x1B escape sequences (23.038 Table 6b). */
static const char *const gsm7_ext[][2] = {
    { "^", "\x14" }, { "{", "\x28" }, { "}", "\x29" }, { "\\", "\x2f" },
    { "[", "\x3c" }, { "]", "\x3d" }, { "|", "\x40" },
    { "\xe2\x82\xac", "\x65" },             /* the euro sign */
    { NULL, NULL }
};

/* Try to consume one table entry at the head of s; return its septet or -1.
 * Position 31 is the alphabet's duplicate space; pack text as position 32 so
 * the bytes match what a standard packer (and CommCenter) produces, while a
 * received 31 still unpacks as a space. */
static int gsm7_lookup(const char *s, unsigned *len)
{
    for (int i = 0; i < 128; i++) {
        unsigned n = strlen(gsm7_main[i]);

        if (i == 31) {
            continue;                         /* prefer the space at 32 */
        }
        if (strncmp(s, gsm7_main[i], n) == 0) {
            *len = n;
            return i;
        }
    }
    for (int i = 0; gsm7_ext[i][0]; i++) {
        unsigned n = strlen(gsm7_ext[i][0]);

        if (strncmp(s, gsm7_ext[i][0], n) == 0) {
            *len = n;
            return 0x100 | gsm7_ext[i][1][0];   /* 0x100 marks an escape */
        }
    }
    return -1;
}

/* Is every char of a UTF-8 string representable in the default alphabet? */
static bool gsm7_encodable(const char *s)
{
    while (*s) {
        unsigned n;
        int v = gsm7_lookup(s, &n);

        if (v < 0) {
            return false;
        }
        s += n;
    }
    return true;
}

/* Pack a UTF-8 string into 7-bit septets (gsm7_encodable() said yes).
 * Septets form a bit stream, least-significant bit first, into bytes the same
 * way: "Hi" -> C8 34, "Hello" -> C8 32 9B FD 06 (verified against
 * CommCenter's own packer, see the calls-sms doc). */
static unsigned gsm7_pack(const char *s, uint8_t *out, unsigned max)
{
    uint16_t septets[512];
    unsigned ns = 0, bitpos = 0, no = 0;

    while (*s && ns + 2 < 512) {
        unsigned n;
        int v = gsm7_lookup(s, &n);

        if (v < 0) {
            break;
        }
        if (v & 0x100) {
            septets[ns++] = 0x1b;
            septets[ns++] = v & 0xff;
        } else {
            septets[ns++] = v;
        }
        s += n;
    }
    memset(out, 0, max);
    for (unsigned i = 0; i < ns; i++) {
        for (unsigned b = 0; b < 7; b++) {
            if (septets[i] & (1u << b)) {
                out[bitpos >> 3] |= 1u << (bitpos & 7);
            }
            bitpos++;
            if ((bitpos >> 3) + 1 > max) {
                return max;
            }
        }
        no = (bitpos + 7) >> 3;
    }
    return no;
}

/* Unpack packed 7-bit septets into UTF-8 (escape table applied). */
static unsigned gsm7_unpack(const uint8_t *in, unsigned nseptets,
                            char *out, unsigned max)
{
    unsigned no = 0, esc = 0;

    for (unsigned i = 0; i < nseptets && no + 4 < max; i++) {
        unsigned bitpos = i * 7;
        uint8_t v = 0;

        for (unsigned b = 0; b < 7; b++) {
            v |= ((in[(bitpos + b) >> 3] >> ((bitpos + b) & 7)) & 1) << b;
        }
        if (esc) {
            esc = 0;
            for (int j = 0; gsm7_ext[j][0]; j++) {
                if ((uint8_t)gsm7_ext[j][1][0] == v) {
                    unsigned n = strlen(gsm7_ext[j][0]);

                    memcpy(out + no, gsm7_ext[j][0], n);
                    no += n;
                    break;
                }
            }
            continue;
        }
        if (v == 0x1b) {
            esc = 1;
            continue;
        }
        unsigned n = strlen(gsm7_main[v]);

        memcpy(out + no, gsm7_main[v], n);
        no += n;
    }
    out[no] = 0;
    return no;
}

/* UTF-8 -> UTF-16BE ("UCS2"). Returns octets written. */
static unsigned utf8_to_ucs2be(const char *s, uint8_t *out, unsigned max)
{
    unsigned no = 0;

    while (*s && no + 2 <= max) {
        unsigned c = (unsigned char)*s++;
        if (c < 0x80) {
            /* plain */
        } else if ((c & 0xe0) == 0xc0) {
            c = ((c & 0x1f) << 6) | (*s++ & 0x3f);
        } else if ((c & 0xf0) == 0xe0) {
            c = ((c & 0x0f) << 12) | (((unsigned char)*s++ & 0x3f) << 6);
            c |= (unsigned char)*s++ & 0x3f;
        } else {
            c = '?';
        }
        if (c >= 0x10000) {
            c = '?';
        }
        out[no++] = c >> 8;
        out[no++] = c & 0xff;
    }
    return no;
}

/* UTF-16BE -> UTF-8. */
static unsigned ucs2be_to_utf8(const uint8_t *in, unsigned n, char *out, unsigned max)
{
    unsigned no = 0;

    for (unsigned i = 0; i + 1 < n && no + 3 < max; i += 2) {
        unsigned c = (in[i] << 8) | in[i + 1];

        if (c < 0x80) {
            out[no++] = c;
        } else if (c < 0x800) {
            out[no++] = 0xc0 | (c >> 6);
            out[no++] = 0x80 | (c & 0x3f);
        } else {
            out[no++] = 0xe0 | (c >> 12);
            out[no++] = 0x80 | ((c >> 6) & 0x3f);
            out[no++] = 0x80 | (c & 0x3f);
        }
    }
    out[no] = 0;
    return no;
}

static const char hexdig[17] = "0123456789ABCDEF";

static void bytes_to_hex(const uint8_t *in, unsigned n, char *out)
{
    for (unsigned i = 0; i < n; i++) {
        out[2 * i] = hexdig[in[i] >> 4];
        out[2 * i + 1] = hexdig[in[i] & 0xf];
    }
    out[2 * n] = 0;
}

static unsigned hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 0xffff;
}

static unsigned hex_nibbles(const char *s, uint8_t *out, unsigned max)
{
    unsigned n = 0;

    while (*s && n < max) {
        unsigned hi = hex_digit(*s++);

        if (hi == 0xffff) {
            continue;                            /* skip noise (spaces, CR) */
        }
        unsigned lo = *s ? hex_digit(*s++) : 0xf;
        if (lo == 0xffff) {
            lo = 0xf;
        }
        out[n++] = (hi << 4) | lo;
    }
    return n;
}

/* Digits -> swapped semi-octets ("14155550100" -> 41 51 55 05 01 F0). */
static unsigned digits_to_semi(const char *digits, uint8_t *out, unsigned max)
{
    unsigned n = 0;

    while (*digits && n < max) {
        unsigned hi = *digits++ - '0';
        unsigned lo = (*digits && *digits >= '0' && *digits <= '9') ? *digits++ - '0' : 0xf;

        out[n++] = (lo << 4) | hi;
    }
    return n;
}

/* Swapped semi-octets -> digits ("41 51 55 05 01 F0" -> "14155550100":
 * the first digit rides the low nibble). */
static unsigned semi_to_digits(const uint8_t *in, unsigned n, char *out, unsigned max)
{
    unsigned nd = 0;

    for (unsigned i = 0; i < n && nd + 2 < max; i++) {
        unsigned hi = in[i] >> 4, lo = in[i] & 0xf;

        if (lo < 10) {
            out[nd++] = '0' + lo;
        }
        if (hi < 10) {
            out[nd++] = '0' + hi;
        }
    }
    out[nd] = 0;
    return nd;
}

/* 7 octets of SCTS from now_ms, swapped nibbles, time zone 0 (GMT). */
static void scts_from_ms(int64_t now_ms, uint8_t out[7])
{
    int64_t secs = now_ms / 1000;
    struct tm tm;

    gmtime_r((const time_t *)&secs, &tm);

    unsigned v[7] = { (tm.tm_year + 1900) % 100, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec, 0 };
    for (int i = 0; i < 7; i++) {
        out[i] = (v[i] % 10) << 4 | (v[i] / 10);
    }
}

/* --------------------------------------------------------------- SMS PDU assembly */

/* Build a full SMS-DELIVER hex PDU (SCA included). Returns octet count, 0 on error. */
static unsigned sms_build_deliver(IosBbCore *bb, const char *number, const char *text,
                                  char *pdu, unsigned pmax, unsigned *tpdu_octets)
{
    uint8_t oct[2 + 1 + 1 + 1 + 1 + 14 + 1 + 175];
    unsigned n = 0;
    unsigned sca_oct = 0;
    bool ucs2 = !gsm7_encodable(text);

    if (strlen(number) > 20) {
        return 0;
    }

    /* SCA: length (counting what follows), TOA 0x91, digits, or empty. */
    if (bb->sca[0]) {
        uint8_t semi[10];

        sca_oct = digits_to_semi(bb->sca, semi, sizeof(semi));
        oct[n++] = 1 + sca_oct;
        oct[n++] = 0x91;
        memcpy(oct + n, semi, sca_oct);
        n += sca_oct;
    } else {
        oct[n++] = 0;
        sca_oct = 0;
    }

    /* SMS-DELIVER, MMS clear (matches what 1.0 parses happily, see the calls doc). */
    oct[n++] = 0x04;
    oct[n++] = strlen(number);                   /* originating address length */
    oct[n++] = 0x91;                             /* international */
    n += digits_to_semi(number, oct + n, sizeof(oct) - n);
    oct[n++] = 0x00;                             /* PID */
    oct[n++] = ucs2 ? 0x08 : 0x00;               /* DCS */
    scts_from_ms(bb->now_ms + bb->wall_offset_ms, oct + n);
    n += 7;

    if (ucs2) {
        uint8_t ud[2 * 160];

        unsigned un = utf8_to_ucs2be(text, ud, sizeof(ud));
        if (un > 140) {
            return 0;
        }
        oct[n++] = un;
        memcpy(oct + n, ud, un);
        n += un;
    } else {
        uint8_t ud[180];

        unsigned un = gsm7_pack(text, ud, sizeof(ud));
        if (strlen(text) > 160) {
            return 0;
        }
        oct[n++] = (uint8_t)strlen(text);        /* septets */
        memcpy(oct + n, ud, un);
        n += un;
    }
    if (2 * n + 1 > pmax) {
        return 0;
    }
    bytes_to_hex(oct, n, pdu);
    *tpdu_octets = n - (bb->sca[0] ? 1 + 1 + sca_oct : 1);
    return n;
}

/* Decode the SMS-SUBMIT the guest sends to +CMGS; records it in last_mo_*. */
static void sms_parse_submit(IosBbCore *bb, const uint8_t *p, unsigned n)
{
    unsigned pos = 0, dlen, vp = 0, da_semi, dcs, udl, udoct;

    if (n < 5) {
        return;
    }
    pos += 1 + p[0];                             /* SCA */
    if (pos >= n) {
        return;
    }
    uint8_t fo = p[pos++];
    if (((fo >> 3) & 3) == 2) {
        vp = 1;                                  /* relative validity period */
    }
    if (pos >= n) {
        return;
    }
    pos++;                                       /* MR */
    if (pos >= n) {
        return;
    }
    dlen = p[pos++];                             /* DA length in digits */
    if (pos >= n) {
        return;
    }
    pos++;                                       /* DA TOA */
    da_semi = pos;
    pos += (dlen + 1) / 2;
    if (pos + 2 + vp >= n) {
        return;
    }
    semi_to_digits(p + da_semi, (dlen + 1) / 2, bb->last_mo_num,
                   sizeof(bb->last_mo_num));
    pos++;                                       /* PID */
    dcs = p[pos++];
    if (vp) {
        pos++;
    }
    if (pos >= n) {
        return;
    }
    udl = p[pos++];
    udoct = n - pos;

    switch ((dcs >> 2) & 0x3) {
    case 0:                                      /* GSM 7-bit */
        gsm7_unpack(p + pos, udl, bb->last_mo_text, sizeof(bb->last_mo_text));
        break;
    case 1:                                      /* 8-bit */
        udoct = udl < udoct ? udl : udoct;
        udoct = udoct < sizeof(bb->last_mo_text) - 1 ? udoct : sizeof(bb->last_mo_text) - 1;
        memcpy(bb->last_mo_text, p + pos, udoct);
        bb->last_mo_text[udoct] = 0;
        break;
    default:                                     /* UCS2 */
        ucs2be_to_utf8(p + pos, udl < udoct ? udl : udoct, bb->last_mo_text,
                       sizeof(bb->last_mo_text));
        break;
    }
}

/* ------------------------------------------------------------------- the AT engine */

/* Line helpers; every line is "\r\n<line>\r\n" on the wire. */
static void at_ok(IosBbCore *bb, int ch);
static void at_error(IosBbCore *bb, int ch);
static const char *arg_after(const char *cmd, const char *pat, const void *unused);

static void at_ok(IosBbCore *bb, int ch)
{
    chan_printf(bb, ch, "\r\nOK\r\n");
}

static void at_error(IosBbCore *bb, int ch)
{
    chan_printf(bb, ch, "\r\nERROR\r\n");
}

/* hex-ASCII of a string (what getStr enc 3 reads under +CSCS="HEX"). */
static void str_to_hexstr(const char *s, char *out, unsigned max)
{
    unsigned n = strlen(s);

    if (n > (max - 1) / 2) {
        n = (max - 1) / 2;
    }
    bytes_to_hex((const uint8_t *)s, n, out);
}

static int at_type_of(const char *number)
{
    return number[0] == '+' ? 145 : 129;
}

/* ------------------------------------------------------------------ call handling */

static IosBbCall *call_new(IosBbCore *bb, bool mt, const char *number, int stat)
{
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        IosBbCall *c = &bb->calls[i];

        if (!c->used) {
            memset(c, 0, sizeof(*c));
            c->used = true;
            c->mt = mt;
            c->id = bb->next_call_id++;
            c->stat = stat;
            c->next_stat = -1;
            c->due_ms = 0;
            snprintf(c->number, sizeof(c->number), "%s", number);
            return c;
        }
    }
    return NULL;
}

static IosBbCall *call_by_id(IosBbCore *bb, int id)
{
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        IosBbCall *c = &bb->calls[i];

        if (c->used && c->id == id) {
            return c;
        }
    }
    return NULL;
}

static void emit_xcallstat(IosBbCore *bb, int id, int stat)
{
    if (!bb->ch[bb->call_ch].open) {
        return;
    }
    chan_printf(bb, bb->call_ch, "\r\n+XCALLSTAT: %d,%d\r\n", id, stat);
}

static void call_release(IosBbCore *bb, IosBbCall *c)
{
    c->stat = IOS_BB_CALL_RELEASED;
    c->next_stat = -1;
    c->due_ms = 0;
    emit_xcallstat(bb, c->id, IOS_BB_CALL_RELEASED);
    c->used = false;
}

static bool radio_ok(const IosBbCore *bb)
{
    return bb->cfun == 1 && bb->registered;
}

/* Emit the URCs that create a ringing call on 1.0 (DLCI 1). */
static void call_ring_urcs(IosBbCore *bb, IosBbCall *c)
{
    bool active = false;

    emit_xcallstat(bb, c->id, IOS_BB_CALL_INCOMING);
    chan_printf(bb, bb->call_ch, "\r\n+CLIP: \"%s\",%d,,,\"\",0\r\n",
                c->number, at_type_of(c->number));
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        IosBbCall *o = &bb->calls[i];

        if (o->used && o != c && (o->stat == IOS_BB_CALL_ACTIVE ||
                                   o->stat == IOS_BB_CALL_DIALING ||
                                   o->stat == IOS_BB_CALL_ALERTING)) {
            active = true;
        }
    }
    if (active) {
        /* Waiting call: +CCWA carries the same number/type, class 1 (voice). */
        chan_printf(bb, bb->call_ch, "\r\n+CCWA: \"%s\",%d,1\r\n",
                    c->number, at_type_of(c->number));
    }
    chan_printf(bb, bb->call_ch, "\r\nRING\r\n");
    c->rings = 1;
    c->next_stat = -2;                           /* -2 = ring repeat */
    c->due_ms = bb->now_ms + 3000;
}

bool ios_bb_incoming_call(IosBbCore *bb, const char *number)
{
    IosBbCall *c;

    if (!bb->ch[bb->call_ch].open || !radio_ok(bb) || !number[0]) {
        return false;
    }
    c = call_new(bb, true, number, IOS_BB_CALL_INCOMING);
    if (!c) {
        return false;
    }
    call_ring_urcs(bb, c);
    return true;
}

static void call_progress(IosBbCore *bb, IosBbCall *c, int stat)
{
    c->stat = stat;
    c->next_stat = -1;
    c->due_ms = 0;
    emit_xcallstat(bb, c->id, stat);
}

void ios_bb_remote_answer(IosBbCore *bb)
{
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        IosBbCall *c = &bb->calls[i];

        if (c->used && (c->stat == IOS_BB_CALL_DIALING ||
                        c->stat == IOS_BB_CALL_ALERTING)) {
            call_progress(bb, c, IOS_BB_CALL_ACTIVE);
            return;
        }
    }
}

void ios_bb_remote_hangup(IosBbCore *bb)
{
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        IosBbCall *c = &bb->calls[i];

        if (c->used && c->stat != IOS_BB_CALL_RELEASED) {
            bb->ceer_cause = 16;                 /* normal call clearing */
            call_release(bb, c);
            return;
        }
    }
}

const char *ios_bb_call_state(const IosBbCore *bb)
{
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        const IosBbCall *c = &bb->calls[i];

        if (c->used) {
            switch (c->stat) {
            case IOS_BB_CALL_ACTIVE:
                return "active";
            case IOS_BB_CALL_HELD:
                return "held";
            case IOS_BB_CALL_DIALING:
                return "dialing";
            case IOS_BB_CALL_ALERTING:
                return "alerting";
            case IOS_BB_CALL_INCOMING:
            case IOS_BB_CALL_WAITING:
                return "incoming";
            default:
                break;
            }
        }
    }
    return "idle";
}

/* ------------------------------------------------------------- registration / SIM */

int ios_bb_rssi(const IosBbCore *bb)
{
    int n = (bb->signal_dbm + 113 + 1) / 2;      /* 2n-113 dBm */

    if (bb->signal_dbm <= -113) {
        return 0;
    }
    if (n > 31) {
        return 31;
    }
    return n < 0 ? 0 : n;
}

static void emit_xciev(IosBbCore *bb)
{
    int rssi = ios_bb_rssi(bb);
    int batt = bb->battery < 1 ? 1 : bb->battery > 100 ? 100 : bb->battery;

    if (!bb->ch[bb->xciev_ch].open) {
        return;
    }
    bb->last_rssi = rssi;
    bb->last_batt = batt;
    chan_printf(bb, bb->xciev_ch, "\r\n+XCIEV: %d,%d\r\n", rssi, batt);
}

/* Schedule the searching -> registered URC burst on DLCI 2. */
static void reg_schedule(IosBbCore *bb)
{
    if (bb->cfun != 1) {
        return;
    }
    bb->reg_step = 1;
    bb->reg_due_ms = bb->now_ms + 300;
    TRACE("registration burst scheduled\n");
}

/* The current +CREG stat the model would report. */
static int reg_stat(const IosBbCore *bb)
{
    if (bb->cfun != 1) {
        return 0;                                /* no RF: not registered, not searching */
    }
    if (bb->cops_detached) {
        return 0;
    }
    return bb->registered ? 1 : 2;
}

/*
 * +XREG value for the data bearer (4.x getDataRAT: values up to 2 count as none).
 * ponytail: one fixed bearer; make it a property if the Mac app wants E/3G toggles.
 */
#define IOS_BB_XREG_BEARER 4

static int xreg_value(const IosBbCore *bb)
{
    return reg_stat(bb) == 1 ? IOS_BB_XREG_BEARER : 0;
}

static void reg_tick(IosBbCore *bb)
{
    int ch = bb->creg_ch;
    int stat = reg_stat(bb);

    if (!bb->ch[ch].open) {
        bb->reg_step = 0;
        return;
    }
    if (bb->reg_step == 1) {
        if (stat == 1) {
            /* +CREG URC form (n=2: stat,lac,ci; CommCenter reads stat base 10,
             * lac/ci base 16). */
            chan_printf(bb, ch, "\r\n+CREG: 2\r\n");
            bb->last_creg = 2;
            if (bb->cgreg_n > 0) {
                chan_printf(bb, ch, "\r\n+CGREG: 2\r\n");
            }
            bb->reg_step = 2;
            bb->reg_due_ms = bb->now_ms + 400;
        } else {
            /* Searching (or off); report once and stop. */
            chan_printf(bb, ch, "\r\n+CREG: %d\r\n", stat);
            bb->last_creg = stat;
            if (bb->cgreg_n > 0) {
                chan_printf(bb, ch, "\r\n+CGREG: %d\r\n", stat);
            }
            bb->reg_step = 0;
        }
        return;
    }
    if (bb->reg_step == 2) {
        chan_printf(bb, ch, "\r\n+CREG: 1,%X,%X\r\n", bb->lac, bb->ci);
        bb->last_creg = 1;
        if (bb->cgreg_n > 0) {
            chan_printf(bb, ch, "\r\n+CGREG: 1\r\n");
        }
        if (bb->xreg_n > 0) {
            chan_printf(bb, ch, "\r\n+XREG: %d\r\n", xreg_value(bb));
        }
        emit_xciev(bb);
        bb->reg_step = 0;
    }
}

void ios_bb_operator_changed(IosBbCore *bb)
{
    /* CommCenter only re-reads +COPS/+XCOPS on a registration change: re-register. */
    if (reg_stat(bb) == 1 && bb->ch[bb->creg_ch].open) {
        reg_schedule(bb);
    }
}

void ios_bb_changed(IosBbCore *bb)
{
    int rssi = ios_bb_rssi(bb);
    int batt = bb->battery < 1 ? 1 : bb->battery > 100 ? 100 : bb->battery;

    if (bb->ch[bb->xciev_ch].open && (rssi != bb->last_rssi || batt != bb->last_batt)) {
        emit_xciev(bb);
    }
    if (bb->cfun == 1 && bb->ch[bb->creg_ch].open && reg_stat(bb) != bb->last_creg) {
        reg_schedule(bb);
    }
    if (bb->sim_present != bb->sim_last && bb->ch[bb->xsim_ch].open) {
        /* SIM inserted/removed under us: the SIM model reacts to +XSIM: n. */
        bb->xsim_due_ms = bb->now_ms + 100;
    }
    bb->sim_last = bb->sim_present;
}

/* ----------------------------------------------------------- +CHLD and dial */

static void chld_command(IosBbCore *bb, int ch, const char *arg)
{
    char what[8] = { 0 };

    if (!arg || !*arg) {
        at_ok(bb, ch);
        return;
    }
    snprintf(what, sizeof(what), "%s", arg);

    /* The reply comes first, then the +XCALLSTAT URCs (the 1.0 call sequences
     * in commcenter-1.0-calls-sms.md all read OK before any URC). */
    at_ok(bb, ch);

    switch (what[0]) {
    case '0':                                  /* release held (+ ringing/waiting) */
        for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
            IosBbCall *c = &bb->calls[i];

            if (c->used && (c->stat == IOS_BB_CALL_HELD ||
                            c->stat == IOS_BB_CALL_INCOMING ||
                            c->stat == IOS_BB_CALL_WAITING)) {
                call_release(bb, c);
            }
        }
        break;
    case '1':                                  /* release active/dialing, or 1<id> */
        if (what[1]) {
            int id = atoi(what + 1);
            IosBbCall *c = call_by_id(bb, id);

            if (c) {
                call_release(bb, c);
            }
            /* else: an in-call keypad code (GSM 02.30 "1x"), not a call id. */
        } else {
            for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
                IosBbCall *c = &bb->calls[i];

                if (c->used && (c->stat == IOS_BB_CALL_ACTIVE ||
                                c->stat == IOS_BB_CALL_DIALING ||
                                c->stat == IOS_BB_CALL_ALERTING)) {
                    call_release(bb, c);
                }
            }
        }
        break;
    case '2':                                  /* answer and/or swap */
        for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
            IosBbCall *c = &bb->calls[i];

            if (c->used && (c->stat == IOS_BB_CALL_INCOMING ||
                            c->stat == IOS_BB_CALL_WAITING)) {
                /* Held everything else, answered this one. */
                for (int j = 0; j < IOS_BB_MAX_CALLS; j++) {
                    IosBbCall *o = &bb->calls[j];

                    if (o->used && o != c && o->stat == IOS_BB_CALL_ACTIVE) {
                        call_progress(bb, o, IOS_BB_CALL_HELD);
                    }
                }
                call_progress(bb, c, IOS_BB_CALL_ACTIVE);
                return;
            }
        }
        /* No incoming: swap active <-> held. */
        for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
            IosBbCall *c = &bb->calls[i];

            if (c->used && c->stat == IOS_BB_CALL_ACTIVE) {
                call_progress(bb, c, IOS_BB_CALL_HELD);
            } else if (c->used && c->stat == IOS_BB_CALL_HELD) {
                call_progress(bb, c, IOS_BB_CALL_ACTIVE);
            }
        }
        break;
    case '3':                                  /* conference: merge held into active */
        for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
            IosBbCall *c = &bb->calls[i];

            if (c->used && c->stat == IOS_BB_CALL_HELD) {
                call_progress(bb, c, IOS_BB_CALL_ACTIVE);
            }
        }
        break;
    case '4':                                  /* call transfer; no model state */
        break;
    default:
        break;
    }
}

/* atd<number>[;] — voice call; the model always accepts a healthy dial. */
static void dial_command(IosBbCore *bb, int ch, const char *num)
{
    IosBbCall *c;
    char nbuf[32];
    unsigned n = 0;
    bool semi = false;

    for (const char *p = num; *p && n < sizeof(nbuf) - 1; p++) {
        if (*p == ';') {
            semi = true;
            break;
        }
        if (*p != ' ') {
            nbuf[n++] = *p;
        }
    }
    nbuf[n] = 0;
    if (!n || !bb->ch[bb->call_ch].open || !radio_ok(bb)) {
        at_error(bb, ch);                        /* final code 1: call failed */
        return;
    }
    c = call_new(bb, false, nbuf, IOS_BB_CALL_DIALING);
    if (!c) {
        at_error(bb, ch);
        return;
    }
    snprintf(bb->last_dialed, sizeof(bb->last_dialed), "%s", nbuf);
    at_ok(bb, ch);
    emit_xcallstat(bb, c->id, IOS_BB_CALL_DIALING);
    c->next_stat = IOS_BB_CALL_ALERTING;
    c->due_ms = bb->now_ms + 2000;
    (void)semi;
}

/* -------------------------------------------------------------- SMS from the guest */

static void cmgs_prompt_commit(IosBbCore *bb, int ch, bool send)
{
    IosBbAtChan *c = &bb->ch[ch];
    uint8_t pdu[220];
    unsigned n;

    c->sms_prompt = false;
    c->len = 0;
    if (!send) {
        at_error(bb, ch);                        /* ESC aborts the send */
        return;
    }
    n = hex_nibbles(c->line, pdu, sizeof(pdu));
    sms_parse_submit(bb, pdu, n);
    bb->sms_mr = (bb->sms_mr + 1) & 0xff;
    chan_printf(bb, ch, "\r\n+CMGS: %d\r\n", bb->sms_mr);
    at_ok(bb, ch);
}

bool ios_bb_incoming_sms(IosBbCore *bb, const char *number, const char *text)
{
    char pdu[400];
    unsigned n, tpdu;
    IosBbSms *slot;

    if (!bb->ch[bb->sms_ch].open || !radio_ok(bb) || !number[0] || !text[0]) {
        return false;
    }
    for (const char *p = number; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;                        /* alphanumeric senders not modelled */
        }
    }
    n = sms_build_deliver(bb, number, text, pdu, sizeof(pdu), &tpdu);
    if (!n) {
        return false;
    }
    slot = &bb->store[bb->store_next % IOS_BB_SMS_STORE];
    slot->used = true;
    snprintf(slot->num, sizeof(slot->num), "%s", number);
    snprintf(slot->pdu, sizeof(slot->pdu), "%s", pdu);
    bb->store_next++;

    chan_printf(bb, bb->sms_ch, "\r\n+CMT: ,%u\r\n\r\n%s\r\n", tpdu, pdu);
    return true;
}

const char *ios_bb_last_mo_sms_number(const IosBbCore *bb)
{
    return bb->last_mo_num;
}

const char *ios_bb_last_mo_sms_text(const IosBbCore *bb)
{
    return bb->last_mo_text;
}

/* ------------------------------------------------------------ the command table */

/*
 * The radio's nvram as iBoot-159 reads it (AT+XDRV=9,1,<block>, 512 bytes a block,
 * "+XDRV: 9,1,<status>,<block>,<hex>", until 0x600 bytes or a status other than 0):
 * entries [type BE16][length BE16, in 16-bit words with the header][data], ended by
 * a zero length. Type 1 is the Wi-Fi calibration iBoot puts into arm-io/sdio
 * tx-calibration (1024 bytes; AppleMRVL868x refuses an all 0x00 or all 0xFF one).
 * Synthetic: a generated table, not a unit's.
 */
#define IOS_BB_NVRAM_SIZE  0x600
#define IOS_BB_NVRAM_BLOCK 512
#define IOS_BB_NVRAM_WIFI_CAL 1

void ios_bb_radio_nvram(uint8_t *nv)
{
    unsigned cal = 1024, words = (4 + cal) / 2;

    memset(nv, 0xff, IOS_BB_NVRAM_SIZE);
    nv[0] = 0;
    nv[1] = IOS_BB_NVRAM_WIFI_CAL;
    nv[2] = words >> 8;
    nv[3] = words;
    for (unsigned i = 0; i < cal; i++) {
        nv[4 + i] = (uint8_t)((i * 37 + 0x5a) ^ (i >> 3));
    }
    memset(nv + 4 + cal, 0, 4);                  /* end: a zero-length entry */
}

static void radio_nvram_block(IosBbCore *bb, int ch, int block)
{
    uint8_t nv[IOS_BB_NVRAM_SIZE];
    char hex[2 * IOS_BB_NVRAM_BLOCK + 1], line[sizeof(hex) + 48];
    int n;

    if (block < 0 || (block + 1) * IOS_BB_NVRAM_BLOCK > IOS_BB_NVRAM_SIZE) {
        chan_printf(bb, ch, "\r\n+XDRV: 9,1,1,%d,\r\n\r\nOK\r\n", block);
        return;
    }
    ios_bb_radio_nvram(nv);
    for (int i = 0; i < IOS_BB_NVRAM_BLOCK; i++) {
        snprintf(hex + 2 * i, 3, "%02X", nv[block * IOS_BB_NVRAM_BLOCK + i]);
    }
    n = snprintf(line, sizeof(line), "\r\n+XDRV: 9,1,0,%d,%s\r\n\r\nOK\r\n", block, hex);
    chan_write(bb, ch, line, n);              /* longer than chan_printf's buffer */
}

/* Parse one complete "at<cmd>" line (already lower-cased). */
static void at_command(IosBbCore *bb, int ch, const char *line)
{
    const char *arg;
    char buf[600];
    const char *cmd = buf;
    size_t n = strlen(line);

    if (getenv(IOS_BB_TRACE_ENV) && atoi(getenv(IOS_BB_TRACE_ENV)) >= 2) {
        fprintf(stderr, "%" PRId64 " ios-bb: ch%d > at%s\n", bb->now_ms, ch, line);
    }
    /* iBoot ends its commands with ';' ("at+cgsn;"); only a dial string needs one. */
    if (n && line[n - 1] == ';' && line[0] != 'd') {
        n--;
    }
    n = MIN(n, sizeof(buf) - 1);
    memcpy(buf, line, n);
    buf[n] = 0;
    if (strncmp(cmd, "+xdrv=9,1,", 10) == 0) {
        radio_nvram_block(bb, ch, atoi(cmd + 10));
        return;
    }

    if (!*cmd) {
        at_ok(bb, ch);                           /* bare "at" ping */
        return;
    }
    if (cmd[0] == 'e' && (cmd[1] == '0' || cmd[1] == '1') && !cmd[2]) {
        at_ok(bb, ch);                           /* echo stays off either way */
        return;
    }
    if (cmd[0] == 'd') {
        dial_command(bb, ch, cmd + 1);
        return;
    }
    if (strcmp(cmd, "a") == 0) {
        /* 4.x answers with ATA (1.0 used +chld=2): OK, then the call goes active. */
        for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
            IosBbCall *c = &bb->calls[i];

            if (c->used && (c->stat == IOS_BB_CALL_INCOMING ||
                            c->stat == IOS_BB_CALL_WAITING)) {
                at_ok(bb, ch);
                call_progress(bb, c, IOS_BB_CALL_ACTIVE);
                return;
            }
        }
        chan_printf(bb, ch, "\r\nNO CARRIER\r\n");
        return;
    }
    if (strcmp(cmd, "h") == 0 || strcmp(cmd, "h0") == 0) {
        at_ok(bb, ch);
        for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
            if (bb->calls[i].used) {
                call_release(bb, &bb->calls[i]);
            }
        }
        return;
    }
    if (strncmp(cmd, "s0=", 3) == 0) {
        bb->s0 = atoi(cmd + 3);
        at_ok(bb, ch);
        return;
    }
    if (cmd[0] != '+') {
        TRACE("unknown AT command \"at%s\"\n", cmd);
        at_ok(bb, ch);
        return;
    }

    cmd++;                                       /* skip '+' */

    /*
     * URCs go back on the DLCI that enabled them. 1.0 fixes the channel roles
     * (call 1, reg 2, sms 3); 4.x may lay its DLCIs out differently, so learn.
     */
    if (ch > 0) {
        if (strncmp(cmd, "creg=", 5) == 0) {
            bb->creg_ch = ch;
        } else if (strncmp(cmd, "xmer=", 5) == 0) {
            bb->xciev_ch = ch;
        } else if (strncmp(cmd, "xcallstat=", 10) == 0) {
            bb->call_ch = ch;
        } else if (strncmp(cmd, "cnmi=", 5) == 0) {
            bb->sms_ch = ch;
        } else if (strncmp(cmd, "xsimstate=", 10) == 0) {
            bb->xsim_ch = ch;
        }
    }

    if (strcmp(cmd, "xsio?") == 0) {
        /* 1.0 wants field 1 after its first char ("*0") to equal 0. */
        chan_printf(bb, ch, "\r\n+XSIO: 0,*0\r\n");
        at_ok(bb, ch);
        return;
    }
    if (strncmp(cmd, "xtransportmode", 14) == 0) {
        /* The kernel snoops this write and is in H5 from then on: a raw OK would be
         * line noise to it (1.0's CommCenter then times out). The OK goes as the
         * first H5 data once the link is Active. */
        if (ch == 0 && !bb->mux) {
            bb->h5 = true;
        }
        at_ok(bb, ch);
        return;
    }
    if (strncmp(cmd, "xgendata", 8) == 0) {
        /*
         * 1.0 (H5): version = first digit after the first quote to the next quote.
         * 4.x (SPI): wants a comma, then "ICE2"/"ICE3" and the digits after it, and a
         * "BOOTLOADER_VERSION:" field (CommCenter 4.2.1 0x36978), which is what About shows
         * as Modem Firmware.
         */
        if (bb->h5) {
            chan_printf(bb, ch, "\r\n+XGENDATA: \"DEV_ICE_MODEM_03.12.08_G\"\r\n");
        } else {
            chan_printf(bb, ch, "\r\n+XGENDATA: \"DEV_ICE2_MODEM_02.10.04\","
                        "\"BOOTLOADER_VERSION: 02.10.04\"\r\n");
        }
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "cgsn") == 0) {
        chan_printf(bb, ch, "\r\n%s\r\n", bb->imei);
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "cimi") == 0) {
        chan_printf(bb, ch, "\r\n%s\r\n", bb->imsi);
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "ccid") == 0) {
        chan_printf(bb, ch, "\r\n%s\r\n", bb->iccid);
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "cpin?") == 0) {
        if (!bb->sim_present) {
            chan_printf(bb, ch, "\r\n+CME ERROR: 10\r\n");
        } else {
            chan_printf(bb, ch, "\r\n+CPIN: READY\r\n");
            at_ok(bb, ch);
        }
        return;
    }
    if ((arg = arg_after(cmd, "cmux=", NULL))) {
        at_ok(bb, ch);                           /* the OK goes out pre-mux */
        bb->mux = true;
        memset(&bb->muxrx, 0, sizeof(bb->muxrx));
        bb->ch[0].open = false;                  /* the default channel retires */
        TRACE("mux started\n");
        return;
    }
    if ((arg = arg_after(cmd, "cfun=", NULL))) {
        int n = atoi(arg);

        at_ok(bb, ch);
        /* Only 0/1/4 move the radio; 4.x's +cfun=6/7 come from its SIM toolkit code. */
        if ((n == 0 || n == 1 || n == 4) && n != bb->cfun) {
            bb->cfun = n;
            if (n == 1) {
                reg_schedule(bb);
                if (bb->ch[bb->xsim_ch].open && !bb->xsim_pushed) {
                    bb->xsim_due_ms = bb->now_ms + 500;
                }
            } else {
                /* Radio off: drop every call and report "not registered". */
                for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
                    if (bb->calls[i].used) {
                        call_release(bb, &bb->calls[i]);
                    }
                }
                if (bb->ch[bb->creg_ch].open && bb->creg_n > 0) {
                    chan_printf(bb, bb->creg_ch, "\r\n+CREG: 0\r\n");
                    bb->last_creg = 0;
                }
                bb->reg_step = 0;
            }
        }
        return;
    }
    if ((arg = arg_after(cmd, "creg=", NULL))) {
        bb->creg_n = atoi(arg);
        at_ok(bb, ch);
        if (bb->creg_n > 0) {
            reg_schedule(bb);
        }
        return;
    }
    if (strcmp(cmd, "creg?") == 0) {
        chan_printf(bb, ch, "\r\n+CREG: %d,%d,%X,%X\r\n", bb->creg_n, reg_stat(bb),
                    bb->lac, bb->ci);
        at_ok(bb, ch);
        return;
    }
    if ((arg = arg_after(cmd, "cgreg=", NULL))) {
        bb->cgreg_n = atoi(arg);
        at_ok(bb, ch);
        if (bb->cgreg_n > 0) {
            reg_schedule(bb);
        }
        return;
    }
    if ((arg = arg_after(cmd, "xreg=", NULL))) {
        bb->xreg_n = atoi(arg);
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "xreg?") == 0) {
        chan_printf(bb, ch, "\r\n+XREG: %d,%d\r\n", bb->xreg_n, xreg_value(bb));
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "cgreg?") == 0) {
        chan_printf(bb, ch, "\r\n+CGREG: %d,%d\r\n", bb->cgreg_n, reg_stat(bb));
        at_ok(bb, ch);
        return;
    }
    if ((arg = arg_after(cmd, "cops=", NULL))) {
        at_ok(bb, ch);
        if (strcmp(arg, "0") == 0) {
            bb->cops_detached = false;
            reg_schedule(bb);
        } else if (strcmp(arg, "2") == 0) {
            bb->cops_detached = true;
            if (bb->ch[bb->creg_ch].open && bb->creg_n > 0) {
                chan_printf(bb, bb->creg_ch, "\r\n+CREG: 0\r\n");
                bb->last_creg = 0;
            }
        } else if (strncmp(arg, "3,", 2) == 0) {
            bb->cops_format = arg[2] - '0';
        } else if (strncmp(arg, "1,2,\"", 5) == 0) {
            /* Manual select: the PLMN arrives hex-encoded (CSCS=HEX). */
            uint8_t plmn[4];

            hex_nibbles(arg + 5, plmn, sizeof(plmn) - 1);
            plmn[3] = 0;
            if (plmn[0] && plmn[1]) {
                snprintf(bb->plmn, sizeof(bb->plmn), "%s", (const char *)plmn);
            }
            reg_schedule(bb);
        }
        return;
    }
    if (strcmp(cmd, "cops?") == 0) {
        char hex[17];

        if (reg_stat(bb) != 1) {
            chan_printf(bb, ch, "\r\n+COPS: 0\r\n");
        } else if (bb->cops_format == 2) {
            str_to_hexstr(bb->plmn, hex, sizeof(hex));
            chan_printf(bb, ch, "\r\n+COPS: 0,2,\"%s\"\r\n", hex);
        } else {
            str_to_hexstr(bb->operator_long[0] ? bb->operator_long : bb->plmn,
                          hex, sizeof(hex));
            chan_printf(bb, ch, "\r\n+COPS: 0,%d,\"%s\"\r\n", bb->cops_format, hex);
        }
        at_ok(bb, ch);
        return;
    }
    if ((arg = arg_after(cmd, "xcops=", NULL))) {
        char hex[65];
        int type = 0;

        /* The name parser reads field 1 (enc 3); the =<n> form also wants field 0
         * to echo the request, so its name is only accepted on a match. */
        if (strcmp(arg, "7") != 0) {
            type = atoi(arg);
        }
        str_to_hexstr(bb->operator_long, hex, sizeof(hex));
        chan_printf(bb, ch, "\r\n+XCOPS: %d,\"%s\"\r\n", type, hex);
        at_ok(bb, ch);
        return;
    }
    if (strncmp(cmd, "xcgedpage", 9) == 0) {
        /*
         * 4.x reads signal from this engineering page, not +XCIEV: it finds
         * RAT:"GSM" (up to the next comma) and "Rssi: <rxlev>", dBm = rxlev - 110,
         * 0..63 (commcenter-4.2.1-3gs.md). One line; the parser searches the text.
         */
        int rxlev = MAX(0, MIN(63, bb->signal_dbm + 110));

        chan_printf(bb, ch, "\r\n+XCGEDPAGE: RAT:\"GSM\",Rssi: %d\r\n",
                    reg_stat(bb) == 1 ? rxlev : 0);
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "csvm?") == 0) {
        char hex[49];

        str_to_hexstr(bb->voicemail, hex, sizeof(hex));
        chan_printf(bb, ch, "\r\n+CSVM: 0,\"%s\",129\r\n", hex);
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "csca?") == 0) {
        char hex[49];

        str_to_hexstr(bb->sca, hex, sizeof(hex));
        chan_printf(bb, ch, "\r\n+CSCA: \"%s\",129\r\n", hex);
        at_ok(bb, ch);
        return;
    }
    if (strncmp(cmd, "xpincnt", 7) == 0) {
        chan_printf(bb, ch, "\r\n+XPINCNT: 3,3,10,10\r\n");
        at_ok(bb, ch);
        return;
    }
    if ((arg = arg_after(cmd, "clck=", NULL))) {
        if (strstr(arg, ",2")) {
            chan_printf(bb, ch, "\r\n+CLCK: 0\r\n");   /* FDN off */
        }
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "ceer") == 0) {
        chan_printf(bb, ch, "\r\n+CEER: CC,%d\r\n", bb->ceer_cause);
        at_ok(bb, ch);
        return;
    }
    if ((arg = arg_after(cmd, "cmgs=", NULL))) {
        int len = atoi(arg);

        (void)len;
        bb->ch[ch].sms_prompt = true;
        bb->ch[ch].len = 0;
        chan_printf(bb, ch, "\r\n> ");          /* 1.0's multi-line machinery */
        return;
    }
    if ((arg = arg_after(cmd, "cmgr=", NULL))) {
        int idx = atoi(arg);
        IosBbSms *s = &bb->store[(idx - 1) % IOS_BB_SMS_STORE];

        if (idx >= 1 && s->used) {
            unsigned oct = strlen(s->pdu) / 2;
            unsigned sca = (bb->sca[0]) ? 1 + 1 + (strlen(bb->sca) + 1) / 2 : 1;

            chan_printf(bb, ch, "\r\n+CMGR: 1,,%u\r\n\r\n%s\r\n",
                        oct - sca, s->pdu);
            at_ok(bb, ch);
        } else {
            at_error(bb, ch);
        }
        return;
    }
    if ((arg = arg_after(cmd, "cnma", NULL))) {
        at_ok(bb, ch);                          /* PDU-mode CNMA takes no parameters */
        return;
    }
    if ((arg = arg_after(cmd, "cmms=", NULL)) ||
        (arg = arg_after(cmd, "cnmi=", NULL)) ||
        (arg = arg_after(cmd, "cscb=", NULL)) ||
        (arg = arg_after(cmd, "xtesm=", NULL)) ||
        (arg = arg_after(cmd, "xmer=", NULL)) ||
        (arg = arg_after(cmd, "ctzr=", NULL)) ||
        (arg = arg_after(cmd, "ctzu=", NULL)) ||
        (arg = arg_after(cmd, "xcallstat=", NULL)) ||
        (arg = arg_after(cmd, "clip=", NULL)) ||
        (arg = arg_after(cmd, "ccwa=", NULL)) ||
        (arg = arg_after(cmd, "cnap=", NULL)) ||
        (arg = arg_after(cmd, "cusd=", NULL)) ||
        (arg = arg_after(cmd, "cssn=", NULL)) ||
        (arg = arg_after(cmd, "xpow=", NULL)) ||
        (arg = arg_after(cmd, "xdrv=", NULL)) ||
        (arg = arg_after(cmd, "xl1set=", NULL)) ||
        (arg = arg_after(cmd, "ipr=", NULL)) ||
        (arg = arg_after(cmd, "cmee=", NULL)) ||
        (arg = arg_after(cmd, "xsimstate=", NULL)) ||
        (arg = arg_after(cmd, "cgdcont=", NULL)) ||
        (arg = arg_after(cmd, "xgauth=", NULL)) ||
        (arg = arg_after(cmd, "xdns=", NULL)) ||
        (arg = arg_after(cmd, "ctfr=", NULL)) ||
        (arg = arg_after(cmd, "vts=", NULL)) ||
        (arg = arg_after(cmd, "xvts=", NULL)) ||
        (arg = arg_after(cmd, "cmgd=", NULL)) ||
        (arg = arg_after(cmd, "csca=", NULL))) {
        at_ok(bb, ch);
        if (strncmp(cmd, "ctfr=", 5) == 0) {
            for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
                IosBbCall *c = &bb->calls[i];

                if (c->used && (c->stat == IOS_BB_CALL_INCOMING ||
                                c->stat == IOS_BB_CALL_WAITING)) {
                    call_release(bb, c);        /* transferred away */
                }
            }
        }
        return;
    }
    if ((arg = arg_after(cmd, "cscs=", NULL))) {
        bb->hex_cs = strstr(arg, "hex") != NULL;
        at_ok(bb, ch);
        return;
    }
    if (strncmp(cmd, "csms", 4) == 0) {
        chan_printf(bb, ch, "\r\n+CSMS: 1,1,1\r\n");
        at_ok(bb, ch);
        return;
    }
    if (strncmp(cmd, "chld", 4) == 0) {
        chld_command(bb, ch, cmd[4] == '=' ? cmd + 5 : NULL);
        return;
    }
    if (strcmp(cmd, "clcc") == 0) {
        at_ok(bb, ch);                          /* 1.0 sends +CLCC raw, no parser */
        return;
    }
    if (strcmp(cmd, "cgact?") == 0) {
        chan_printf(bb, ch, "\r\n+CGACT: 1,%d\r\n", bb->pdp_active);
        at_ok(bb, ch);
        return;
    }
    if ((arg = arg_after(cmd, "cgact=", NULL))) {
        int cid = 1;

        if (atoi(arg) == 0) {
            sscanf(arg, "0,%d", &cid);
            bb->pdp_active = false;
            at_ok(bb, ch);
            for (int i = 0; i < IOS_BB_MAX_CH; i++) {
                if (bb->ch[i].data_cid == cid) {
                    /* Back to command mode, and say so on that DLCI as a modem does:
                     * 4.x waits for it before it reuses the channel (no NO CARRIER:
                     * it resets the baseband a few seconds later). */
                    bb->ch[i].data_cid = 0;
                    bb->ip_rxlen = 0;
                    chan_printf(bb, i, "\r\nNO CARRIER\r\n");
                    if (!bb->h5) {
                        mx_msc(bb, i, false);
                    }
                }
            }
        } else if (bb->data_out && radio_ok(bb)) {
            bb->pdp_active = true;
            at_ok(bb, ch);
        } else {
            /* No network behind this modem (or no service): refuse activation. */
            chan_printf(bb, ch, "\r\n+CME ERROR: 100\r\n");
        }
        return;
    }
    if ((arg = arg_after(cmd, "cgpaddr=", NULL))) {
        /* 4.x reads field 1 as the address (commcenter-4.2.1-3gs.md). */
        chan_printf(bb, ch, "\r\n+CGPADDR: %d,\"%s\"\r\n", atoi(arg), IOS_BB_PDP_IP);
        at_ok(bb, ch);
        return;
    }
    if (strcmp(cmd, "xdns?") == 0) {
        chan_printf(bb, ch, "\r\n+XDNS: 1,\"%s\",\"0.0.0.0\"\r\n", IOS_BB_PDP_DNS);
        at_ok(bb, ch);
        return;
    }
    if ((arg = arg_after(cmd, "cgdata=", NULL))) {
        const char *comma = strrchr(arg, ',');

        if (!bb->pdp_active || ch == 0) {
            chan_printf(bb, ch, "\r\nNO CARRIER\r\n");
            return;
        }
        chan_printf(bb, ch, "\r\nCONNECT\r\n");
        if (!bb->h5) {
            mx_msc(bb, ch, true);                /* carrier up on the data DLCI */
        }
        bb->ch[ch].data_cid = comma ? atoi(comma + 1) : 1;
        bb->ip_rxlen = 0;
        TRACE("dlci %d is raw IP for cid %d\n", ch, bb->ch[ch].data_cid);
        return;
    }
    TRACE("unknown AT command \"at+%s\"\n", cmd);
    at_ok(bb, ch);
}

/* ---------------------------------------------------------------- AT line assembly */

/* If cmd starts with pat, return what follows; NULL otherwise. */
static const char *arg_after(const char *cmd, const char *pat, const void *unused)
{
    size_t n = strlen(pat);

    (void)unused;
    if (strncmp(cmd, pat, n) == 0) {
        return cmd + n;
    }
    return NULL;
}

/*
 * Raw IP from the guest. Frames need not line up with packets, so packets are cut
 * by the IPv4 total length; anything that is not IPv4 resynchronises by dropping.
 */
static void data_chan_input(IosBbCore *bb, const uint8_t *data, unsigned len)
{
    while (len) {
        unsigned n = MIN(len, (unsigned)sizeof(bb->ip_rx) - bb->ip_rxlen), tot;

        memcpy(bb->ip_rx + bb->ip_rxlen, data, n);
        bb->ip_rxlen += n;
        data += n;
        len -= n;
        while (bb->ip_rxlen >= 20) {
            tot = bb->ip_rx[2] << 8 | bb->ip_rx[3];
            if ((bb->ip_rx[0] >> 4) != 4 || tot < 20 || tot > sizeof(bb->ip_rx)) {
                TRACE("data: not IPv4, dropping %u bytes\n", bb->ip_rxlen);
                bb->ip_rxlen = 0;
                break;
            }
            if (bb->ip_rxlen < tot) {
                break;
            }
            if (bb->data_out) {
                bb->data_out(bb->data_opaque, bb->ip_rx, tot);
            }
            TRACE("data: %u-byte IPv4 packet to the network\n", tot);
            memmove(bb->ip_rx, bb->ip_rx + tot, bb->ip_rxlen - tot);
            bb->ip_rxlen -= tot;
        }
    }
}

bool ios_bb_data_input(IosBbCore *bb, const uint8_t *pkt, size_t len)
{
    for (int i = 1; i < IOS_BB_MAX_CH; i++) {
        if (bb->ch[i].open && bb->ch[i].data_cid) {
            /* ponytail: one UIH per <=1500-byte slice; N1 from +cmux if a guest wants smaller. */
            for (size_t off = 0; off < len; off += 1500) {
                mx_send(bb, i, false, MX_UIH, pkt + off, MIN(len - off, 1500));
            }
            return true;
        }
    }
    return false;
}

static void at_chan_input(IosBbCore *bb, int ch, const uint8_t *data, unsigned len)
{
    IosBbAtChan *c = &bb->ch[ch];

    if (c->data_cid) {
        data_chan_input(bb, data, len);
        return;
    }

    for (unsigned i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (c->sms_prompt) {
            if (b == 0x1a) {                     /* Ctrl-Z sends */
                cmgs_prompt_commit(bb, ch, true);
            } else if (b == 0x1b) {              /* ESC aborts */
                cmgs_prompt_commit(bb, ch, false);
            } else if (hex_digit(b) != 0xffff && c->len < sizeof(c->line) - 1) {
                c->line[c->len++] = b;
            }
            continue;
        }
        if (b == '\r') {
            if (c->len) {
                c->line[c->len] = 0;
                if (c->len >= 2 && c->line[0] == 'a' && c->line[1] == 't') {
                    at_command(bb, ch, c->line + 2);
                } else {
                    TRACE("ignoring non-AT line \"%s\"\n", c->line);
                }
                c->len = 0;
            }
        } else if (b == '\n') {
            continue;
        } else if (c->len < sizeof(c->line) - 1) {
            c->line[c->len++] = tolower(b);
        }
    }
}

/* H5 reliable data payload: mux bytes, or the pre-mux "default" channel. */
static void h5_rx_payload(IosBbCore *bb, const uint8_t *data, unsigned len)
{
    if (bb->mux) {
        mx_input(bb, data, len);
    } else {
        at_chan_input(bb, 0, data, len);
    }
}

/* ------------------------------------------------------------- IFX SPI framing */

#define IFX_MORE      0x10         /* header byte 1 */
#define IFX_V2_CREDIT_REQ 0x40     /* header byte 1: the sender holds no credits */
#define IFX_V1_CTS    0x40         /* header byte 3 */
#define IFX_V2_GRANT  16           /* the AP's credit level we keep it at (tx-buffer-count) */

void ios_bb_ifx_init(IosBbIfx *x, int version, unsigned max_data)
{
    memset(x, 0, sizeof(*x));
    x->version = version;
    x->max_data = MIN(max_data, 0xffeu);
}

void ios_bb_ifx_queue(void *opaque, const uint8_t *buf, size_t len)
{
    IosBbIfx *x = opaque;

    if (x->txq_len + len > sizeof(x->txq)) {
        TRACE("ifx tx queue overflow, dropping %zu bytes\n", len);
        return;
    }
    memcpy(x->txq + x->txq_len, buf, len);
    x->txq_len += len;
}

/*
 * Only data raises SRDY. v2 credits ride whatever frame comes next: the N90 kernel
 * asks for them itself (byte 1 bit 6), and an SRDY raised just to grant them, before
 * the kernel's first transfer, ends in its SRDY timeout.
 */
bool ios_bb_ifx_pending(const IosBbIfx *x)
{
    return x->txq_len;
}

/* v2: data we may put in a frame (none without a credit from the AP). */
static unsigned ifx_can_send(const IosBbIfx *x)
{
    return x->version == 2 && x->credits_in <= 0 ? 0 : x->txq_len;
}

void ios_bb_ifx_xfer(IosBbIfx *x, const uint8_t *mosi, uint8_t *miso, size_t n,
                     const uint8_t **rx, size_t *rxlen)
{
    unsigned in_len = 0, out_len, grant = 0;

    *rx = NULL;
    *rxlen = 0;
    if (mosi && n >= IOS_BB_IFX_HDR) {
        in_len = mosi[0] | (mosi[1] & 0xf) << 8;
        /* 0xfff and anything past the frame: no payload (the kext's own rule). */
        if (in_len <= x->max_data && IOS_BB_IFX_HDR + in_len <= n) {
            *rx = mosi + IOS_BB_IFX_HDR;
            *rxlen = in_len;
        } else {
            in_len = 0;
        }
        if (x->version == 2) {
            if (in_len) {
                x->credits_out--;
            }
            if (mosi[1] & IFX_V2_CREDIT_REQ) {
                x->credits_out = 0;            /* it has none: grant on the next frame */
            }
            x->credits_in += mosi[2] | (mosi[3] & 0xf) << 8;
        }
    }
    if (!miso) {
        return;
    }
    memset(miso, 0, n);
    if (n < IOS_BB_IFX_HDR) {
        return;
    }
    out_len = MIN(ifx_can_send(x), MIN(x->max_data, (unsigned)n - IOS_BB_IFX_HDR));
    miso[0] = out_len;
    miso[1] = out_len >> 8;
    if (x->txq_len > out_len) {
        miso[1] |= IFX_MORE;
    }
    if (x->version == 2) {
        if (out_len) {
            x->credits_in--;
        } else if (x->txq_len) {
            miso[1] |= IFX_V2_CREDIT_REQ;
        }
        /*
         * Top the AP back up on every frame. Our count of what it spent (one per
         * data frame) can lag its own; an AP that believes it has none and nothing
         * else to say never asks again, and both sides wait until CommCenter resets
         * the baseband (seen after ~1 min of N90 data). Granting too much is harmless.
         */
        if (x->credits_out < IFX_V2_GRANT) {
            grant = IFX_V2_GRANT - x->credits_out;
            x->credits_out += grant;
        }
        miso[2] = grant;
        miso[3] = grant >> 8;
    } else {
        miso[2] = x->max_data;                 /* next_data_size: what we can take */
        miso[3] = ((x->max_data >> 8) & 0xf) | IFX_V1_CTS;
    }
    memcpy(miso + IOS_BB_IFX_HDR, x->txq, out_len);
    memmove(x->txq, x->txq + out_len, x->txq_len - out_len);
    x->txq_len -= out_len;
}

void ios_bb_ifx_unsent(IosBbIfx *x, const uint8_t *miso)
{
    unsigned len = miso[0] | (miso[1] & 0xf) << 8;

    if (!len || len > x->max_data || x->txq_len + len > sizeof(x->txq)) {
        return;
    }
    memmove(x->txq + len, x->txq, x->txq_len);
    memcpy(x->txq, miso + IOS_BB_IFX_HDR, len);
    x->txq_len += len;
    if (x->version == 2) {
        x->credits_in++;                       /* the credit was not spent either */
    }
    TRACE("ifx: frame of %u bytes never clocked; requeued\n", len);
}

/* ------------------------------------------------------------------ public API */

void ios_bb_input(IosBbCore *bb, const uint8_t *buf, size_t len)
{
    if (!bb->h5 && bb->mux) {
        /* SPI (3GS/iPhone 4): the mux runs straight on the byte stream, no H5. */
        mx_input(bb, buf, len);
    } else if (!bb->h5) {
        /* Raw AT until the kernel's H5 kicks in (it starts with SLIP frames). */
        size_t i;

        for (i = 0; i < len; i++) {
            if (buf[i] == 0xc0) {
                break;
            }
        }
        if (i) {
            at_chan_input(bb, 0, buf, i);
        }
        if (i < len) {
            bb->h5 = true;
            h5_input(bb, buf + i, len - i);
        }
    } else {
        h5_input(bb, buf, len);
    }
    if (bb->h5_need_ack) {
        h5_send_ack(bb);
    }
}

void ios_bb_tick(IosBbCore *bb, int64_t now_ms)
{
    bb->now_ms = now_ms;

    if (bb->reg_step && now_ms >= bb->reg_due_ms) {
        reg_tick(bb);
    }
    if (bb->xsim_due_ms && now_ms >= bb->xsim_due_ms) {
        bb->xsim_due_ms = 0;
        if (bb->ch[bb->xsim_ch].open) {
            chan_printf(bb, bb->xsim_ch, "\r\n+XSIM: %d\r\n",
                        bb->sim_present ? 1 : 0);
            bb->sim_last = bb->sim_present;
            bb->xsim_pushed = true;
        }
    }
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        IosBbCall *c = &bb->calls[i];

        if (!c->used) {
            continue;
        }
        if (c->next_stat == -2 && now_ms >= c->due_ms) {
            /* MT call: repeat the ring until answered or given up. */
            c->rings++;
            chan_printf(bb, bb->call_ch, "\r\nRING\r\n");
            if (bb->s0 > 0 && c->rings >= bb->s0) {
                call_progress(bb, c, IOS_BB_CALL_ACTIVE);
            } else {
                c->due_ms = now_ms + 3000;
            }
        } else if (c->next_stat >= 0 && now_ms >= c->due_ms) {
            if (c->next_stat == IOS_BB_CALL_ALERTING) {
                c->stat = IOS_BB_CALL_ALERTING;
                emit_xcallstat(bb, c->id, IOS_BB_CALL_ALERTING);
                if (bb->answer_delay_ms >= 0) {
                    c->next_stat = IOS_BB_CALL_ACTIVE;
                    c->due_ms = now_ms + (bb->answer_delay_ms ? bb->answer_delay_ms : 100);
                } else {
                    c->next_stat = -1;
                }
            } else if (c->next_stat == IOS_BB_CALL_ACTIVE) {
                call_progress(bb, c, IOS_BB_CALL_ACTIVE);
            } else {
                c->next_stat = -1;
            }
        }
    }
    if (bb->h5_nunacked && now_ms - bb->h5_last_tx_ms >= H5_RETX_MS) {
        h5_retransmit(bb);
        bb->h5_last_tx_ms = now_ms;
    }
}

int64_t ios_bb_next_due(const IosBbCore *bb)
{
    int64_t due = 0;

    if (bb->reg_step) {
        due = bb->reg_due_ms;
    }
    if (bb->xsim_due_ms && (!due || bb->xsim_due_ms < due)) {
        due = bb->xsim_due_ms;
    }
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        const IosBbCall *c = &bb->calls[i];

        if (c->used && c->due_ms && c->next_stat != -1 &&
            (!due || c->due_ms < due)) {
            due = c->due_ms;
        }
    }
    if (bb->h5_nunacked) {
        int64_t r = bb->h5_last_tx_ms + H5_RETX_MS;

        if (!due || r < due) {
            due = r;
        }
    }
    return due;
}

/* Clear everything a baseband power cycle would clear; keep the controls. */
void ios_bb_reset(IosBbCore *bb)
{
    char operator_long[33], operator_short[17], plmn[7], sca[24], voicemail[24];
    char imei[16], imsi[16], iccid[21];
    int signal_dbm = bb->signal_dbm, battery = bb->battery, answer_delay_ms = bb->answer_delay_ms;
    unsigned lac = bb->lac, ci = bb->ci;
    bool registered = bb->registered, sim_present = bb->sim_present;
    IosBbOutFn out = bb->out, data_out = bb->data_out;
    void *opaque = bb->opaque, *data_opaque = bb->data_opaque;
    int64_t now_ms = bb->now_ms, wall_offset_ms = bb->wall_offset_ms;

    memcpy(operator_long, bb->operator_long, sizeof(operator_long));
    memcpy(operator_short, bb->operator_short, sizeof(operator_short));
    memcpy(plmn, bb->plmn, sizeof(plmn));
    memcpy(sca, bb->sca, sizeof(sca));
    memcpy(voicemail, bb->voicemail, sizeof(voicemail));
    memcpy(imei, bb->imei, sizeof(imei));
    memcpy(imsi, bb->imsi, sizeof(imsi));
    memcpy(iccid, bb->iccid, sizeof(iccid));

    memset(bb, 0, sizeof(*bb));

    memcpy(bb->operator_long, operator_long, sizeof(operator_long));
    memcpy(bb->operator_short, operator_short, sizeof(operator_short));
    memcpy(bb->plmn, plmn, sizeof(plmn));
    memcpy(bb->sca, sca, sizeof(sca));
    memcpy(bb->voicemail, voicemail, sizeof(voicemail));
    memcpy(bb->imei, imei, sizeof(imei));
    memcpy(bb->imsi, imsi, sizeof(imsi));
    memcpy(bb->iccid, iccid, sizeof(iccid));
    bb->signal_dbm = signal_dbm;
    bb->battery = battery;
    bb->answer_delay_ms = answer_delay_ms;
    bb->lac = lac;
    bb->ci = ci;
    bb->registered = registered;
    bb->sim_present = sim_present;

    bb->out = out;
    bb->opaque = opaque;
    bb->data_out = data_out;
    bb->data_opaque = data_opaque;
    bb->now_ms = now_ms;
    bb->wall_offset_ms = wall_offset_ms;

    /* The FCS/CRC tables are lazy-initialized on first use; the mux rx path
     * can run before anything is ever sent, so make sure they exist. */
    if (!h5_crc_table[1]) {
        h5_crc_init();
    }
    if (!mx_fcs_table[1]) {
        mx_fcs_init();
    }

    bb->ch[0].open = true;                       /* the pre-mux "default" channel */
    /* A modem boots with its radio on: 4.x never sends +cfun=1 at start (only
     * +cfun=4 for airplane mode and +cfun=1 to leave it); 1.0 sends it anyway. */
    bb->cfun = 1;
    bb->call_ch = 1;
    bb->creg_ch = 2;
    bb->xciev_ch = 2;
    bb->xsim_ch = 3;
    bb->sms_ch = 3;
    bb->cops_format = 2;
    bb->next_call_id = 1;
    bb->ceer_cause = 16;                         /* normal call clearing */
    bb->sim_last = sim_present;
    for (int i = 0; i < IOS_BB_MAX_CALLS; i++) {
        bb->calls[i].next_stat = -1;
    }
}

void ios_bb_init(IosBbCore *bb, IosBbOutFn out, void *opaque)
{
    /*
     * No memset here: the caller owns the control block (identity, network,
     * battery) and filled it in before this call; ios_bb_reset clears
     * everything else a fresh baseband starts without.
     */
    bb->out = out;
    bb->opaque = opaque;
    bb->now_ms = 0;
    ios_bb_reset(bb);                            /* fills the transport defaults */
}
