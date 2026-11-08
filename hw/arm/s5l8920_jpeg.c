/*
 * The JPEG block of the S5L8920 and A4 ('jpeg,s5l8920x', AppleJPEGDriver, 0x88200000; wired on the A4 boards):
 * Samsung's JPEG codec, whose register map is the one Linux's s5p-jpeg knows for the S5PC110 where it matters
 * (tables at 0x400 and 0x800, software reset at 0x78, clock control at 0x6c, compression mode at 0x68, color
 * coefficients at 0x5c..0x64), with Apple's own placement of the geometry, the addresses and the status words.
 * The camera's still path (JPEGH1.videoencoder) and SpringBoard's snapshots hand it a 4:2:2 'yuvs' surface and take
 * back a complete baseline JFIF stream, headers included, built from the quantization and Huffman tables the
 * driver programmed. The encoder here is that: the tables come from the registers, never from a quality setting,
 * so the stream is the one the block would write. The decoder (jpeg_header, jpeg_decode_start) takes a baseline
 * stream and writes the picture in the layout the driver programmed.
 *
 * Registers as AppleJPEGDriver 2.0.5 (4.2.1) uses them:
 *   0x00 mode: bits 2..0 the output's subsampling (1 4:2:2, 2 4:2:0), bit 3 decode
 *   0x0c handshake (reads back what was written)   0x10 height, 0x14 width (the sensor's landscape picture)
 *   0x18 the compressed size, after the encode     0x1c interrupt acknowledge (the driver only ever writes 0)
 *   0x20 interrupt status: 0x40 encode (or decode) done, 0x08 headers parsed, 0x10 stream corrupt; reading it drops
 *        the interrupt line (the driver's handler reads it and acknowledges nothing), the status stays until a reset
 *   0x50 source, 0x58 destination (dart1 stream 0) 0x68 compression mode, 0x6c clock control
 *   0x70 start (1)                                 0x78 software reset (1, self-clearing)
 *   0x7c timer, 0x80 timer status                  0x98 bits 23..5 the destination's size, bit 24 its check
 *   0x9c stream status: bit 0 the stream overflowed
 *   0x400 + 0x100 n quantization table n (64 words, natural order: the driver's standard table reads 16 11 10)
 *   0x800/0x840 DC luminance bits/values, 0x880/0x8c0 AC luminance, 0xc00/0xc40 DC chrominance, 0xc80/0xcc0 AC
 * IT_JPEG_TRACE=FILE logs each operation and keeps each size's last stream as FILE.WxH.jpg.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"

#define JPEG_SIZE       0x1000
#define R_MODE          0x00
#define R_HANDSHAKE     0x0c
#define R_HEIGHT        0x10
#define R_WIDTH         0x14
#define R_OUT_SIZE      0x18
#define R_INT_ACK       0x1c
#define R_INT_STATUS    0x20
#define R_SRC           0x50
#define R_DST           0x58
#define R_START         0x70
#define R_RESET         0x78
#define R_STREAM_BOUND  0x98
#define R_STREAM_STATUS 0x9c
/* The decoder's, as AppleJPEGHal 3.0.22 (6.1) uses them: a start at 0x70 parses the headers, one at 0x74 decodes. */
#define R_DEC_OUT       0x50            /* the picture's (first plane's) destination */
#define R_DEC_PITCH     0x54
#define R_DEC_STREAM    0x58            /* the compressed stream ... */
#define R_DEC_START     0x74
#define R_DEC_FORMAT    0x88            /* bits 1..0: 3 '2vuy' order, else 'yuvs' */
#define R_DEC_LENGTH    0x94            /* ... and its length */
#define R_DEC_CHROMA    0xa4            /* the Cb/Cr plane of a 4:2:0 picture, 0 for a packed one */
#define R_DEC_CPITCH    0xa8
#define R_DEC_SCALE     0x100           /* the DCT scaling: 1/2^n */
#define R_QTBL(n)       (0x400 + (n) * 0x100)

#define INT_ENC_DONE    0x40
#define INT_CORRUPT     0x10
#define INT_HEADER_DONE 0x08            /* a header pass (0x70 with mode bit 3) */
#define INT_DEC_DONE    0x40            /* a decode (0x74) */

#define TYPE_S5L8920_JPEG "s5l8920.jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8920JPEGState, S5L8920_JPEG)

struct S5L8920JPEGState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[JPEG_SIZE / 4];
    hwaddr (*xlate)(void *opaque, unsigned sid, uint32_t va);
    void *xlate_opaque;
    uint32_t dart_sid;
    QEMUTimer *done;
    uint32_t pending;               /* the status the running operation ends with */
    FILE *trace;
    char *trace_path;
};

void s5l8920_jpeg_set_iommu(DeviceState *dev, hwaddr (*xlate)(void *, unsigned, uint32_t), void *opaque)
{
    S5L8920_JPEG(dev)->xlate = xlate;
    S5L8920_JPEG(dev)->xlate_opaque = opaque;
}

