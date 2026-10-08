/*
 * Host unit test for the fake cellular baseband core: drives it through the
 * transport iPhone OS 1.0's CommCenter puts on the wire - raw AT, the Apple H5
 * link (kext CRC), the 27.010 basic mux - then through the doc's init
 * sequence, registration, SIM, calls and SMS (docs/baseband/commcenter-1.0.md
 * and the byte-level exchange examples in commcenter-1.0-calls-sms.md).
 *
 * The test's client side is written independently of the core: the H5 CRC is
 * pinned to the golden vector from the kext ("123456789" -> 0xf689), and the
 * mux FCS is a bit-at-a-time loop rather than the core's table. If either
 * breaks - or the mux framing, the H5 sequencing, or any reply format 1.0
 * parses - this fails.
 */
#include "qemu/osdep.h"
#include "hw/misc/ios_baseband_core.h"

/* ------------------------------------------------------------------- harness */

static int failures;

#define CHECK(cond) do {                                                      \
    if (!(cond)) {                                                            \
        failures++;                                                           \
        fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #cond);       \
    }                                                                         \
} while (0)

static void check_str(const char *a, const char *b, const char *what)
{
    if (strcmp(a, b) != 0) {
        failures++;
        fprintf(stderr, "FAIL %s: \"%s\" != \"%s\"\n", what, a, b);
    }
}

static IosBbCore bb;
static int64_t tnow = 1000;

static uint8_t outbuf[128 * 1024];
static size_t outlen;

static void core_out(void *opaque, const uint8_t *buf, size_t len)
{
    assert(outlen + len < sizeof(outbuf));
    memcpy(outbuf + outlen, buf, len);
    outlen += len;
}

/*
 * Events: what the core sent, decoded by the test's own client stack.
 *   EV_RAW   pre-mux AT stream bytes (inside H5, before the mux starts)
 *   EV_FRAME one mux UIH/control frame (dlci, payload)
 *   EV_LINK  an H5 link-control payload (SYNC RESP, CONFIG RESP)
 *   EV_FLAGS a wake-up-flag answer
 */
enum { EV_RAW, EV_FRAME, EV_LINK, EV_FLAGS };

typedef struct Ev {
    int kind;
    int dlci;
    uint8_t ctrl;
    uint8_t payload[2048];
    unsigned plen;
} Ev;

static Ev evs[512];
static int nev, ev_i;

static void ev_add(int kind, int dlci, uint8_t ctrl,
                   const uint8_t *p, unsigned n)
{
    if (nev >= (int)ARRAY_SIZE(evs) - 1) {
        return;
    }
    if (getenv("BBTEST_TRACE")) {
        fprintf(stderr, "ev[%d] kind=%d dlci=%d plen=%u: ", nev, kind, dlci, n);
        for (unsigned i = 0; i < n && i < 70; i++) {
            if (p[i] == '\r') { fprintf(stderr, "\\r"); }
            else if (p[i] == '\n') { fprintf(stderr, "\\n"); }
            else if (p[i] < 0x20 || p[i] >= 0x7f) { fprintf(stderr, "<%02x>", p[i]); }
            else { fputc(p[i], stderr); }
        }
        fprintf(stderr, "\n");
    }
    Ev *e = &evs[nev++];

    e->kind = kind;
    e->dlci = dlci;
    e->ctrl = ctrl;
    e->plen = n;
    memcpy(e->payload, p, n < sizeof(e->payload) ? n : sizeof(e->payload));
}

/* --------------------------------------------------------- test's H5 client side */

static uint8_t c_tx_seq;          /* our data packet numbering */
static uint8_t c_rx_next;         /* next core seq we expect */
static uint8_t c_acked;          /* last ack value we sent the core */
static bool autoack = true;
static bool test_mux_on;         /* our mux starts after the +cmux OK */

