/* VXD H.264 on the host: every picture of three streams, given as the guest gives them (a slice NAL, the register
 * fields AppleVXD375Framework programs and where the shift register starts) with no SPS or PPS, must come out
 * byte-identical to the stream's own decode.
 *
 * Links the patched FFmpeg (scripts/configure-patched-ffmpeg writes build/ffmpeg-pkgconfig): stock FFmpeg ends a
 * picture at every slice in chunk mode.
 *
 * SLICE hw/arm/s5l8920_vxd_h264.h range typedef struct VXDH264 | #endif
 * SLICE hw/arm/s5l8920_vxd_h264.c range #include <libavcodec/avcodec.h> | #else
 * PKG glib-2.0 $ROOT/build/ffmpeg-pkgconfig/libavcodec.pc $ROOT/build/ffmpeg-pkgconfig/libavutil.pc
 * CFLAGS -Xlinker -rpath -Xlinker
 * PKG --variable=libdir $ROOT/build/ffmpeg-pkgconfig/libavcodec.pc
 */
#define IT_HAVE_AVCODEC 1
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline int32_t sextract32(uint32_t v, int start, int length)
{
    return ((int32_t)(v << (32 - length - start))) >> (32 - length);
}
#include "slice.h"
#include "s5l8920-vxd-h264-streams.h"

/* The stream's own parameters, parsed independently of the unit under test. */
typedef struct {
    unsigned l2fn, poct, l2poc, w, h, fmo, d8x8, delta0;
    unsigned cabac, bfpo, l0, l1, wp, wb, dbf, red, cip, t8x8;
    int qp, cqp;
} Truth;

typedef struct { const uint8_t *p; size_t bits, pos; } R;
static unsigned u(R *r, unsigned n) { unsigned v = 0; while (n--) { v = v << 1 | ((r->p[r->pos / 8] >> (7 - r->pos % 8)) & 1); r->pos++; } return v; }
static unsigned ue(R *r) { unsigned z = 0; while (!u(r, 1)) z++; return (1u << z) - 1 + u(r, z); }
static int se(R *r) { unsigned v = ue(r); return v & 1 ? (int)(v + 1) / 2 : -(int)(v / 2); }

/* RBSP of a NAL payload, and raw[i] = the raw byte index of RBSP byte i. */
static size_t unescape(const uint8_t *n, size_t len, uint8_t *out, size_t *raw)
{
    size_t k = 0; unsigned z = 0;
    for (size_t i = 0; i < len; i++) {
        if (z == 2 && n[i] == 3) { z = 0; continue; }
        raw[k] = i; out[k++] = n[i]; z = n[i] ? 0 : z + 1;
    }
    return k;
}