/* ---- a baseline JPEG encoder over given tables ---------------------------------------------------------- */

static const uint8_t zigzag[64] = {        /* zigzag index -> natural (row-major) index */
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

typedef struct {
    uint8_t bits[16], vals[256];
    unsigned nvals;
    uint16_t code[256];
    uint8_t len[256];
} JpegHuff;

typedef struct {
    uint8_t *buf;
    size_t cap, pos;
    uint32_t acc;
    int nacc;
    bool overflow;
} JpegOut;

static void out_byte(JpegOut *o, uint8_t b)
{
    if (o->pos < o->cap) {
        o->buf[o->pos] = b;
    } else {
        o->overflow = true;
    }
    o->pos++;
}

static void out_bits(JpegOut *o, uint32_t v, int n)
{
    o->acc = (o->acc << n) | (v & ((1u << n) - 1));
    o->nacc += n;
    while (o->nacc >= 8) {
        uint8_t b = o->acc >> (o->nacc - 8);

        out_byte(o, b);
        if (b == 0xff) {
            out_byte(o, 0);     /* byte stuffing */
        }
        o->nacc -= 8;
    }
}

static void out_marker(JpegOut *o, uint8_t m, unsigned len)
{
    out_byte(o, 0xff);
    out_byte(o, m);
    if (len) {
        out_byte(o, (len + 2) >> 8);
        out_byte(o, len + 2);
    }
}

/* Annex C: code lengths from BITS, codes in order. */
static void huff_build(JpegHuff *h)
{
    unsigned k = 0, code = 0;

    memset(h->len, 0, sizeof(h->len));
    for (unsigned l = 1; l <= 16; l++) {
        for (unsigned i = 0; i < h->bits[l - 1] && k < h->nvals; i++, k++) {
            /* bounded: vals[] is uint8_t, code[] and len[] have 256 entries */
            h->code[h->vals[k]] = code++;
            h->len[h->vals[k]] = l;     /* bounded: as above */
        }
        code <<= 1;
    }
}

static void huff_put(JpegOut *o, const JpegHuff *h, uint8_t sym)
{
    out_bits(o, h->code[sym], h->len[sym] ? h->len[sym] : 1);
}

static unsigned magnitude(int v)
{
    unsigned n = 0;

    for (v = v < 0 ? -v : v; v; v >>= 1) {
        n++;
    }
    return n;
}

static void encode_block(JpegOut *o, const float in[64], const uint8_t *q, const JpegHuff *dc,
                         const JpegHuff *ac, int *pred)
{
    static float c[8][8];
    static bool init;
    float tmp[64], f[64];
    int zz[64], run = 0;

    if (!init) {
        for (int u = 0; u < 8; u++) {
            for (int x = 0; x < 8; x++) {
                c[u][x] = (u ? 0.5f : 0.5f / sqrtf(2.0f)) * cosf((2 * x + 1) * u * (float)M_PI / 16);
            }
        }
        init = true;
    }
    for (int y = 0; y < 8; y++) {           /* rows, then columns */
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++) {
                s += c[u][x] * in[y * 8 + x];
            }
            tmp[y * 8 + u] = s;
        }
    }
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            float s = 0;
            for (int y = 0; y < 8; y++) {
                s += c[v][y] * tmp[y * 8 + u];
            }
            f[v * 8 + u] = s;
        }
    }
    for (int k = 0; k < 64; k++) {
        float qv = f[zigzag[k]] / MAX(q[zigzag[k]], 1);
        zz[k] = (int)lrintf(qv);
    }
    int diff = zz[0] - *pred;
    unsigned n = magnitude(diff);
    *pred = zz[0];
    huff_put(o, dc, n);
    if (n) {
        out_bits(o, diff < 0 ? diff - 1 : diff, n);
    }
    for (int k = 1; k < 64; k++) {
        if (!zz[k]) {
            run++;
            continue;
        }
        while (run > 15) {
            huff_put(o, ac, 0xf0);
            run -= 16;
        }
        n = magnitude(zz[k]);
        huff_put(o, ac, run << 4 | n);
        out_bits(o, zz[k] < 0 ? zz[k] - 1 : zz[k], n);
        run = 0;
    }
    if (run) {
        huff_put(o, ac, 0x00);      /* EOB */
    }
}

static void put_dht(JpegOut *o, const JpegHuff *h, uint8_t cls_id)
{
    out_byte(o, cls_id);
    for (int i = 0; i < 16; i++) {
        out_byte(o, h->bits[i]);
    }
    for (unsigned i = 0; i < h->nvals; i++) {
        out_byte(o, h->vals[i]);
    }
}