/* Build one H5 packet (the kernel's shape) and feed it to the core. */
static void h5_send(int type, bool reliable, const uint8_t *payload, unsigned len)
{
    uint8_t pkt[4 + 1500 + 2];
    uint8_t wire[2 * sizeof(pkt) + 2];
    unsigned n = 4 + len;
    bool crc = type != 15;

    pkt[0] = (reliable ? 0x80 : 0) | (crc ? 0x40 : 0) | (c_rx_next << 3) |
             (reliable ? c_tx_seq : 0);
    pkt[1] = type | ((len & 0xf) << 4);
    pkt[2] = len >> 4;
    pkt[3] = ~(pkt[0] + pkt[1] + pkt[2]);
    memcpy(pkt + 4, payload, len);
    if (crc) {
        uint16_t c = ios_bb_h5_crc(pkt, n);

        pkt[n++] = c >> 8;
        pkt[n++] = c;
    }

    uint8_t *o = wire;
    *o++ = 0xc0;
    for (unsigned i = 0; i < n; i++) {
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

    bb.now_ms = tnow;
    ios_bb_input(&bb, wire, o - wire);
    if (reliable) {
        c_tx_seq = (c_tx_seq + 1) & 7;
    }
}

static void c_link(const uint8_t *p, unsigned n)
{
    h5_send(15, false, p, n);
}

static void c_data(const uint8_t *p, unsigned n)
{
    h5_send(14, true, p, n);
}

static void c_data_str(const char *s)
{
    c_data((const uint8_t *)s, strlen(s));
}

/* ---------------------------------------------------------- test's mux client side */

static uint8_t m_fcs(uint8_t fcs, const uint8_t *p, unsigned n)
{
    while (n--) {
        fcs ^= *p++;
        for (int i = 0; i < 8; i++) {
            fcs = (fcs & 1) ? (fcs >> 1) ^ 0xe0 : fcs >> 1;
        }
    }
    return fcs;
}

static void mux_frame(uint8_t addr, uint8_t ctrl, const uint8_t *data, unsigned len)
{
    uint8_t f[5 + 1600 + 2];
    unsigned n = 0, hdr;
    uint8_t fcs;

    f[n++] = 0xf9;                                   /* 1.0's CommCenter flags its frames inside H5 */
    f[n++] = addr;
    f[n++] = ctrl;
    if (len < 128) {
        f[n++] = (len << 1) | 1;
    } else {
        f[n++] = (len & 0x7f) << 1;
        f[n++] = len >> 7;
    }
    hdr = n;
    memcpy(f + n, data, len);
    n += len;
    /* 27.010: UIH covers address+control+length only. */
    fcs = m_fcs(0xff, f + 1, hdr - 1);
    if ((ctrl & ~0x10) != 0xef) {
        fcs = m_fcs(fcs, data, len);
    }
    f[n++] = 0xff - fcs;
    f[n++] = 0xf9;
    c_data(f, n);
}

static void c_mux(int dlci, const uint8_t *p, unsigned n)
{
    mux_frame((dlci << 2) | 0x03, 0xef, p, n);       /* C/R set: command */
}

static void c_mux_str(int dlci, const char *s)
{
    c_mux(dlci, (const uint8_t *)s, strlen(s));
}

static void c_sabm(int dlci, uint8_t ctrl)
{
    mux_frame((dlci << 2) | 0x03, ctrl, NULL, 0);
}

/* Parse one mux frame out of the core's data payload, the way the kernel and
 * then CommCenter would: [addr][ctrl][len][info][fcs] with a bitwise FCS. */
static void mux_payload(const uint8_t *p, unsigned n)
{
    unsigned pos = 0;

    while (pos + 3 < n) {
        if (p[pos] == 0xf9) {                        /* each frame between flags */
            pos++;
            continue;
        }
        uint8_t addr = p[pos];
        uint8_t ctrl = p[pos + 1];
        unsigned len = p[pos + 2] >> 1;
        unsigned lenbytes = 1;

        if (!(p[pos + 2] & 1)) {
            if (pos + 4 > n) {
                break;
            }
            len |= p[pos + 3] << 7;
            lenbytes = 2;
        }
        if (pos + 2 + lenbytes + len + 1 > n) {
            break;
        }
        const uint8_t *info = p + pos + 2 + lenbytes;
        uint8_t fcs = m_fcs(0xff, p + pos, 2 + lenbytes);

        if ((ctrl & ~0x10) != 0xef) {
            fcs = m_fcs(fcs, info, len);
        }
        CHECK(m_fcs(fcs, &p[pos + 2 + lenbytes + len], 1) == 0xcf);
        ev_add(EV_FRAME, addr >> 2, ctrl, info, len);
        pos += 2 + lenbytes + len + 1;
    }
}

/* One H5 packet the core sent, after SLIP deframing. */
static void h5_rx_pkt(const uint8_t *p, unsigned n)
{
    unsigned len = (p[1] >> 4) | (p[2] << 4);
    unsigned type = p[1] & 0xf;

    if (n < 4) {
        CHECK(!"short H5 packet");
        return;
    }
    CHECK((uint8_t)(p[0] + p[1] + p[2]) == (uint8_t)~p[3]);
    if (4 + len + ((p[0] >> 6) & 1) * 2 != n) {
        CHECK(!"H5 length mismatch");
        return;
    }
    if (p[0] & 0x40) {
        uint16_t got = (p[4 + len] << 8) | p[4 + len + 1];
        uint16_t want = ios_bb_h5_crc(p, 4 + len);

        if (got != want) {
            CHECK(!"H5 CRC mismatch on a core packet");
            return;
        }
    }
    if (type == 15) {
        ev_add(EV_LINK, 0, 0, p + 4, len);
        return;
    }
    if (type == 0) {
        return;                                  /* pure ack; not interesting */
    }
    if (type != 14 || !(p[0] & 0x80)) {
        CHECK(!"unexpected H5 type");
        return;
    }
    uint8_t seq = p[0] & 7;

    if (seq == c_rx_next) {
        c_rx_next = (seq + 1) & 7;
    } else if (seq == ((c_rx_next - 1) & 7)) {
        /* a retransmission of what we have; still an event to observe */
    } else {
        CHECK(!"seq desync from the core");
        c_rx_next = (seq + 1) & 7;
    }

    const uint8_t *info = p + 4;
    bool all_flags = true;

    for (unsigned i = 0; i < len; i++) {
        if (info[i] != 0xf9) {
            all_flags = false;
            break;
        }
    }
    if (all_flags && len) {
        ev_add(EV_FLAGS, 0, 0, info, len);
        return;
    }
    if (test_mux_on) {
        CHECK(len >= 2 && info[0] == 0xf9 && info[len - 1] == 0xf9);   /* flagged, as 1.0 wants */
        mux_payload(info, len);
    } else {
        ev_add(EV_RAW, 0, 0, info, len);
    }
}

/*
 * Consume pending output into events, acking what arrived. Events accumulate:
 * expectations walk the stream in order, and the acks leave promptly the way
 * the kernel's would (otherwise the core legitimately retransmits at 250 ms
 * while we step time without pumping).
 */
static void pump(void)
{
    if (!bb.h5) {
        /* Raw AT phase: the output is plain bytes, not SLIP. */
        if (outlen) {
            ev_add(EV_RAW, 0, 0, outbuf, outlen);
        }
        outlen = 0;
        return;
    }

    for (int round = 0; round < 8; round++) {
        /* SLIP-deframe the pending output (unescaping 0xDB 0xDC/0xDD). */
        uint8_t slip[8192];
        unsigned slen = 0;

        for (size_t i = 0; i < outlen; i++) {
            if (outbuf[i] == 0xc0) {
                if (slen) {
                    h5_rx_pkt(slip, slen);
                    slen = 0;
                }
                continue;
            }
            uint8_t c = outbuf[i];

            if (c == 0xdb && i + 1 < outlen) {
                i++;
                c = outbuf[i] == 0xdc ? 0xc0 : outbuf[i] == 0xdd ? 0xdb : 0;
                if (!c) {
                    CHECK(!"bad SLIP escape from the core");
                    slen = 0;
                    continue;
                }
            }
            if (slen < sizeof(slip)) {
                slip[slen++] = c;
            }
        }
        outlen = 0;

        /*
         * Piggyback acks ride our next packet; when there is none, the kernel
         * sends a pure ack. Without it the core retransmits (and the H5
         * window test below relies on doing that once by hand).
         */
        if (autoack && c_rx_next != c_acked) {
            c_acked = c_rx_next;
            h5_send(0, false, NULL, 0);
            continue;                            /* reparse: the ack may drain more */
        }
        break;
    }
}

/* ---------------------------------------------------------------- expectations */

static Ev *ev_next(void)
{
    return ev_i < nev ? &evs[ev_i++] : NULL;
}

static void expect_none(void)
{
    if (ev_i < nev) {
        failures++;
        fprintf(stderr, "FAIL unexpected event (kind %d)\n", evs[ev_i].kind);
        ev_i = nev;
    }
}

static Ev *expect_kind(int kind)
{
    Ev *e = ev_next();

    if (!e || e->kind != kind) {
        failures++;
        fprintf(stderr, "FAIL %d: wanted event kind %d\n", __LINE__, kind);
        return NULL;
    }
    return e;
}

static void expect_raw(const char *s)
{
    Ev *e = expect_kind(EV_RAW);

    if (!e) {
        return;
    }
    e->payload[e->plen < 2047 ? e->plen : 2047] = 0;
    check_str((char *)e->payload, s, "raw AT reply");
}

static void expect_frame(int dlci, const char *s)
{
    Ev *e = expect_kind(EV_FRAME);

    if (!e) {
        return;
    }
    CHECK(e->dlci == dlci);
    if (e->plen != strlen(s) || memcmp(e->payload, s, e->plen)) {
        failures++;
        e->payload[e->plen < 2047 ? e->plen : 2047] = 0;
        fprintf(stderr, "FAIL frame %d: \"%s\" != \"%s\"\n", dlci,
                (char *)e->payload, s);
    }
}

static void expect_ua(int dlci, uint8_t ctrl)
{
    Ev *e = expect_kind(EV_FRAME);

    if (!e) {
        return;
    }
    CHECK(e->dlci == dlci);
    CHECK(e->ctrl == ctrl);
    CHECK(e->plen == 0);
}

static void expect_link(const uint8_t *p, unsigned n)
{
    Ev *e = expect_kind(EV_LINK);

    if (!e) {
        return;
    }
    CHECK(e->plen == n);
    CHECK(memcmp(e->payload, p, n) == 0);
}

/* Feed helpers that keep the core's clock moving. */
static void feed_raw(const char *s)
{
    bb.now_ms = tnow;
    ios_bb_input(&bb, (const uint8_t *)s, strlen(s));
    pump();
}

static void tick_to(int64_t t)
{
    while (tnow < t) {
        tnow += 10;
        ios_bb_tick(&bb, tnow);
        pump();                                  /* ack what the core sends */
    }
}

/* ----------------------------------------------------------------------- tests */

/* "123456789" -> 0xf689 is the golden vector from the kext disassembly. */
static void test_h5_crc(void)
{
    CHECK(ios_bb_h5_crc((const uint8_t *)"123456789", 9) == 0xf689);
}

static void test_boot(void)
{
    /* Controls first, then init: init keeps them. */
    snprintf(bb.operator_long, sizeof(bb.operator_long), "Test Network");
    snprintf(bb.plmn, sizeof(bb.plmn), "00101");
    snprintf(bb.imei, sizeof(bb.imei), "000000001234569");
    snprintf(bb.imsi, sizeof(bb.imsi), "001010000000001");
    snprintf(bb.iccid, sizeof(bb.iccid), "89001010000000000001");
    bb.signal_dbm = -59;
    bb.battery = 100;
    bb.registered = true;
    bb.sim_present = true;
    bb.lac = 0x1abf;
    bb.ci = 0x53f1;
    bb.answer_delay_ms = -1;               /* QMP answers calls instead */
    bb.nitz = true;                        /* the M68's: kept by init */
    ios_bb_init(&bb, core_out, NULL);
    tnow = 1000;

    /* -- raw AT: CommCenter's first probes (docs/baseband/commcenter-1.0.md) */
    feed_raw("at\r");
    pump();
    expect_raw("\r\nOK\r\n");

    feed_raw("ate0\r");
    pump();
    expect_raw("\r\nOK\r\n");

    feed_raw("at+xsio?\r");
    pump();
    expect_raw("\r\n+XSIO: 0,*0\r\n\r\nOK\r\n");

    feed_raw("at+ipr=750000\r");
    pump();
    expect_raw("\r\nOK\r\n");

    /* The kernel snoops this and is in H5 from the write on: the OK must not come
     * back raw (the kernel would drop it as line noise), it is the first H5 data. */
    feed_raw("at+xtransportmode\r");
    pump();
    expect_none();

    /* -- H5 link establishment: SYNC then CONFIG with cfg 0x17 */
    c_link((const uint8_t[]){ 0x01, 0x7e }, 2);
    pump();
    expect_link((const uint8_t[]){ 0x02, 0x7d }, 2);

    c_link((const uint8_t[]){ 0x03, 0xfc, 0x17 }, 3);
    pump();
    expect_link((const uint8_t[]){ 0x04, 0x7b, 0x17 }, 3);
    expect_raw("\r\nOK\r\n");                    /* +xtransportmode's */

    /* Reliable data now flows; the kernel re-delivers what was queued. */
    c_data_str("ate0\r");
    pump();
    expect_raw("\r\nOK\r\n");

    /* -- retransmit: the core resends what we did not acknowledge */
    autoack = false;
    c_data_str("at+cmee=1\r");
    pump();
    expect_raw("\r\nOK\r\n");

    tnow += 251;
    ios_bb_tick(&bb, tnow);
    pump();
    expect_raw("\r\nOK\r\n");                    /* same unacked reply */

    autoack = true;
    h5_send(0, false, NULL, 0);                   /* the overdue ack */
    pump();
    expect_none();
    tnow += 300;
    ios_bb_tick(&bb, tnow);
    pump();
    expect_none();                               /* and it stops */

    /* -- multiplexer: +cmux=0,0,0,1500, then SABM DLCI 0..5 */
    c_data_str("at+cmux=0,0,0,1500\r");
    pump();
    expect_raw("\r\nOK\r\n");
    test_mux_on = true;                           /* the OK ends the pre-mux stream */

    c_sabm(0, 0x2f);
    pump();
    expect_ua(0, 0x63);
    for (int dlci = 1; dlci <= 5; dlci++) {
        c_sabm(dlci, 0x2f);
        pump();
        expect_ua(dlci, 0x63);
    }
    /* P bit is mirrored into the UA */
    c_sabm(2, 0x3f);
    pump();
    expect_ua(2, 0x73);

    /* -- the SIM channel's existence pokes the SIM model (+XSIM: 1) */
    tick_to(tnow + 150);
    pump();
    expect_frame(3, "\r\n+XSIM: 1\r\n");
    expect_none();

    /* -- the queued per-channel init: e0, +cmee=1, +cscs="HEX" everywhere */
    for (int dlci = 1; dlci <= 5; dlci++) {
        c_mux_str(dlci, "ate0\r");
        pump();
        expect_frame(dlci, "\r\nOK\r\n");
    }

    /* -- radio on, registration enables, then the URC burst (DLCI 1/2) */
    c_mux_str(1, "at+cfun=1\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");

    c_mux_str(1, "at+xpow=5,250,0\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");

    c_mux_str(2, "at+xmer=1\r");
    pump();
    expect_frame(2, "\r\nOK\r\n");
    c_mux_str(2, "at+creg=2\r");
    pump();
    expect_frame(2, "\r\nOK\r\n");
    c_mux_str(2, "at+cgreg=1\r");
    pump();
    expect_frame(2, "\r\nOK\r\n");
    c_mux_str(2, "at+cops=0\r");
    pump();
    expect_frame(2, "\r\nOK\r\n");
    c_mux_str(2, "at+cops=3,2\r");
    pump();
    expect_frame(2, "\r\nOK\r\n");
    c_mux_str(2, "at+ctzr=1\r");
    pump();
    expect_frame(2, "\r\nOK\r\n");

    tick_to(tnow + 350);                          /* searching */
    pump();
    expect_frame(2, "\r\n+CREG: 2\r\n");
    expect_frame(2, "\r\n+CGREG: 2\r\n");
    /* NITZ on registration: the host's offset in quarter hours (EDT: -16) and the time, UTC. */
    setenv("TZ", "America/New_York", 1);
    tzset();
    int64_t wall = bb.wall_offset_ms;
    bb.wall_offset_ms = 1791309600900LL - (tnow + 500);     /* 2026-10-06 18:00:00.9 UTC at the tick */
    tick_to(tnow + 500);                          /* registered on the home PLMN */
    pump();
    expect_frame(2, "\r\n+CREG: 1,1ABF,53F1\r\n");
    expect_frame(2, "\r\n+CTZV: -16,\"26/10/06,18:00:00\"\r\n");
    bb.nitz = false;                              /* the other boards' modem from here on */
    bb.wall_offset_ms = wall;
    expect_frame(2, "\r\n+CGREG: 1\r\n");
    expect_frame(2, "\r\n+XCIEV: 27,100\r\n");
    expect_none();

    /* Queries. */
    c_mux_str(2, "at+cops?\r");
    pump();
    expect_frame(2, "\r\n+COPS: 0,2,\"3030313031\"\r\n");
    expect_frame(2, "\r\nOK\r\n");
    c_mux_str(2, "at+creg?\r");
    pump();
    expect_frame(2, "\r\n+CREG: 2,1,1ABF,53F1\r\n");
    expect_frame(2, "\r\nOK\r\n");
    c_mux_str(2, "at+xcops=7\r");
    pump();
    expect_frame(2, "\r\n+XCOPS: 0,\"54657374204E6574776F726B\"\r\n");
    expect_frame(2, "\r\nOK\r\n");

    /* -- SIM identity chain, once the model has asked its first +cpin? */
    c_mux_str(3, "at+cpin?\r");
    pump();
    expect_frame(3, "\r\n+CPIN: READY\r\n");
    expect_frame(3, "\r\nOK\r\n");
    c_mux_str(3, "at+cimi\r");
    pump();
    expect_frame(3, "\r\n001010000000001\r\n");
    expect_frame(3, "\r\nOK\r\n");
    c_mux_str(3, "at+ccid\r");
    pump();
    expect_frame(3, "\r\n89001010000000000001\r\n");
    expect_frame(3, "\r\nOK\r\n");
    c_mux_str(3, "at+xpincnt\r");
    pump();
    expect_frame(3, "\r\n+XPINCNT: 3,3,10,10\r\n");
    expect_frame(3, "\r\nOK\r\n");
    c_mux_str(3, "at+clck=\"fd\",2\r");
    pump();
    expect_frame(3, "\r\n+CLCK: 0\r\n");
    expect_frame(3, "\r\nOK\r\n");

    /* -- SMS setup (raw sends; replies consumed as strays are OK here) */
    c_mux_str(3, "at+cnmi=1,2,2,1\r");
    pump();
    expect_frame(3, "\r\nOK\r\n");
    c_mux_str(3, "at+csms=1\r");
    pump();
    expect_frame(3, "\r\n+CSMS: 1,1,1\r\n");
    expect_frame(3, "\r\nOK\r\n");
}

/*
 * The calls-sms doc's worked example 4.1: an incoming call from +1 415 555 0100,
 * answered, hung up by the remote party - byte for byte.
 */
static void test_incoming_call(void)
{
    CHECK(ios_bb_incoming_call(&bb, "+14155550100"));
    pump();
    expect_frame(1, "\r\n+XCALLSTAT: 1,4\r\n");
    expect_frame(1, "\r\n+CLIP: \"14155550100\",145,,,\"\",0\r\n");
    expect_frame(1, "\r\nRING\r\n");

    /* the ring repeats */
    tick_to(tnow + 3000);
    pump();
    expect_frame(1, "\r\nRING\r\n");

    check_str(ios_bb_call_state(&bb), "incoming", "call state");

    c_mux_str(1, "at+chld=2\r");                  /* user swipes answer */
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 1,0\r\n");
    check_str(ios_bb_call_state(&bb), "active", "call state");

    ios_bb_remote_hangup(&bb);                    /* the remote hangs up */
    pump();
    expect_frame(1, "\r\n+XCALLSTAT: 1,6\r\n");

    c_mux_str(1, "at+ceer\r");                     /* 1.0 asks for the cause */
    pump();
    expect_frame(1, "\r\n+CEER: CC,16\r\n");
    expect_frame(1, "\r\nOK\r\n");
    check_str(ios_bb_call_state(&bb), "idle", "call state");

    /* 4.x answers with ATA and hangs up with ATH. */
    CHECK(ios_bb_incoming_call(&bb, "15555550199"));
    pump();
    expect_frame(1, "\r\n+XCALLSTAT: 2,4\r\n");
    expect_frame(1, "\r\n+CLIP: \"15555550199\",145,,,\"\",0\r\n");
    ev_i = nev;
    c_mux_str(1, "ata\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,0\r\n");
    c_mux_str(1, "ath\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,6\r\n");
    check_str(ios_bb_call_state(&bb), "idle", "call state");
    bb.next_call_id = 2;                         /* the 1.0 sections below expect id 2 next */
}

/* Worked example 4.3: user dials +1 415 555 0100 and hangs up. */
static void test_outgoing_call(void)
{
    c_mux_str(1, "atd+14155550100;\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,2\r\n");
    check_str(ios_bb_call_state(&bb), "dialing", "call state");
    check_str(bb.last_dialed, "+14155550100", "last dialed");

    tick_to(tnow + 2100);                         /* remote starts ringing */
    pump();
    expect_frame(1, "\r\n+XCALLSTAT: 2,3\r\n");
    check_str(ios_bb_call_state(&bb), "alerting", "call state");

    ios_bb_remote_answer(&bb);
    pump();
    /* the connected line: 1.0's only source for the answered call's number */
    expect_frame(1, "\r\n+COLP: \"+14155550100\",145\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,0\r\n");
    check_str(ios_bb_call_state(&bb), "active", "call state");

    c_mux_str(1, "at+chld=1\r");                 /* local hang up */
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,6\r\n");
    check_str(ios_bb_call_state(&bb), "idle", "call state");
    bb.next_call_id = 2;

    /* +COLP=0 turns it off; the setting reads back through +COLP? */
    c_mux_str(1, "at+colp=0\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    c_mux_str(1, "at+colp?\r");
    pump();
    expect_frame(1, "\r\n+COLP: 0,1\r\n");
    expect_frame(1, "\r\nOK\r\n");
    c_mux_str(1, "atd5550100;\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,2\r\n");
    ios_bb_remote_answer(&bb);
    pump();
    expect_frame(1, "\r\n+XCALLSTAT: 2,0\r\n");
    c_mux_str(1, "at+chld=1\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,6\r\n");
    c_mux_str(1, "at+colp=1\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    bb.next_call_id = 2;                         /* the 1.0 sections below expect id 2 next */
}

/*
 * The lock screen's emergency dialer (issue 32): CommCenter asks +XEMN first and dials
 * only when field 1 says emergency; the call then runs as any other, bracketed by
 * +XEMC: 1/0 once +XEMC=1 asked for them.
 */
static void test_emergency_call(void)
{
    c_mux_str(1, "at+xemn=\"5550100\"\r");
    pump();
    expect_frame(1, "\r\n+XEMN: \"5550100\",0\r\n");
    expect_frame(1, "\r\nOK\r\n");
    c_mux_str(1, "at+xemn=\"911\"\r");
    pump();
    expect_frame(1, "\r\n+XEMN: \"911\",1\r\n");
    expect_frame(1, "\r\nOK\r\n");
    c_mux_str(1, "at+xemc=1\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");

    c_mux_str(1, "atd911;\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XEMC: 1\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,2\r\n");
    CHECK(ios_bb_emergency_call(&bb));
    check_str(bb.last_dialed, "911", "last dialed");
    ios_bb_remote_answer(&bb);
    pump();
    expect_frame(1, "\r\n+COLP: \"911\",129\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,0\r\n");
    check_str(ios_bb_call_state(&bb), "active", "call state");
    c_mux_str(1, "ath\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 2,6\r\n");
    expect_frame(1, "\r\n+XEMC: 0\r\n");
    CHECK(!ios_bb_emergency_call(&bb));

    /* An ordinary number: no +XEMC, not an emergency call. */
    c_mux_str(1, "atd5550100;\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 3,2\r\n");
    CHECK(!ios_bb_emergency_call(&bb));
    c_mux_str(1, "ath\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    expect_frame(1, "\r\n+XCALLSTAT: 3,6\r\n");
    c_mux_str(1, "at+xemc=0\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    bb.next_call_id = 2;                         /* the 1.0 sections below expect id 2 next */
}

/*
 * The M68's vibration motor, which hangs off the modem: CommCenter 1.0 runs it once for an SMS
 * (+xdrv=4,0,1,12,400,399), on 1 s of every 2 while ringing (2,12,2000,1000), and stops it (0,0,0,0).
 */
static void test_vibrator(void)
{
    int64_t t0 = tnow;

    CHECK(!ios_bb_vibrating(&bb));
    c_mux_str(1, "at+xdrv=4,0,1,12,400,399\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    CHECK(ios_bb_vibrating(&bb));
    CHECK(ios_bb_next_due(&bb) && ios_bb_next_due(&bb) <= t0 + 399);
    tick_to(t0 + 390);
    CHECK(ios_bb_vibrating(&bb));
    tick_to(t0 + 400);
    CHECK(!ios_bb_vibrating(&bb));

    t0 = tnow;
    c_mux_str(1, "at+xdrv=4,0,2,12,2000,1000\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    tick_to(t0 + 990);
    CHECK(ios_bb_vibrating(&bb));
    tick_to(t0 + 1010);
    CHECK(!ios_bb_vibrating(&bb));
    tick_to(t0 + 1990);
    CHECK(!ios_bb_vibrating(&bb));
    tick_to(t0 + 2010);
    CHECK(ios_bb_vibrating(&bb));
    CHECK(ios_bb_next_due(&bb) && ios_bb_next_due(&bb) <= t0 + 3000);

    c_mux_str(1, "at+xdrv=4,0,0,0,0,0\r");
    pump();
    expect_frame(1, "\r\nOK\r\n");
    CHECK(!ios_bb_vibrating(&bb));
    tick_to(tnow + 2000);
    CHECK(!ios_bb_vibrating(&bb));
}

/*
 * The calls-sms doc's worked example 4.2: an SMS from 14155550100 saying
 * "Hello from 2007", acked with +CNMA, and read back with +CMGR.
 */
static void test_incoming_sms(void)
{
    /* 2007-04-02 13:52:18 UTC: the SCTS of the doc's example. */
    struct tm tm = { 0 };

    tm.tm_year = 107;
    tm.tm_mon = 3;
    tm.tm_mday = 2;
    tm.tm_hour = 13;
    tm.tm_min = 52;
    tm.tm_sec = 18;
    tnow = (int64_t)timegm(&tm) * 1000;
    ios_bb_tick(&bb, tnow);

    CHECK(ios_bb_incoming_sms(&bb, "14155550100", "Hello from 2007"));
    pump();
    /* Both +CMT lines ride one UIH frame; 1.0 splits the response into
     * lines itself and only reads line 1. */
    expect_frame(3, "\r\n+CMT: ,33\r\n"
        "\r\n00040B914151550501F00000704020312581000FC8329BFD0699E5EF36480683DD00\r\n");

    c_mux_str(3, "at+cnma\r");
    pump();
    expect_frame(3, "\r\nOK\r\n");

    /* read it back the way a +CMTI-driven +CMGR would */
    c_mux_str(3, "at+cmgr=1\r");
    pump();
    expect_frame(3, "\r\n+CMGR: 1,,33\r\n"
        "\r\n00040B914151550501F00000704020312581000FC8329BFD0699E5EF36480683DD00\r\n");
    expect_frame(3, "\r\nOK\r\n");

    /* A "+" sender is the same international address (TOA 0x91): the same PDU. */
    CHECK(ios_bb_incoming_sms(&bb, "+14155550100", "Hello from 2007"));
    pump();
    expect_frame(3, "\r\n+CMT: ,33\r\n"
        "\r\n00040B914151550501F00000704020312581000FC8329BFD0699E5EF36480683DD00\r\n");
    c_mux_str(3, "at+cnma\r");
    pump();
    expect_frame(3, "\r\nOK\r\n");
    CHECK(ios_bb_sms_sender_ok("+14155550100") && ios_bb_sms_sender_ok("5550100"));
    CHECK(!ios_bb_sms_sender_ok("+") && !ios_bb_sms_sender_ok("") && !ios_bb_sms_sender_ok("Apple") &&
          !ios_bb_sms_sender_ok("1+2") && !ios_bb_sms_sender_ok("123456789012345678901"));
    CHECK(!ios_bb_incoming_sms(&bb, "Apple", "alphanumeric senders are not modeled"));

    /* URCs follow the DLCI that enabled them (4.x lays its channels out its own way). */
    c_mux_str(4, "at+cnmi=1,2,2,1\r");
    pump();
    expect_frame(4, "\r\nOK\r\n");
    CHECK(ios_bb_incoming_sms(&bb, "14155550100", "Hi"));
    pump();
    expect_frame(4, "\r\n+CMT: ,21\r\n\r\n00040B914151550501F000007040203125810002C834\r\n");
    c_mux_str(3, "at+cnmi=1,2,2,1\r");
    pump();
    expect_frame(3, "\r\nOK\r\n");
}

/* Worked example 4.3: the user sends "Hi" to +1 415 555 0100. */
static void test_outgoing_sms(void)
{
    c_mux_str(3, "at+cmgs=15\r");
    pump();
    expect_frame(3, "\r\n> ");

    const char pdu[] = "0001000B814151550501F0000002C834";

    c_mux_str(3, pdu);
    c_mux(3, (const uint8_t[]){ 0x1a }, 1);      /* Ctrl-Z sends it */
    pump();
    expect_frame(3, "\r\n+CMGS: 1\r\n");
    expect_frame(3, "\r\nOK\r\n");

    check_str(ios_bb_last_mo_sms_number(&bb), "14155550100", "MO number");
    check_str(ios_bb_last_mo_sms_text(&bb), "Hi", "MO text");
    CHECK(bb.mo_count == 1);                     /* mo-sms-count: the host sees each send */

    /* The host panel's network fields, validated as the modem's setters do. */
    CHECK(ios_bb_plmn_ok("00101") && ios_bb_plmn_ok("310410") && !ios_bb_plmn_ok("0010") &&
          !ios_bb_plmn_ok("0010123") && !ios_bb_plmn_ok("00a01") && !ios_bb_plmn_ok(""));
    CHECK(ios_bb_carrier_ok("Test Network") && ios_bb_carrier_ok("12345678901234567890123456789012") &&
          !ios_bb_carrier_ok("123456789012345678901234567890123") && !ios_bb_carrier_ok("") &&
          !ios_bb_carrier_ok("a\"b") && !ios_bb_carrier_ok("a\nb"));
}

/* A text the default alphabet cannot carry goes out as UCS2 (DCS 08). */
static void test_incoming_sms_ucs2(void)
{
    struct tm tm = { 0 };

    tm.tm_year = 107;
    tm.tm_mon = 3;
    tm.tm_mday = 2;
    tm.tm_hour = 13;
    tm.tm_min = 52;
    tm.tm_sec = 18;
    tnow = (int64_t)timegm(&tm) * 1000;
    ios_bb_tick(&bb, tnow);

    /* trade mark: outside the GSM alphabet, so DCS 08 / UTF-16BE */
    CHECK(ios_bb_incoming_sms(&bb, "14155550100", "\xe2\x84\xa2"));
    pump();
    expect_frame(3, "\r\n+CMT: ,21\r\n"
        "\r\n00040B914151550501F0000870402031258100022122\r\n");
}

/* Calls fail without a network: the dial parser wants a real error final. */
static void test_dial_no_service(void)
{
    bb.registered = false;
    ios_bb_changed(&bb);
    tick_to(tnow + 500);
    pump();
    expect_frame(2, "\r\n+CREG: 2\r\n");           /* still searching */
    expect_frame(2, "\r\n+CGREG: 2\r\n");

    CHECK(!ios_bb_incoming_call(&bb, "14155550100"));
    CHECK(!ios_bb_incoming_sms(&bb, "14155550100", "no dice"));

    c_mux_str(1, "atd+14155550100;\r");
    pump();
    expect_frame(1, "\r\nERROR\r\n");

    bb.registered = true;
    ios_bb_changed(&bb);
    tick_to(tnow + 500);
    pump();
    expect_frame(2, "\r\n+CREG: 2\r\n");
    expect_frame(2, "\r\n+CGREG: 2\r\n");
    tick_to(tnow + 500);
    pump();
    expect_frame(2, "\r\n+CREG: 1,1ABF,53F1\r\n");
    expect_frame(2, "\r\n+CGREG: 1\r\n");
    expect_frame(2, "\r\n+XCIEV: 27,100\r\n");
}

/* Signal and battery changes reach the guest as +XCIEV. */
static void test_signal_change(void)
{
    bb.signal_dbm = -93;
    ios_bb_changed(&bb);
    pump();
    expect_frame(2, "\r\n+XCIEV: 10,100\r\n");

    bb.battery = 42;
    ios_bb_changed(&bb);
    pump();
    expect_frame(2, "\r\n+XCIEV: 10,42\r\n");

    bb.signal_dbm = -59;
    bb.battery = 100;
    ios_bb_changed(&bb);
    pump();
    expect_frame(2, "\r\n+XCIEV: 27,100\r\n");
}

/* SIM removal and re-insertion are signaled with +XSIM: n. */
static void test_sim_removal(void)
{
    bb.sim_present = false;
    ios_bb_changed(&bb);
    tick_to(tnow + 150);
    pump();
    expect_frame(3, "\r\n+XSIM: 0\r\n");

    c_mux_str(3, "at+cpin?\r");
    pump();
    expect_frame(3, "\r\n+CME ERROR: 10\r\n");

    bb.sim_present = true;
    ios_bb_changed(&bb);
    tick_to(tnow + 150);
    pump();
    expect_frame(3, "\r\n+XSIM: 1\r\n");

    c_mux_str(3, "at+cpin?\r");
    pump();
    expect_frame(3, "\r\n+CPIN: READY\r\n");
    expect_frame(3, "\r\nOK\r\n");

    /* EF_SST exists (4.2.1 installs no carrier bundle without it); optional EFs are 94 04. */
    c_mux_str(3, "at+crsm=192,28472\r");
    pump();
    expect_frame(3, "\r\n+CRSM: 144,0,\"000000046F38040014FF4401020000\"\r\n");
    expect_frame(3, "\r\nOK\r\n");
    c_mux_str(3, "at+crsm=176,28472,0,0,4\r");
    pump();
    expect_frame(3, "\r\n+CRSM: 144,0,\"FF000000\"\r\n");
    expect_frame(3, "\r\nOK\r\n");
    c_mux_str(3, "at+crsm=192,28436\r");
    pump();
    expect_frame(3, "\r\n+CRSM: 148,4\r\n");
    expect_frame(3, "\r\nOK\r\n");
}

/* Wake-up flags get flags back; PSC is acked; CLD closes the multiplexer. */
static void test_power_and_mux_close(void)
{    /* four flags in, at least two come back */
    const uint8_t flags[4] = { 0xf9, 0xf9, 0xf9, 0xf9 };

    c_data(flags, sizeof(flags));
    pump();
    Ev *e = expect_kind(EV_FLAGS);

    CHECK(e && e->plen >= 2);

    /* PSC: the model acks power save (the kext exits with wake-up flags) */
    const uint8_t psc[2] = { 0x23, 0x01 };

    c_mux(0, psc, sizeof(psc));
    pump();
    while (ev_i < nev && evs[ev_i].kind == EV_FLAGS) {
        ev_i++;           /* the frame's own opening flag extends the wake-up run */
    }
    expect_frame(0, "\x21\x01");

    /* CLD: acked, then the modem is back to plain AT on channel 0 */
    const uint8_t cld[2] = { 0x63, 0x01 };

    c_mux(0, cld, sizeof(cld));
    pump();
    expect_frame(0, "\x61\x01");
    test_mux_on = false;                         /* the mux is closed */

    c_data_str("at\r");
    pump();
    expect_raw("\r\nOK\r\n");
}


/* ------------------------------------------------------------ IFX SPI framing */

/* One AP transfer of n bytes carrying payload; returns the modem's payload. */
static unsigned ifx_frame(IosBbIfx *x, IosBbCore *c, const char *payload,
                          uint8_t *miso, size_t n, char *got)
{
    uint8_t mosi[2048] = { 0 };
    const uint8_t *rx;
    size_t rxlen, plen = strlen(payload);
    unsigned len;

    mosi[0] = plen;
    mosi[1] = plen >> 8;
    memcpy(mosi + IOS_BB_IFX_HDR, payload, plen);
    ios_bb_ifx_xfer(x, mosi, miso, n, &rx, &rxlen);
    CHECK(rxlen == plen && (!plen || memcmp(rx, payload, plen) == 0));
    if (rxlen) {
        ios_bb_input(c, rx, rxlen);
    }
    len = miso[0] | (miso[1] & 0xf) << 8;
    memcpy(got, miso + IOS_BB_IFX_HDR, len);
    got[len] = 0;
    return len;
}

static void test_ifx(void)
{
    static IosBbCore c;
    static IosBbIfx x;
    uint8_t miso[2048];
    char got[2048];

    /* v1 (3GS DT: protocol-version 1, max-data-size 0x7f8). */
    memset(&c, 0, sizeof(c));
    c.registered = c.sim_present = true;
    snprintf(c.imei, sizeof(c.imei), "000000001234569");
    ios_bb_ifx_init(&x, 1, 0x7f8);
    ios_bb_init(&c, ios_bb_ifx_queue, &x);
    CHECK(!ios_bb_ifx_pending(&x));
    ifx_frame(&x, &c, "at\r", miso, 0x7fc, got);
    CHECK(!(miso[3] & 0x40));                   /* bit 6 clear: N88 re-polls while it is set */
    CHECK(ios_bb_ifx_pending(&x));              /* the OK wants SRDY */
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    check_str(got, "\r\nOK\r\n", "ifx v1 OK");

    /* 4.x signal: the engineering page CommCenter polls once registered. */
    c.signal_dbm = -60;
    ifx_frame(&x, &c, "at+cfun=1\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "at+xcgedpage=0,1\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    check_str(got, "\r\n+XCGEDPAGE: RAT:\"GSM\",Rssi: 50\r\n\r\nOK\r\n",
              "xcgedpage");
    CHECK(!(miso[1] & 0x10) && !ios_bb_ifx_pending(&x));

    /* A reply longer than a frame splits with the more bit. */
    ifx_frame(&x, &c, "at+cgsn\r", miso, IOS_BB_IFX_HDR + 8, got);
    ifx_frame(&x, &c, "", miso, IOS_BB_IFX_HDR + 8, got);
    check_str(got, "\r\n000000", "ifx v1 first slice");
    CHECK(miso[1] & 0x10);
    while (ios_bb_ifx_pending(&x)) {
        ifx_frame(&x, &c, "", miso, IOS_BB_IFX_HDR + 8, got);
    }
    CHECK(!(miso[1] & 0x10));

    /* iOS 6 signal: +xsigstr=1 -> +XSIGSTR: 2,<RSCP_LEV 0-91>,<Ec/No_LEV 0-49> (TS 25.133). */
    c.signal_dbm = -70;
    ifx_frame(&x, &c, "at+xsigstr=1\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    CHECK(strstr(got, "+XSIGSTR: 2,46,45") != NULL);   /* -70 dBm = RSCP_LEV 46 */
    c.signal_dbm = -200;                         /* clamped to the range */
    ios_bb_changed(&c);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    CHECK(strstr(got, "+XSIGSTR: 2,0,45") != NULL);
    c.signal_dbm = -60;

    /* Call forwarding: nothing is forwarded. CommCenter reads +XCFC: 3 as not
     * active (no +XCFC line shows the forwarding arrow); +CCFC queries
     * answer status 0 for the asked class (27.007 7.11). */
    ifx_frame(&x, &c, "at+xcfc\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    check_str(got, "\r\n+XCFC: 3\r\n\r\nOK\r\n", "xcfc");
    ifx_frame(&x, &c, "at+ccfc=0,2\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    check_str(got, "\r\n+CCFC: 0,7\r\n\r\nOK\r\n", "ccfc query");
    ifx_frame(&x, &c, "at+ccfc=2,2,,,1\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    check_str(got, "\r\n+CCFC: 0,1\r\n\r\nOK\r\n", "ccfc query, class 1");
    ifx_frame(&x, &c, "at+ccfc=0,3,\"5551234\",129\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    check_str(got, "\r\nOK\r\n", "ccfc registration");

    /* 3GS temperature notifications: +xdrv=5,16,<s> then +XDRVI: 5,17 every <s> seconds. */
    ifx_frame(&x, &c, "at+xdrv=5,16,20\r", miso, 0x7fc, got);
    ios_bb_tick(&c, c.now_ms + 1500);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    CHECK(strstr(got, "+XDRVI: 5,17,") != NULL);
    {
        /* iOS 6 asserts field 3 (the sensor id) is 0-5 and then reads two values. */
        int f2 = -1, sensor = -1, v1 = -1, v2 = -1;
        CHECK(sscanf(strstr(got, "+XDRVI: 5,17,"), "+XDRVI: 5,17,%d,%d,%d,%d", &f2, &sensor, &v1, &v2) == 4);
        CHECK(sensor >= 0 && sensor <= 5);
    }

    /* No H5 on SPI: after +cmux the mux frames ride the IFX payload directly. */
    ifx_frame(&x, &c, "at+cmux=0,0,0,1500\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "\xf9\x03\x3f\x01\x1c\xf9", miso, 0x7fc, got);   /* SABM DLCI 0, P */
    check_str(got, "\r\nOK\r\n", "ifx cmux OK");
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    if (getenv("BBTEST_TRACE")) {
        fprintf(stderr, "UA? %02x %02x %02x %02x len %u\n", (uint8_t)got[0], (uint8_t)got[1],
                (uint8_t)got[2], (uint8_t)got[3], miso[0] | (miso[1] & 0xf) << 8);
    }
    CHECK(got[0] == (char)0xf9 && got[1] == 0x03 && got[2] == 0x73);   /* UA */

    /* 0xfff length = no payload. */
    {
        uint8_t mosi[16] = { 0xff, 0x0f, 0, 0 };
        const uint8_t *rx;
        size_t rxlen;

        ios_bb_ifx_xfer(&x, mosi, miso, sizeof(mosi), &rx, &rxlen);
        CHECK(rxlen == 0);
    }

    /* v2 (iPhone 4: 0x7fc): credits granted up front, topped up as used. */
    ios_bb_ifx_init(&x, 2, 0x7fc);
    ios_bb_init(&c, ios_bb_ifx_queue, &x);
    CHECK(!ios_bb_ifx_pending(&x));             /* credits ride the AP's first frame */
    ifx_frame(&x, &c, "", miso, 0x800, got);
    CHECK((miso[2] | (miso[3] & 0xf) << 8) == 16);
    for (int i = 0; i < 5; i++) {
        ifx_frame(&x, &c, "at\r", miso, 0x800, got);
    }
    CHECK(x.credits_out == 16);                 /* topped up on every frame */

    /* A modem reset (bb_rst) drops queued data but not the AP's credits: the kext
     * keeps its count and re-grants only the difference. */
    x.credits_in = 3;
    ios_bb_ifx_queue(&x, (const uint8_t *)"stale", 5);
    ios_bb_ifx_modem_reset(&x);
    CHECK(!ios_bb_ifx_pending(&x) && x.credits_in == 3 && x.credits_out == 16);
    x.credits_in = 165;                         /* repeated unanswered "at" pings granting 15 each */
    ios_bb_ifx_modem_reset(&x);
    CHECK(x.credits_in == 16);

    /* +CPWROFF: the OK, then silence (CommCenter's raw "at" pings must go unanswered,
     * or it waits out its ~30 s power-off timeout) until bb_rst resets the modem. */
    ios_bb_reset(&c);
    ifx_frame(&x, &c, "at+cpwroff\r", miso, 0x7fc, got);
    ifx_frame(&x, &c, "", miso, 0x7fc, got);
    check_str(got, "\r\nOK\r\n", "cpwroff OK");
    ifx_frame(&x, &c, "at\r", miso, 0x7fc, got);
    ios_bb_tick(&c, c.now_ms + 10000);
    CHECK(!ios_bb_ifx_pending(&x) && !ios_bb_next_due(&c));
    ios_bb_reset(&c);
    ifx_frame(&x, &c, "at\r", miso, 0x7fc, got);
    CHECK(ios_bb_ifx_pending(&x));              /* answering again */
}

/* ------------------------------------------------------------- packet data */

static uint8_t dpkt[4096];
static size_t dpkt_len;
static int dpkts;

static void data_out(void *opaque, const uint8_t *p, size_t n)
{
    memcpy(dpkt, p, n);
    dpkt_len = n;
    dpkts++;
}

/* 4.x's PDP sequence on a fresh DLCI 6: define, activate, address, DNS, CGDATA, IP both ways. */
static void test_packet_data(void)
{
    uint8_t ip[40] = { 0x45, 0, 0, 40, 0, 0, 0, 0, 64, 17 };

    bb.data_out = data_out;
    c_sabm(6, 0x3f);
    pump();
    ev_i = nev;                                /* skip the UA */
    c_mux_str(5, "at+cgdcont=1,\"IP\",\"test\"\r");
    pump();
    expect_frame(5, "\r\nOK\r\n");
    c_mux_str(5, "at+cgact=1,1\r");
    pump();
    expect_frame(5, "\r\nOK\r\n");
    c_mux_str(5, "at+cgpaddr=1\r");
    pump();
    expect_frame(5, "\r\n+CGPADDR: 1,\"10.0.2.15\"\r\n");
    expect_frame(5, "\r\nOK\r\n");
    c_mux_str(5, "at+xdns?\r");
    pump();
    expect_frame(5, "\r\n+XDNS: 1,\"10.0.2.3\",\"0.0.0.0\"\r\n");
    expect_frame(5, "\r\nOK\r\n");
    c_mux_str(6, "at+cgdata=\"M-RAW_IP\",1\r");
    pump();
    expect_frame(6, "\r\nCONNECT\r\n");

    /* Guest -> network: one packet split over two frames, then two in one. */
    c_mux(6, ip, 25);
    c_mux(6, ip + 25, 15);
    pump();
    CHECK(dpkts == 1 && dpkt_len == 40);
    {
        uint8_t two[80];

        memcpy(two, ip, 40);
        memcpy(two + 40, ip, 40);
        c_mux(6, two, 80);
        pump();
        CHECK(dpkts == 3);
    }
    /* Network -> guest: rides DLCI 6 as UIH. */
    CHECK(ios_bb_data_input(&bb, ip, 40));
    pump();
    CHECK(evs[ev_i].dlci == 6 && evs[ev_i].plen == 40 && memcmp(evs[ev_i].payload, ip, 40) == 0);
    ev_i = nev;

    c_mux_str(5, "at+cgact=0,1\r");
    pump();
    expect_frame(5, "\r\nOK\r\n");
    expect_frame(6, "\r\nNO CARRIER\r\n");
    CHECK(!ios_bb_data_input(&bb, ip, 40));
    bb.data_out = NULL;
}

/*
 * The tests are one ordered chain: each section continues the protocol
 * session the previous one built (init -> registration -> SIM -> calls ->
 * SMS -> teardown), exactly like a booting CommCenter.
 */
static void test_chain(void)
{
    test_h5_crc();
    test_boot();               /* through init, registration and SIM */
    test_incoming_call();
    test_outgoing_call();
    test_emergency_call();
    test_vibrator();
    test_incoming_sms();
    test_outgoing_sms();
    test_incoming_sms_ucs2();
    test_dial_no_service();
    test_signal_change();
    test_packet_data();
    test_sim_removal();
    test_power_and_mux_close();
}

/*
 * 1.0's own kernel (the boot kernelcache's AppleReliableSerialLayer) links up
 * differently from the restore kernel the notes were read from: its CONFIG has
 * no configuration field, it takes its send window from the CONFIG RESP (none =
 * window 0, "Waiting for remote window to open" forever), and CommCenter's
 * +xtransportmode OK has to come over H5 once the link is Active.
 */
static void test_h5_link_1_0(void)
{
    memset(&bb, 0, sizeof(bb));
    snprintf(bb.imei, sizeof(bb.imei), "000000001234569");
    ios_bb_init(&bb, core_out, NULL);
    tnow = 1000;
    outlen = 0;
    nev = ev_i = 0;
    c_tx_seq = c_rx_next = c_acked = 0;
    autoack = true;
    test_mux_on = false;

    feed_raw("at+xtransportmode\r");
    pump();
    expect_none();
    c_link((const uint8_t[]){ 0x01, 0x7e }, 2);
    pump();
    expect_link((const uint8_t[]){ 0x02, 0x7d }, 2);
    c_link((const uint8_t[]){ 0x03, 0xfc }, 2);
    pump();
    expect_link((const uint8_t[]){ 0x04, 0x7b, IOS_BB_H5_WINDOW }, 3);
    expect_raw("\r\nOK\r\n");
    c_data_str("ate0\r");
    pump();
    expect_raw("\r\nOK\r\n");

    /* Its data CRC comes low byte first. */
    {
        uint8_t pkt[4 + 8 + 2] = { 0x80 | 0x40 | (c_rx_next << 3) | c_tx_seq, 14 | (8 << 4), 0 };
        uint8_t wire[2 * sizeof(pkt) + 2], *o = wire;
        uint16_t c;

        pkt[3] = ~(pkt[0] + pkt[1] + pkt[2]);
        memcpy(pkt + 4, "at+cgsn\r", 8);
        c = ios_bb_h5_crc(pkt, 12);
        pkt[12] = c;
        pkt[13] = c >> 8;
        *o++ = 0xc0;
        for (unsigned i = 0; i < sizeof(pkt); i++) {
            if (pkt[i] == 0xc0 || pkt[i] == 0xdb) {
                *o++ = 0xdb;
                *o++ = pkt[i] == 0xc0 ? 0xdc : 0xdd;
            } else {
                *o++ = pkt[i];
            }
        }
        *o++ = 0xc0;
        c_tx_seq = (c_tx_seq + 1) & 7;
        bb.now_ms = tnow;
        ios_bb_input(&bb, wire, o - wire);
        pump();
        expect_raw("\r\n000000001234569\r\n");
        expect_raw("\r\nOK\r\n");
    }
}

/*
 * iBoot-159's radio nvram read: AT+XDRV=9,1,<block>; (its trailing ';') until 0x600
 * bytes; the type-1 entry is the Wi-Fi calibration it copies into the DT, and
 * AppleMRVL868x wants its first 128 bytes neither all 0x00 nor all 0xFF.
 */
static void test_radio_nvram(void)
{
    uint8_t nv[0x600];
    unsigned got = 0;

    memset(&bb, 0, sizeof(bb));
    snprintf(bb.imei, sizeof(bb.imei), "000000001234569");
    ios_bb_init(&bb, core_out, NULL);
    outlen = 0;
    nev = ev_i = 0;
    for (int block = 0; block < 4; block++) {
        char cmd[32], *line, *hex;
        int status, b;

        snprintf(cmd, sizeof(cmd), "at+xdrv=9,1,%d;\r", block);
        outlen = 0;
        bb.now_ms = tnow;
        ios_bb_input(&bb, (const uint8_t *)cmd, strlen(cmd));   /* not pumped: read outbuf */
        outbuf[outlen] = 0;
        line = strstr((char *)outbuf, "+XDRV: 9,1,");
        CHECK(line && strstr((char *)outbuf, "\r\nOK\r\n"));
        if (!line || sscanf(line, "+XDRV: 9,1,%d,%d,", &status, &b) != 2) {
            CHECK(!"no +XDRV reply");
            return;
        }
        CHECK(b == block);
        if (block == 3) {
            CHECK(status != 0);                      /* past the image: iBoot stops */
            break;
        }
        CHECK(status == 0);
        hex = strchr(strchr(strchr(strchr(line, ',') + 1, ',') + 1, ',') + 1, ',') + 1;
        for (unsigned i = 0; i < 512; i++, got++) {
            unsigned v;
            CHECK(sscanf(hex + 2 * i, "%2X", &v) == 1);
            nv[got] = v;
        }
    }
    CHECK(got == 0x600);
    CHECK(nv[0] == 0 && nv[1] == 1 && ((nv[2] << 8) | nv[3]) * 2 - 4 == 1024);
    bool zero = true, ones = true;
    for (int i = 0; i < 128; i++) {
        zero &= nv[4 + i] == 0;
        ones &= nv[4 + i] == 0xff;
    }
    CHECK(!zero && !ones);
    CHECK(((nv[4 + 1024 + 2] << 8) | nv[4 + 1024 + 3]) == 0);   /* the list ends */

    /* and the ';' iBoot puts on "+cgsn;" */
    outlen = 0;
    ios_bb_input(&bb, (const uint8_t *)"at+cgsn;\r", 9);
    outbuf[outlen] = 0;
    CHECK(strstr((char *)outbuf, "000000001234569") != NULL);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/baseband/chain", test_chain);
    g_test_add_func("/baseband/ifx", test_ifx);
    g_test_add_func("/baseband/h5-link-1.0", test_h5_link_1_0);
    g_test_add_func("/baseband/radio-nvram", test_radio_nvram);
    g_test_run();
    if (failures) {
        fprintf(stderr, "test-ios-baseband: %d failure(s)\n", failures);
        return 1;
    }
    printf("test-ios-baseband: all checks passed\n");
    return 0;
}