static void run(const char *name, const uint8_t *data, size_t size)
{
    /* The reference: the whole stream, its own SPS and PPS included, NAL by NAL in chunk mode as the unit feeds it. */
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ref = avcodec_alloc_context3(codec);
    ref->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ref->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    ref->apply_cropping = 0;      /* the guest's buffers hold whole macroblocks */
    assert(avcodec_open2(ref, codec, NULL) == 0);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    GPtrArray *frames = g_ptr_array_new_with_free_func(g_free);
    for (size_t at = 0; at < size;) {
        size_t e = at + 3;
        while (e + 3 <= size && !(data[e] == 0 && data[e + 1] == 0 && (data[e + 2] == 1 || (data[e + 2] == 0 && e + 3 < size && data[e + 3] == 1)))) e++;
        if (e + 3 > size) e = size;
        av_new_packet(pkt, e - at); memcpy(pkt->data, data + at, e - at); avcodec_send_packet(ref, pkt); av_packet_unref(pkt);
        at = e;
        while (avcodec_receive_frame(ref, f) == 0) {
            size_t ys = (size_t)f->width * f->height;
            uint8_t *copy = g_malloc(ys * 3 / 2);
            for (int y = 0; y < f->height; y++) memcpy(copy + y * f->width, f->data[0] + y * f->linesize[0], f->width);
            for (int c = 1; c < 3; c++)
                for (int y = 0; y < f->height / 2; y++)
                    memcpy(copy + ys + (c - 1) * ys / 4 + y * f->width / 2, f->data[c] + y * f->linesize[c], f->width / 2);
            g_ptr_array_add(frames, copy);
        }
    }

    /* The guest's view: slices only. */
    VXDH264 *h = vxd_h264_new();
    Truth t = {0};
    unsigned pictures = 0, slices = 0;
    size_t i = 0;
    while (i + 3 < size) {
        size_t s = i + (data[i + 2] == 1 ? 3 : 4), e = s;
        while (e + 3 <= size && !(data[e] == 0 && data[e + 1] == 0 && (data[e + 2] == 1 || (data[e + 2] == 0 && e + 3 < size && data[e + 3] == 1)))) e++;
        if (e + 3 > size) e = size;
        const uint8_t *nal = data + s; size_t len = e - s;
        g_autofree uint8_t *rb = g_malloc(len); g_autofree size_t *raw = g_new(size_t, len);
        size_t rl = unescape(nal + 1, len - 1, rb, raw);
        R r = { rb, rl * 8, 0 };
        unsigned type = nal[0] & 31;
        if (type == 7) {
            unsigned prof = u(&r, 8); u(&r, 16); ue(&r);
            if (prof >= 100) { assert(ue(&r) == 1); ue(&r); ue(&r); u(&r, 1); assert(!u(&r, 1)); }
            t.l2fn = ue(&r) + 4; t.poct = ue(&r);
            if (t.poct == 0) t.l2poc = ue(&r) + 4;
            assert(t.poct != 1);
            ue(&r); u(&r, 1); t.w = ue(&r) + 1; t.h = ue(&r) + 1; t.fmo = u(&r, 1); assert(t.fmo); t.d8x8 = u(&r, 1);
        } else if (type == 8) {
            ue(&r); ue(&r); t.cabac = u(&r, 1); t.bfpo = u(&r, 1); assert(!ue(&r));
            t.l0 = ue(&r) + 1; t.l1 = ue(&r) + 1; t.wp = u(&r, 1); t.wb = u(&r, 2);
            t.qp = 26 + se(&r); se(&r); t.cqp = se(&r); t.dbf = u(&r, 1); t.cip = u(&r, 1); t.red = u(&r, 1);
        } else if (type == 1 || type == 5) {
            unsigned first = ue(&r), st = ue(&r) % 5; ue(&r); u(&r, t.l2fn);
            if (type == 5) ue(&r);
            if (t.poct == 0) { u(&r, t.l2poc); if (t.bfpo) se(&r); }
            if (t.red) ue(&r);
            unsigned l0 = t.l0;
            if (st == 0) { if (u(&r, 1)) l0 = ue(&r) + 1; if (u(&r, 1)) for (unsigned idc; (idc = ue(&r)) != 3;) ue(&r); }
            if (t.wp && st == 0) {
                ue(&r); ue(&r);
                for (unsigned k = 0; k < l0; k++) { if (u(&r, 1)) { se(&r); se(&r); } if (u(&r, 1)) { se(&r); se(&r); se(&r); se(&r); } }
            }
            if (nal[0] & 0x60) {
                if (type == 5) u(&r, 2);
                else if (u(&r, 1)) for (unsigned op; (op = ue(&r));) { if (op == 1 || op == 3) ue(&r); if (op == 2 || op == 3 || op == 4 || op == 6) ue(&r); }
            }
            unsigned cinit = t.cabac && st != 2 ? ue(&r) : 0;
            int qp = t.qp + se(&r);
            unsigned idc = 0; int alpha = 0, beta = 0;
            if (t.dbf && (idc = ue(&r)) != 1) { alpha = se(&r); beta = se(&r); }
            /* The shift register starts at this RBSP bit; as the guest gives it, counted in raw bits from the NAL header. */
            unsigned sr_bit = 8 + raw[r.pos / 8] * 8 + r.pos % 8;
            unsigned x = first % t.w, y = first / t.w;
            /* The last slice of its picture unless the next NAL is a slice that doesn't start at macroblock 0. */
            size_t ns = e + (e + 2 < size && data[e + 2] == 1 ? 3 : 4);
            bool last = !(ns + 1 < size && ((data[ns] & 31) == 1 || (data[ns] & 31) == 5) && !(data[ns + 1] & 0x80));
            VXDSlice sl = {
                .nal = nal, .len = len, .sr_bit = sr_bit, .last = last,
                .sps0 = (t.w - 1) | t.fmo << 8 | 1 << 9 | t.d8x8 << 10,
                .pps0 = t.t8x8 | t.cip << 1 | t.cabac << 2 | t.wb << 8 | (t.cqp & 0x1f) << 16 | (t.cqp & 0x1f) << 24,
                .pic0 = (t.h - 1) << 8 | (t.w * t.h - 1) << 16,
                .slice0 = idc << 28 | (alpha & 15) << 24 | (beta & 15) << 20 | cinit << 14,
                .slice1 = y << 24 | x << 16 | qp << 10 | (t.l1 - 1) << 5 | (l0 - 1),
            };
            VXDPicture pic;
            int got = vxd_h264_decode(h, &sl, &pic);
            slices++;
            if (got < 0) { fprintf(stderr, "%s: slice %u failed\n", name, slices); assert(0); }
            if (got) {
                assert(pictures < frames->len && pic.width == (int)t.w * 16 && pic.height == (int)t.h * 16);
                const uint8_t *want = frames->pdata[pictures];
                size_t ys = (size_t)pic.width * pic.height;
                for (int yy = 0; yy < pic.height; yy++) assert(!memcmp(pic.plane[0] + yy * pic.stride[0], want + yy * pic.width, pic.width));
                for (int c = 1; c < 3; c++)
                    for (int yy = 0; yy < pic.height / 2; yy++)
                        assert(!memcmp(pic.plane[c] + yy * pic.stride[c], want + ys + (c - 1) * ys / 4 + yy * pic.width / 2, pic.width / 2));
                pictures++;
            }
        }
        i = e;
    }
    printf("%s: %u slices, %u of %u pictures identical\n", name, slices, pictures, frames->len);
    assert(pictures == frames->len);
    vxd_h264_free(h);
    avcodec_free_context(&ref);
    av_packet_free(&pkt);
    av_frame_free(&f);
    g_ptr_array_unref(frames);
}

int main(void)
{
    run("a", stream_a, sizeof(stream_a));
    run("b", stream_b, sizeof(stream_b));
    run("c", stream_c, sizeof(stream_c));
    return 0;
}