/*
 * A 'yuvs' surface (4:2:2, Y0 Cb Y1 Cr) to a baseline stream of three components, table 0 for luma and 1 for
 * chroma: 4:2:2 (luma 2x1, MCUs of 16x8) or, with `v2`, 4:2:0 (luma 2x2, MCUs of 16x16, each chroma sample the
 * mean of its two rows).
 */
static size_t jpeg_encode(const uint8_t *src, unsigned w, unsigned h, unsigned stride, bool v2,
                          const uint8_t qt[2][64], JpegHuff huff[4], uint8_t *dst, size_t cap, bool *overflow)
{
    unsigned mh = v2 ? 16 : 8;
    JpegOut o = { .buf = dst, .cap = cap };
    int pred[3] = { 0 };

    for (int i = 0; i < 4; i++) {
        huff_build(&huff[i]);
    }
    out_marker(&o, 0xd8, 0);                                    /* SOI */
    for (int t = 0; t < 2; t++) {                               /* DQT */
        out_marker(&o, 0xdb, 65);
        out_byte(&o, t);
        for (int k = 0; k < 64; k++) {
            out_byte(&o, qt[t][zigzag[k]]);     /* DQT is in zigzag order */
        }
    }
    out_marker(&o, 0xc0, 15);                                   /* SOF0 */
    out_byte(&o, 8);
    out_byte(&o, h >> 8);
    out_byte(&o, h);
    out_byte(&o, w >> 8);
    out_byte(&o, w);
    out_byte(&o, 3);
    out_byte(&o, 1); out_byte(&o, v2 ? 0x22 : 0x21); out_byte(&o, 0);
    out_byte(&o, 2); out_byte(&o, 0x11); out_byte(&o, 1);
    out_byte(&o, 3); out_byte(&o, 0x11); out_byte(&o, 1);
    for (int i = 0; i < 4; i++) {                               /* DHT: DC0, AC0, DC1, AC1 */
        static const uint8_t ids[4] = { 0x00, 0x10, 0x01, 0x11 };
        static const int which[4] = { 0, 2, 1, 3 };
        out_marker(&o, 0xc4, 17 + huff[which[i]].nvals);
        put_dht(&o, &huff[which[i]], ids[i]);
    }
    out_marker(&o, 0xda, 10);                                   /* SOS */
    out_byte(&o, 3);
    out_byte(&o, 1); out_byte(&o, 0x00);
    out_byte(&o, 2); out_byte(&o, 0x11);
    out_byte(&o, 3); out_byte(&o, 0x11);
    out_byte(&o, 0); out_byte(&o, 63); out_byte(&o, 0);

    for (unsigned my = 0; my < h; my += mh) {
        for (unsigned mx = 0; mx < w; mx += 16) {
            float blk[6][64];       /* luma 8x8s in raster order, then Cb, Cr; edges repeat the last pixel */
            unsigned ny = v2 ? 4 : 2;

            for (unsigned y = 0; y < mh; y++) {
                unsigned py = MIN(my + y, h - 1);
                const uint8_t *row = src + (size_t)py * stride;
                for (unsigned x = 0; x < 16; x++) {
                    unsigned px = MIN(mx + x, w - 1), b = (y / 8) * 2 + x / 8;

                    blk[b][(y % 8) * 8 + x % 8] = row[px * 2] - 128.0f;
                    if (!(x & 1) && (!v2 || !(y & 1))) {
                        const uint8_t *c = row + (px & ~1u) * 2 + 1;
                        const uint8_t *c2 = v2 ? src + (size_t)MIN(py + 1, h - 1) * stride + (px & ~1u) * 2 + 1 : c;
                        unsigned cy = v2 ? y / 2 : y;

                        blk[ny][cy * 8 + x / 2] = (c[0] + c2[0]) / 2.0f - 128.0f;
                        blk[ny + 1][cy * 8 + x / 2] = (c[2] + c2[2]) / 2.0f - 128.0f;
                    }
                }
            }
            for (unsigned b = 0; b < ny; b++) {
                encode_block(&o, blk[b], qt[0], &huff[0], &huff[2], &pred[0]);
            }
            encode_block(&o, blk[ny], qt[1], &huff[1], &huff[3], &pred[1]);
            encode_block(&o, blk[ny + 1], qt[1], &huff[1], &huff[3], &pred[2]);
        }
    }
    if (o.nacc) {
        out_bits(&o, 0x7f, 8 - o.nacc);      /* pad with ones */
    }
    out_marker(&o, 0xd9, 0);                                    /* EOI */
    *overflow = o.overflow;
    return o.pos;
}

/* ---- a baseline JPEG decoder ----------------------------------------------------------------------------- */

typedef struct {
    uint8_t vals[256];
    int mincode[17], maxcode[18], valptr[17];
    bool set;
} JpegDHuff;

typedef struct {
    unsigned w, h, ncomp, hmax, vmax, restart, mcux, mcuy;
    struct { uint8_t id, h, v, tq, td, ta; } comp[3];
    uint16_t qt[4][64];
    JpegDHuff dc[4], ac[4];
    const uint8_t *scan;
    size_t scanlen;
} JpegFrame;

/* Annex F.2.2.3's tables from BITS and HUFFVAL. */
static void dhuff_build(JpegDHuff *t, const uint8_t bits[16], const uint8_t *vals, unsigned n)
{
    int code = 0, k = 0;

    memcpy(t->vals, vals, n);
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        code += bits[l - 1];
        k += bits[l - 1];
        t->maxcode[l] = bits[l - 1] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = INT_MAX;
    t->set = true;
}

/* The headers up to the first scan: the frame (baseline or extended Huffman, 8-bit, 1 or 3 components), its tables
 * and where the entropy-coded data starts. False for anything the block can't decode (progressive, arithmetic,
 * 12-bit), which it reports as a corrupt stream. */
static bool jpeg_parse(const uint8_t *p, size_t n, JpegFrame *f)
{
    size_t i = 2;
    bool frame = false;

    memset(f, 0, sizeof(*f));
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8) {
        return false;
    }
    while (i + 4 <= n) {
        if (p[i] != 0xff) {
            return false;
        }
        uint8_t m = p[i + 1];
        if (m == 0xff) {
            i++;
            continue;
        }
        size_t len = (p[i + 2] << 8) | p[i + 3], s = i + 4, e = i + 2 + len;
        if (len < 2 || e > n) {
            return false;
        }
        switch (m) {
        case 0xdb:                                          /* DQT */
            while (s < e) {
                unsigned pq = p[s] >> 4, tq = p[s] & 3;
                if (s + 1 + 64 * (pq + 1) > e) {
                    return false;
                }
                for (int k = 0; k < 64; k++) {
                    f->qt[tq][zigzag[k]] = pq ? (p[s + 1 + 2 * k] << 8) | p[s + 2 + 2 * k] : p[s + 1 + k];
                }
                s += 1 + 64 * (pq + 1);
            }
            break;
        case 0xc4:                                          /* DHT */
            while (s + 17 <= e) {
                unsigned tc = p[s] >> 4, th = p[s] & 3, total = 0;
                for (int l = 0; l < 16; l++) {
                    total += p[s + 1 + l];
                }
                if (total > 256 || s + 17 + total > e || tc > 1) {
                    return false;
                }
                dhuff_build(tc ? &f->ac[th] : &f->dc[th], p + s + 1, p + s + 17, total);
                s += 17 + total;
            }
            break;
        case 0xc0: case 0xc1:                               /* SOF0, SOF1 */
            if (len < 8 || p[s] != 8) {
                return false;
            }
            f->h = (p[s + 1] << 8) | p[s + 2];
            f->w = (p[s + 3] << 8) | p[s + 4];
            f->ncomp = p[s + 5];
            if ((f->ncomp != 1 && f->ncomp != 3) || len < 8 + 3 * f->ncomp || !f->w || !f->h ||
                f->w > 8192 || f->h > 8192) {          /* the encoder's bound: 192 MiB of planes at most */
                return false;
            }
            for (unsigned c = 0; c < f->ncomp; c++) {
                f->comp[c].id = p[s + 6 + 3 * c];
                f->comp[c].h = p[s + 7 + 3 * c] >> 4;
                f->comp[c].v = p[s + 7 + 3 * c] & 15;
                f->comp[c].tq = p[s + 8 + 3 * c] & 3;
                if (f->comp[c].h < 1 || f->comp[c].h > 2 || f->comp[c].v < 1 || f->comp[c].v > 2) {
                    return false;
                }
                f->hmax = MAX(f->hmax, f->comp[c].h);
                f->vmax = MAX(f->vmax, f->comp[c].v);
            }
            f->mcux = (f->w + 8 * f->hmax - 1) / (8 * f->hmax);
            f->mcuy = (f->h + 8 * f->vmax - 1) / (8 * f->vmax);
            frame = true;
            break;
        case 0xdd:                                          /* DRI */
            f->restart = len >= 4 ? (p[s] << 8) | p[s + 1] : 0;
            break;
        case 0xda:                                          /* SOS: one interleaved scan of every component */
            if (!frame || p[s] != f->ncomp) {
                return false;
            }
            for (unsigned c = 0; c < f->ncomp; c++) {
                unsigned id = p[s + 1 + 2 * c], k;
                for (k = 0; k < f->ncomp && f->comp[k].id != id; k++) {
                }
                if (k == f->ncomp) {
                    return false;
                }
                f->comp[k].td = p[s + 2 + 2 * c] >> 4 & 3;
                f->comp[k].ta = p[s + 2 + 2 * c] & 3;
                if (!f->dc[f->comp[k].td].set || !f->ac[f->comp[k].ta].set) {
                    return false;
                }
            }
            f->scan = p + e;
            f->scanlen = n - e;
            return true;
        default:
            if (m >= 0xc2 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
                return false;                               /* progressive, lossless, arithmetic */
            }
            break;
        }
        i = e;
    }
    return false;
}

typedef struct {
    const uint8_t *p;
    size_t n, pos;
    uint32_t acc;
    int nacc;
} JpegIn;

static int in_bit(JpegIn *b)
{
    if (!b->nacc) {
        uint8_t v = 0;
        if (b->pos < b->n) {
            v = b->p[b->pos];
            if (v == 0xff) {
                if (b->pos + 1 < b->n && b->p[b->pos + 1] == 0) {
                    b->pos += 2;                            /* a stuffed 0xff */
                } else {
                    v = 0;                                  /* a marker: zeros until the restart */
                }
            } else {
                b->pos++;
            }
        }
        b->acc = v;
        b->nacc = 8;
    }
    return (b->acc >> --b->nacc) & 1;
}

static int in_bits(JpegIn *b, int n)
{
    int v = 0;

    while (n--) {
        v = (v << 1) | in_bit(b);
    }
    return v;
}

static int in_huff(JpegIn *b, const JpegDHuff *t)
{
    int code = in_bit(b), l = 1;

    while (code > t->maxcode[l]) {
        if (++l > 16) {
            return -1;
        }
        code = (code << 1) | in_bit(b);
    }
    return t->vals[(t->valptr[l] + code - t->mincode[l]) & 0xff];
}

static int extend(int v, int t)
{
    return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v;
}

static bool decode_block(JpegIn *b, const JpegFrame *f, unsigned c, int *pred, uint8_t *out, unsigned stride)
{
    static float cs[8][8];
    static bool init;
    float coef[64] = { 0 }, tmp[64];
    int t = in_huff(b, &f->dc[f->comp[c].td]);
    const uint16_t *q = f->qt[f->comp[c].tq];

    if (!init) {
        for (int x = 0; x < 8; x++) {
            for (int u = 0; u < 8; u++) {
                cs[x][u] = (u ? 0.5f : 0.5f / sqrtf(2.0f)) * cosf((2 * x + 1) * u * (float)M_PI / 16);
            }
        }
        init = true;
    }
    if (t < 0 || t > 11) {
        return false;
    }
    *pred += extend(in_bits(b, t), t);
    coef[0] = *pred * q[0];
    for (int k = 1; k < 64;) {
        int rs = in_huff(b, &f->ac[f->comp[c].ta]);
        if (rs < 0) {
            return false;
        }
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15) {
                break;                                      /* EOB */
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) {
            return false;
        }
        coef[zigzag[k]] = extend(in_bits(b, s), s) * q[zigzag[k]];
        k++;
    }
    for (int y = 0; y < 8; y++) {                           /* rows, then columns */
        for (int x = 0; x < 8; x++) {
            float sum = 0;
            for (int u = 0; u < 8; u++) {
                sum += cs[x][u] * coef[y * 8 + u];
            }
            tmp[y * 8 + x] = sum;
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            float sum = 0;
            for (int v = 0; v < 8; v++) {
                sum += cs[y][v] * tmp[v * 8 + x];
            }
            int px = lrintf(sum + 128.0f);
            out[y * stride + x] = px < 0 ? 0 : px > 255 ? 255 : px;
        }
    }
    return true;
}

/* Every component's samples, each plane padded to whole MCUs (component c: mcux * h * 8 wide). */
static bool jpeg_decode(const JpegFrame *f, uint8_t *planes[3])
{
    JpegIn b = { .p = f->scan, .n = f->scanlen };
    int pred[3] = { 0 };
    unsigned todo = f->restart;

    for (unsigned my = 0; my < f->mcuy; my++) {
        for (unsigned mx = 0; mx < f->mcux; mx++) {
            if (f->restart && !todo--) {                    /* RSTn: realign, skip the marker, reset predictions */
                todo = f->restart - 1;
                b.nacc = 0;
                while (b.pos + 1 < b.n && !(b.p[b.pos] == 0xff && b.p[b.pos + 1] >= 0xd0 && b.p[b.pos + 1] <= 0xd7)) {
                    b.pos++;
                }
                b.pos += 2;
                memset(pred, 0, sizeof(pred));
            }
            for (unsigned c = 0; c < f->ncomp; c++) {
                unsigned H = f->comp[c].h, V = f->comp[c].v, stride = f->mcux * H * 8;
                for (unsigned v = 0; v < V; v++) {
                    for (unsigned h = 0; h < H; h++) {
                        uint8_t *o = planes[c] + ((size_t)(my * V + v) * 8) * stride + (mx * H + h) * 8;
                        if (!decode_block(&b, f, c, &pred[c], o, stride)) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

/* ---- the block --------------------------------------------------------------------------------------------- */

static hwaddr jpeg_pa(S5L8920JPEGState *s, uint32_t va)
{
    return s->xlate ? s->xlate(s->xlate_opaque, s->dart_sid, va) : va;
}

static bool jpeg_dma(S5L8920JPEGState *s, uint32_t va, uint8_t *buf, size_t len, bool write)
{
    while (len) {
        size_t n = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa = jpeg_pa(s, va);

        if (pa == (hwaddr)-1) {
            if (s->trace) {
                fprintf(s->trace, "DMA failed sid %u va %08x remaining %zu write %u\n",
                        s->dart_sid, va, len, write);
            }
            return false;
        }
        if (address_space_rw(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                             buf, n, write) != MEMTX_OK) {
            return false;
        }
        va += n;
        buf += n;
        len -= n;
    }
    return true;
}

/* Nothing is left to report. Never raises: a status the driver has read must not interrupt it again. */
static void jpeg_update_irq(S5L8920JPEGState *s)
{
    if (!s->regs[R_INT_STATUS / 4]) {
        qemu_irq_lower(s->irq);
    }
}

/* The compressed stream the driver named, parsed: false (and nothing allocated) when it can't be read or decoded. */
static bool jpeg_stream(S5L8920JPEGState *s, uint8_t **buf, JpegFrame *f)
{
    size_t len = s->regs[R_DEC_LENGTH / 4];

    *buf = NULL;
    if (!len || len > 16 * MiB) {
        return false;
    }
    *buf = g_malloc(len);
    if (!jpeg_dma(s, s->regs[R_DEC_STREAM / 4], *buf, len, false) || !jpeg_parse(*buf, len, f)) {
        g_free(*buf);
        *buf = NULL;
        return false;
    }
    return true;
}

static void jpeg_finish(S5L8920JPEGState *s, uint32_t status)
{
    s->pending = status;
    /* ponytail: the block's time isn't modeled beyond a millisecond; measure one on a device if it matters. */
    timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
}

/* The header pass: the picture's size and its subsampling (mode bits 2..0, as the encoder's: 0 4:4:4, 1 4:2:2,
 * 2 4:2:0, 3 grayscale), for the driver to size the output and the scaling from. */
static void jpeg_header(S5L8920JPEGState *s)
{
    uint32_t *r = s->regs;
    g_autofree uint8_t *buf = NULL;
    JpegFrame f;
    int code = -1;

    if (jpeg_stream(s, &buf, &f)) {
        if (f.ncomp == 1) {
            code = 3;
        } else if (f.comp[1].h == 1 && f.comp[1].v == 1 && f.comp[2].h == 1 && f.comp[2].v == 1) {
            code = f.comp[0].h == 1 && f.comp[0].v == 1 ? 0 : f.comp[0].h == 2 && f.comp[0].v == 1 ? 1
                 : f.comp[0].h == 2 && f.comp[0].v == 2 ? 2 : -1;
        }
    }
    if (s->trace) {
        fprintf(s->trace, "header stream %08x len %u -> %ux%u subsampling %d\n", r[R_DEC_STREAM / 4],
                r[R_DEC_LENGTH / 4], code < 0 ? 0 : f.w, code < 0 ? 0 : f.h, code);
    }
    if (code < 0) {
        jpeg_finish(s, INT_CORRUPT);
        return;
    }
    r[R_MODE / 4] = (r[R_MODE / 4] & ~7u) | code;
    r[R_HEIGHT / 4] = f.h;
    r[R_WIDTH / 4] = f.w;
    jpeg_finish(s, INT_HEADER_DONE);
}

/* The decode: the picture, scaled by 1/2^n, as packed 4:2:2 ('yuvs' or '2vuy') or as a 4:2:0 luma plane and an
 * interleaved Cb/Cr plane, at the addresses and pitches the driver programmed. */
static void jpeg_decode_start(S5L8920JPEGState *s)
{
    uint32_t *r = s->regs;
    g_autofree uint8_t *buf = NULL;
    uint8_t *planes[3] = { NULL };
    JpegFrame f;
    bool ok = false;
    unsigned scale = MIN(r[R_DEC_SCALE / 4], 3u), ow = 0, oh = 0;

    if (jpeg_stream(s, &buf, &f)) {
        for (unsigned c = 0; c < f.ncomp; c++) {
            planes[c] = g_malloc0((size_t)f.mcux * f.comp[c].h * 8 * f.mcuy * f.comp[c].v * 8);
        }
        ok = jpeg_decode(&f, planes);
    }
    if (ok) {
        /* The block this driver finds (version 0: none of its output-format controls) writes what Celestial asks
         * it for (fjds_createPixelBufferFromJPEGData): a 4:2:2 picture as packed 'yuvf', anything else as '420f',
         * its luma rows then its Cb/Cr rows, each as wide as the picture's whole MCUs; the driver names only the
         * surface. A driver that programs the planes (R_DEC_CHROMA) gets them where it says. */
        bool packed = !r[R_DEC_CHROMA / 4] && f.ncomp == 3 && f.comp[0].h == 2 && f.comp[0].v == 1;
        bool planar = !packed, cfirst = (r[R_DEC_FORMAT / 4] & 3) == 3;
        unsigned pitch = r[R_DEC_PITCH / 4], cpitch = r[R_DEC_CPITCH / 4];
        ow = (f.mcux * 8 * f.hmax) >> scale;
        oh = (f.mcuy * 8 * f.vmax) >> scale;
        if (!pitch) {
            pitch = planar ? ow : ow * 2;
        }
        if (!cpitch) {
            cpitch = pitch;
        }
        uint32_t chroma = r[R_DEC_CHROMA / 4] ? r[R_DEC_CHROMA / 4] : r[R_DEC_OUT / 4] + pitch * oh;
        g_autofree uint8_t *row = g_malloc(MAX(ow * 2, 2u) + 2);
        /* A sample of component c at the picture's (x, y), from its plane. */
#define SAMPLE(c, x, y) (f.ncomp == 1 && (c) ? 128 : \
        planes[c][(size_t)(((y) * f.comp[c].v) / f.vmax) * (f.mcux * f.comp[c].h * 8) + ((x) * f.comp[c].h) / f.hmax])
        for (unsigned y = 0; y < oh && ok; y++) {
            unsigned sy = y << scale;
            for (unsigned x = 0; x < ow; x += 2) {
                unsigned sx = x << scale, sx1 = MIN(x + 1, ow - 1) << scale;
                uint8_t y0 = SAMPLE(0, sx, sy), y1 = SAMPLE(0, sx1, sy), cb = SAMPLE(1, sx, sy), cr = SAMPLE(2, sx, sy);
                uint8_t *p = row + x * (planar ? 1 : 2);
                if (planar) {
                    p[0] = y0;
                    p[1] = y1;
                } else if (cfirst) {
                    p[0] = cb; p[1] = y0; p[2] = cr; p[3] = y1;
                } else {
                    p[0] = y0; p[1] = cb; p[2] = y1; p[3] = cr;
                }
            }
            ok = jpeg_dma(s, r[R_DEC_OUT / 4] + y * pitch, row, planar ? ow : ow * 2, true);
            if (ok && planar && !(y & 1)) {
                for (unsigned x = 0; x < ow; x += 2) {
                    row[x] = SAMPLE(1, x << scale, sy);
                    row[x + 1] = SAMPLE(2, x << scale, sy);
                }
                ok = jpeg_dma(s, chroma + (y / 2) * cpitch, row, ow & ~1u, true);
            }
        }
#undef SAMPLE
    }
    if (s->trace) {
        fprintf(s->trace, "decode %ux%u scale %u -> %ux%u%s", ok ? f.w : 0, ok ? f.h : 0, scale, ow, oh,
                ok ? "" : " FAILED");
        for (unsigned i = 0; i < 0x104 / 4; i++) {
            if (r[i]) {
                fprintf(s->trace, "  %02x=%08x", i * 4, r[i]);
            }
        }
        fputc('\n', s->trace);
    }
    for (unsigned c = 0; c < 3; c++) {
        g_free(planes[c]);
    }
    jpeg_finish(s, ok ? INT_DEC_DONE : INT_CORRUPT);
}

static void jpeg_start(S5L8920JPEGState *s)
{
    uint32_t *r = s->regs;
    unsigned mode = r[R_MODE / 4] & 0xf, w = r[R_WIDTH / 4], h = r[R_HEIGHT / 4];
    bool v2 = mode == 2;
    unsigned stride = w * 2;            /* ponytail: packed rows assumed; find the stride register if a surface pads */
    size_t in = (size_t)stride * h;
    size_t cap = r[R_STREAM_BOUND / 4] & 0xffffe0, len = 0;
    uint8_t qt[2][64];
    JpegHuff huff[4] = { 0 };
    static const unsigned hbase[4] = { 0x800, 0xc00, 0x880, 0xc80 };   /* DC0, DC1, AC0, AC1 */
    bool overflow = false, ok = false;

    for (int t = 0; t < 2; t++) {
        for (int k = 0; k < 64; k++) {
            qt[t][k] = r[(R_QTBL(t) + k * 4) / 4];
        }
    }
    for (int i = 0; i < 4; i++) {
        huff[i].nvals = 0;
        for (int l = 0; l < 16; l++) {
            huff[i].bits[l] = r[(hbase[i] + l * 4) / 4];
            huff[i].nvals += huff[i].bits[l];
        }
        huff[i].nvals = MIN(huff[i].nvals, i < 2 ? 12u : 162u);
        for (unsigned v = 0; v < huff[i].nvals; v++) {
            huff[i].vals[v] = r[(hbase[i] + 0x40 + v * 4) / 4];
        }
    }
    if (r[R_MODE / 4] & 8) {
        jpeg_header(s);
        return;
    }
    /* Packed 4:2:2 pairs pixels: an odd width would read the last pair's chroma past the row. */
    if ((mode == 1 || mode == 2) && w && !(w & 1) && h && w <= 8192 && h <= 8192 && cap) {
        g_autofree uint8_t *src = g_malloc(in);
        g_autofree uint8_t *dst = g_malloc(cap);

        if (jpeg_dma(s, r[R_SRC / 4], src, in, false)) {
            len = jpeg_encode(src, w, h, stride, v2, qt, huff, dst, cap, &overflow);
            ok = jpeg_dma(s, r[R_DST / 4], dst, MIN(len, cap), true);
            if (s->trace_path && !overflow) {
                g_autofree char *path = g_strdup_printf("%s.%ux%u.jpg", s->trace_path, w, h);
                g_file_set_contents(path, (const char *)dst, len, NULL);
            }
        }
    }
    if (s->trace) {
        fprintf(s->trace, "encode mode %x %ux%u src %08x dst %08x cap %zu -> %zu%s%s q0 %u %u %u\n", r[R_MODE / 4],
                w, h, r[R_SRC / 4], r[R_DST / 4], cap, len, overflow ? " overflow" : "", ok ? "" : " FAILED",
                qt[0][0], qt[0][1], qt[0][2]);
        for (unsigned i = 0; i < 0x100 / 4; i++) {
            if (r[i]) {
                fprintf(s->trace, "  %02x=%08x", i * 4, r[i]);
            }
        }
        fputc('\n', s->trace);
    }
    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR, "jpeg: unsupported or invalid encode, mode 0x%x %ux%u\n", r[R_MODE / 4], w, h);
    }
    r[R_OUT_SIZE / 4] = len;
    r[R_STREAM_STATUS / 4] = overflow;
    s->pending = INT_ENC_DONE;
    /* ponytail: the block's encode time isn't modeled beyond a millisecond; measure one on a device if it matters. */
    timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
}

static void jpeg_complete(void *opaque)
{
    S5L8920JPEGState *s = opaque;

    s->regs[R_INT_STATUS / 4] |= s->pending;
    qemu_irq_raise(s->irq);
}

static uint64_t jpeg_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8920JPEGState *s = opaque;

    if (addr == R_INT_STATUS) {
        qemu_irq_lower(s->irq);
    }
    return s->regs[addr / 4];
}

static void jpeg_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8920JPEGState *s = opaque;

    switch (addr) {
    case R_RESET:
        if (val & 1) {
            timer_del(s->done);
            memset(s->regs, 0, 0x400);      /* the tables survive a reset */
            jpeg_update_irq(s);
        }
        return;
    case R_INT_ACK:
        s->regs[R_INT_STATUS / 4] &= ~val;
        s->regs[addr / 4] = val;
        jpeg_update_irq(s);
        return;
    case R_START:
        s->regs[addr / 4] = val;
        if (val & 1) {
            jpeg_start(s);
        }
        return;
    case R_DEC_START:
        s->regs[addr / 4] = val;
        if (val & 1) {
            jpeg_decode_start(s);
        }
        return;
    }
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps jpeg_ops = {
    .read = jpeg_read,
    .write = jpeg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void jpeg_init(Object *obj)
{
    S5L8920JPEGState *s = S5L8920_JPEG(obj);
    const char *trace = getenv("IT_JPEG_TRACE");

    memory_region_init_io(&s->iomem, obj, &jpeg_ops, s, TYPE_S5L8920_JPEG, JPEG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, jpeg_complete, s);
    if (trace && *trace) {
        s->trace = fopen(trace, "a");
        s->trace_path = g_strdup(trace);
        if (s->trace) {
            setvbuf(s->trace, NULL, _IOLBF, 0);
        }
    }
}

static void jpeg_reset(DeviceState *dev)
{
    S5L8920JPEGState *s = S5L8920_JPEG(dev);

    timer_del(s->done);
    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription jpeg_vmstate = {
    .name = TYPE_S5L8920_JPEG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8920JPEGState, JPEG_SIZE / 4),
        VMSTATE_TIMER_PTR(done, S5L8920JPEGState),
        VMSTATE_UINT32(pending, S5L8920JPEGState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property jpeg_properties[] = {
    DEFINE_PROP_UINT32("dart-sid", S5L8920JPEGState, dart_sid, 0),
};

static void jpeg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, jpeg_reset);
    device_class_set_props(dc, jpeg_properties);
    dc->vmsd = &jpeg_vmstate;
    dc->desc = "S5L8920/A4 JPEG codec";
}

static const TypeInfo jpeg_info = {
    .name = TYPE_S5L8920_JPEG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8920JPEGState),
    .instance_init = jpeg_init,
    .class_init = jpeg_class_init,
};

static void jpeg_register_types(void)
{
    type_register_static(&jpeg_info);
}

type_init(jpeg_register_types)
