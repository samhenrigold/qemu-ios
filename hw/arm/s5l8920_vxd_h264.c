/*
 * VXD H.264: decode the guest's slices on the host.
 *
 * AppleVXD375Framework parses the stream on the CPU, as a DXVA host driver
 * does, and gives the decoder only what its entropy decoder needs: register
 * fields (picture size, entropy mode, slice QP, active references...) and the
 * slice NAL, whose shift register starts after the slice header. The NAL is
 * still whole in guest memory, so the host decoder takes it as it is; what it
 * lacks is the SPS and PPS. Those are rebuilt: the register fields give some,
 * and the rest are the parameters under which the slice header parses to end
 * exactly where the shift register starts, with the registers' QP, active
 * reference count and CABAC table. Candidates that fail a slice drop out.
 *
 * The host decoder keeps its own DPB, fed every slice in decode order, as the
 * guest's own DPB is. ponytail: no seeding from the guest's reference planes
 * (the iPod bridge's I_PCM priming); a skipped or dropped picture leaves the
 * two DPBs apart until the next IDR.
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "s5l8920_vxd_h264.h"

#ifdef IT_HAVE_AVCODEC
#include <libavcodec/avcodec.h>

typedef struct BitReader {
    const uint8_t *p;
    size_t bits, pos;
    bool bad;
} BitReader;

static unsigned br_u(BitReader *b, unsigned n)
{
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++, b->pos++) {
        if (b->pos >= b->bits) {
            b->bad = true;
            return 0;
        }
        v = v << 1 | ((b->p[b->pos / 8] >> (7 - b->pos % 8)) & 1);
    }
    return v;
}

static unsigned br_ue(BitReader *b)
{
    unsigned z = 0;
    while (!br_u(b, 1) && !b->bad) {
        if (++z > 31) {
            b->bad = true;
            return 0;
        }
    }
    return (1u << z) - 1 + br_u(b, z);
}

static int br_se(BitReader *b)
{
    unsigned v = br_ue(b);
    return v & 1 ? (int)((v + 1) / 2) : -(int)(v / 2);
}

typedef struct BitWriter {
    GByteArray *out;
    uint8_t cur;
    unsigned n;
} BitWriter;

static void bw_u(BitWriter *w, unsigned n, unsigned v)
{
    while (n--) {
        w->cur = w->cur << 1 | ((v >> n) & 1);
        if (++w->n == 8) {
            g_byte_array_append(w->out, &w->cur, 1);
            w->n = 0;
            w->cur = 0;
        }
    }
}

static void bw_ue(BitWriter *w, unsigned v)
{
    unsigned bits = 32 - clz32(v + 1);
    bw_u(w, bits - 1, 0);
    bw_u(w, bits, v + 1);
}

static void bw_se(BitWriter *w, int v)
{
    bw_ue(w, v > 0 ? 2 * v - 1 : -2 * v);
}

/* RBSP trailing bits, then emulation prevention, with a start code in front. */
static GByteArray *bw_nal(BitWriter *w, uint8_t header)
{
    static const uint8_t start[4] = { 0, 0, 0, 1 };
    GByteArray *nal = g_byte_array_new();
    unsigned zeros = 0;

    bw_u(w, 1, 1);
    if (w->n) {
        bw_u(w, 8 - w->n, 0);
    }
    g_byte_array_append(nal, start, 4);
    g_byte_array_append(nal, &header, 1);
    for (unsigned i = 0; i < w->out->len; i++) {
        uint8_t c = w->out->data[i];
        if (zeros == 2 && c <= 3) {
            static const uint8_t three = 3;
            g_byte_array_append(nal, &three, 1);
            zeros = 0;
        }
        g_byte_array_append(nal, &c, 1);
        zeros = c ? 0 : zeros + 1;
    }
    g_byte_array_unref(w->out);
    return nal;
}

/* The SPS and PPS fields a slice header's parse depends on, and what was learned with them. */
typedef struct Params {
    uint8_t log2_max_frame_num, poc_type, log2_max_poc_lsb;
    bool delta_pic_order_always_zero, bottom_field_pic_order, deblocking_control, redundant_pic_cnt, weighted_pred;
    bool alive;
    int pic_init_qp;                 /* -1 until a slice fixes it */
    int num_ref_idx_l0_default;      /* 0 until a P slice without an override fixes it */
    int num_ref_idx_l1_default;
    int prev_ref_fn, cur_fn;         /* frame_num of the last reference picture before this one, and this one's */
    bool cur_ref;
} Params;

#define N_FRAME_NUM 13
#define N_POC       (1 + 13 * 2 + 2 * 2)
#define N_PARAMS    (N_FRAME_NUM * N_POC * 8)

struct VXDH264 {
    Params params[N_PARAMS];
    int chosen;
    AVCodecContext *codec;
    AVFrame *frame;
    uint32_t key[3];                 /* SPS0, PPS0, PIC0 of the open stream */
    GByteArray *sps, *pps;           /* what the decoder has */
    GPtrArray *pending;              /* the current picture's slices, Annex B */
};

static void params_init(VXDH264 *h)
{
    unsigned i = 0;

    for (unsigned fn = 0; fn < N_FRAME_NUM; fn++) {
        for (unsigned poc = 0; poc < N_POC; poc++) {
            for (unsigned flags = 0; flags < 8; flags++, i++) {
                Params *p = &h->params[i];
                *p = (Params){ .log2_max_frame_num = 4 + fn, .alive = true, .pic_init_qp = -1, .prev_ref_fn = -1 };
                if (poc == 0) {
                    p->poc_type = 2;
                } else if (poc <= 26) {
                    p->poc_type = 0;
                    p->log2_max_poc_lsb = 4 + (poc - 1) / 2;
                    p->bottom_field_pic_order = (poc - 1) & 1;
                } else {
                    p->poc_type = 1;
                    p->delta_pic_order_always_zero = (poc - 27) & 1;
                    p->bottom_field_pic_order = (poc - 27) >> 1;
                }
                p->deblocking_control = flags & 1;
                p->redundant_pic_cnt = flags & 2;
                p->weighted_pred = flags & 4;
            }
        }
    }
    h->chosen = -1;
}

/* Field of the guest's FE/BE registers (psb's names). */
#define SPS0_FRAME_MBS_ONLY(r)   (((r) >> 8) & 1)
#define SPS0_DIRECT_8X8(r)       (((r) >> 10) & 1)
#define SPS0_WIDTH_MBS(r)        (((r) & 0x7f) + 1)
#define PPS0_TRANSFORM_8X8(r)    ((r) & 1)
#define PPS0_CONSTRAINED_INTRA(r) (((r) >> 1) & 1)
#define PPS0_CABAC(r)            (((r) >> 2) & 1)
#define PPS0_WEIGHTED_BIPRED(r)  (((r) >> 8) & 3)
#define PPS0_CHROMA_QP(r)        (sextract32((r), 16, 5))
#define PPS0_SECOND_CHROMA_QP(r) (sextract32((r), 24, 5))
#define PIC0_HEIGHT_MBS(r)       ((((r) >> 8) & 0x7f) + 1)
#define SLICE0_CABAC_INIT(r)     (((r) >> 14) & 3)
#define SLICE0_DEBLOCK_IDC(r)    (((r) >> 28) & 3)
#define SLICE0_ALPHA_DIV2(r)     (sextract32((r), 24, 4))
#define SLICE0_BETA_DIV2(r)      (sextract32((r), 20, 4))
#define SLICE1_QP(r)             (((r) >> 10) & 0x3f)
#define SLICE1_L0_ACTIVE(r)      (((r) & 0x1f) + 1)
#define SLICE1_L1_ACTIVE(r)      ((((r) >> 5) & 0x1f) + 1)

/* Parse the slice header under p; true if it agrees with the slice's registers. */
static bool slice_fits(Params *p, const VXDSlice *sl, const uint8_t *rbsp, size_t len, unsigned *pps_id)
{
    BitReader b = { .p = rbsp + 1, .bits = (len - 1) * 8 };
    uint8_t nal = rbsp[0];
    bool idr = (nal & 0x1f) == 5, field = false;
    bool cabac = PPS0_CABAC(sl->pps0);
    unsigned type, l0 = 0, l1 = 0;
    bool override = false;

    br_ue(&b);                                   /* first_mb_in_slice */
    type = br_ue(&b) % 5;
    *pps_id = br_ue(&b);
    int frame_num = br_u(&b, p->log2_max_frame_num);
    if (!SPS0_FRAME_MBS_ONLY(sl->sps0)) {
        field = br_u(&b, 1);
        if (field) {
            br_u(&b, 1);
        }
    }
    if (idr) {
        br_ue(&b);
    }
    if (p->poc_type == 0) {
        br_u(&b, p->log2_max_poc_lsb);
        if (p->bottom_field_pic_order && !field) {
            br_se(&b);
        }
    } else if (p->poc_type == 1 && !p->delta_pic_order_always_zero) {
        br_se(&b);
        if (p->bottom_field_pic_order && !field) {
            br_se(&b);
        }
    }
    if (p->redundant_pic_cnt && br_ue(&b)) {
        return false;                            /* the guest decodes primary pictures only */
    }
    if (type == 1) {
        br_u(&b, 1);                             /* direct_spatial_mv_pred */
    }
    if (type == 0 || type == 1 || type == 3) {
        override = br_u(&b, 1);
        if (override) {
            l0 = br_ue(&b) + 1;
            if (type == 1) {
                l1 = br_ue(&b) + 1;
            }
        }
    }
    for (unsigned list = 0; list < (type == 1 ? 2 : type % 5 == 2 || type == 4 ? 0 : 1); list++) {
        if (br_u(&b, 1)) {
            for (unsigned n = 0; n < 33; n++) {
                unsigned idc = br_ue(&b);
                if (idc == 3 || b.bad) {
                    break;
                }
                br_ue(&b);
            }
        }
    }
    unsigned n0 = override ? l0 : SLICE1_L0_ACTIVE(sl->slice1);
    unsigned n1 = override ? l1 : SLICE1_L1_ACTIVE(sl->slice1);
    if ((p->weighted_pred && (type == 0 || type == 3)) || (PPS0_WEIGHTED_BIPRED(sl->pps0) == 1 && type == 1)) {
        br_ue(&b);
        br_ue(&b);
        for (unsigned list = 0; list < (type == 1 ? 2u : 1u); list++) {
            for (unsigned i = 0; i < (list ? n1 : n0) && !b.bad; i++) {
                if (br_u(&b, 1)) {
                    br_se(&b);
                    br_se(&b);
                }
                if (br_u(&b, 1)) {
                    br_se(&b); br_se(&b); br_se(&b); br_se(&b);
                }
            }
        }
    }
    if (nal & 0x60) {
        if (idr) {
            br_u(&b, 2);
        } else if (br_u(&b, 1)) {
            for (unsigned n = 0; n < 66; n++) {
                unsigned op = br_ue(&b);
                if (!op || b.bad) {
                    break;
                }
                if (op == 1 || op == 3) {
                    br_ue(&b);
                }
                if (op == 2 || op == 3 || op == 4 || op == 6) {
                    br_ue(&b);
                }
            }
        }
    }
    if (cabac && type != 2 && type != 4) {
        if (br_ue(&b) != SLICE0_CABAC_INIT(sl->slice0)) {
            return false;
        }
    }
    int qp_delta = br_se(&b);
    if (type == 3 || type == 4) {
        if (type == 3) {
            br_u(&b, 1);
        }
        br_se(&b);
    }
    unsigned idc = 0;
    int alpha = 0, beta = 0;
    if (p->deblocking_control) {
        idc = br_ue(&b);
        if (idc != 1) {
            alpha = br_se(&b);
            beta = br_se(&b);
        }
    }
    if (b.bad || b.pos + 8 != sl->sr_bit || idc != SLICE0_DEBLOCK_IDC(sl->slice0) ||
        alpha != SLICE0_ALPHA_DIV2(sl->slice0) || beta != SLICE0_BETA_DIV2(sl->slice0)) {
        return false;
    }
    int init = (int)SLICE1_QP(sl->slice1) - qp_delta;
    if (init < 0 || init > 51 || (p->pic_init_qp >= 0 && p->pic_init_qp != init)) {
        return false;
    }
    if ((type == 0 || type == 3) && !override && p->num_ref_idx_l0_default &&
        p->num_ref_idx_l0_default != (int)n0) {
        return false;
    }
    /*
     * frame_num tells the split between it and the POC fields: a picture's slices share it, an IDR's is 0, and
     * any other picture's follows the last reference picture's by one. ponytail: a stream with gaps in frame_num
     * (or MMCO 5) disproves every candidate; the search then starts over at that slice.
     */
    bool start = !((sl->slice1 >> 16) & 0x7f7f);
    int prev_ref = p->cur_ref ? p->cur_fn : p->prev_ref_fn;
    if (start ? (idr ? frame_num != 0 : prev_ref >= 0 && frame_num != (prev_ref + 1) % (1 << p->log2_max_frame_num))
              : frame_num != p->cur_fn) {
        return false;
    }
    if (start) {
        p->prev_ref_fn = idr ? -1 : prev_ref;
        p->cur_fn = frame_num;
        p->cur_ref = nal & 0x60;
    }
    p->pic_init_qp = init;
    if ((type == 0 || type == 3 || type == 1) && !override) {
        p->num_ref_idx_l0_default = n0;
        if (type == 1) {
            p->num_ref_idx_l1_default = n1;
        }
    }
    return true;
}

static GByteArray *make_sps(const Params *p, const VXDSlice *sl)
{
    BitWriter w = { .out = g_byte_array_new() };

    bw_u(&w, 8, 100);                    /* High: a superset of what the guest's profiles parse */
    bw_u(&w, 8, 0);
    bw_u(&w, 8, 51);
    bw_ue(&w, 0);                        /* sps id */
    bw_ue(&w, 1);                        /* 4:2:0 */
    bw_ue(&w, 0);
    bw_ue(&w, 0);
    bw_u(&w, 1, 0);
    bw_u(&w, 1, 0);                      /* ponytail: flat scaling matrices only */
    bw_ue(&w, p->log2_max_frame_num - 4);
    bw_ue(&w, p->poc_type);
    if (p->poc_type == 0) {
        bw_ue(&w, p->log2_max_poc_lsb - 4);
    } else if (p->poc_type == 1) {
        bw_u(&w, 1, p->delta_pic_order_always_zero);
        bw_se(&w, 0);
        bw_se(&w, 0);
        bw_ue(&w, 0);
    }
    bw_ue(&w, 16);                       /* max refs: the guest's DPB decides what is kept */
    bw_u(&w, 1, 1);                      /* gaps allowed */
    bw_ue(&w, SPS0_WIDTH_MBS(sl->sps0) - 1);
    bw_ue(&w, PIC0_HEIGHT_MBS(sl->pic0) / (SPS0_FRAME_MBS_ONLY(sl->sps0) ? 1 : 2) - 1);
    bw_u(&w, 1, SPS0_FRAME_MBS_ONLY(sl->sps0));
    if (!SPS0_FRAME_MBS_ONLY(sl->sps0)) {
        bw_u(&w, 1, 0);
    }
    bw_u(&w, 1, SPS0_DIRECT_8X8(sl->sps0));
    bw_u(&w, 1, 0);
    bw_u(&w, 1, 0);
    return bw_nal(&w, 0x67);
}

static GByteArray *make_pps(const Params *p, const VXDSlice *sl, unsigned pps_id)
{
    BitWriter w = { .out = g_byte_array_new() };

    bw_ue(&w, pps_id);
    bw_ue(&w, 0);
    bw_u(&w, 1, PPS0_CABAC(sl->pps0));
    bw_u(&w, 1, p->bottom_field_pic_order);
    bw_ue(&w, 0);
    bw_ue(&w, MAX(p->num_ref_idx_l0_default, 1) - 1);
    bw_ue(&w, MAX(p->num_ref_idx_l1_default, 1) - 1);
    bw_u(&w, 1, p->weighted_pred);
    bw_u(&w, 2, PPS0_WEIGHTED_BIPRED(sl->pps0));
    bw_se(&w, MAX(p->pic_init_qp, 0) - 26);
    bw_se(&w, 0);
    bw_se(&w, PPS0_CHROMA_QP(sl->pps0));
    bw_u(&w, 1, p->deblocking_control);
    bw_u(&w, 1, PPS0_CONSTRAINED_INTRA(sl->pps0));
    bw_u(&w, 1, p->redundant_pic_cnt);
    bw_u(&w, 1, PPS0_TRANSFORM_8X8(sl->pps0));
    bw_u(&w, 1, 0);
    bw_se(&w, PPS0_SECOND_CHROMA_QP(sl->pps0));
    return bw_nal(&w, 0x68);
}

VXDH264 *vxd_h264_new(void)
{
    VXDH264 *h = g_new0(VXDH264, 1);
    params_init(h);
    h->pending = g_ptr_array_new_with_free_func((GDestroyNotify)g_byte_array_unref);
    return h;
}

void vxd_h264_free(VXDH264 *h)
{
    if (h) {
        avcodec_free_context(&h->codec);
        g_clear_pointer(&h->sps, g_byte_array_unref);
        g_clear_pointer(&h->pps, g_byte_array_unref);
        g_ptr_array_unref(h->pending);
        av_frame_free(&h->frame);
        g_free(h);
    }
}

static int vxd_send(VXDH264 *h, const uint8_t *data, size_t len)
{
    AVPacket *pkt = av_packet_alloc();
    int err = -1;

    if (pkt && av_new_packet(pkt, len) == 0) {
        memcpy(pkt->data, data, len);
        err = avcodec_send_packet(h->codec, pkt);
    }
    av_packet_free(&pkt);
    return err;
}

int vxd_h264_decode(VXDH264 *h, const VXDSlice *sl, VXDPicture *pic)
{
    /* The NAL's RBSP, and the shift register's start counted in it. */
    g_autofree uint8_t *rbsp = g_malloc(sl->len);
    size_t n = 0;
    unsigned zeros = 0, sr_bit = sl->sr_bit;
    for (size_t i = 0; i < sl->len; i++) {
        if (zeros == 2 && sl->nal[i] == 3) {
            if (i * 8 < sl->sr_bit) {
                sr_bit -= 8;
            }
            zeros = 0;
            continue;
        }
        rbsp[n++] = sl->nal[i];
        zeros = sl->nal[i] ? 0 : zeros + 1;
    }
    VXDSlice rs = *sl;
    rs.sr_bit = sr_bit;

    unsigned pps_id = 0, alive = 0;
    for (int pass = 0; pass < 2 && !alive; pass++) {
        if (pass) {
            params_init(h);          /* nothing fits: a new stream */
        }
        for (unsigned i = 0; i < N_PARAMS; i++) {
            Params *p = &h->params[i];
            unsigned id;
            if (p->alive && (p->alive = slice_fits(p, &rs, rbsp, n, &id))) {
                if (!alive++) {
                    pps_id = id;
                }
            }
        }
    }
    if (!alive) {
        return -1;
    }
    /* Keep the parameters in use while they still fit; else the first that does. */
    if (h->chosen < 0 || !h->params[h->chosen].alive) {
        for (unsigned i = 0; i < N_PARAMS; i++) {
            if (h->params[i].alive) {
                h->chosen = i;
                break;
            }
        }
    }
    if (!h->codec) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        h->codec = codec ? avcodec_alloc_context3(codec) : NULL;
        h->frame = av_frame_alloc();
        if (!h->codec || !h->frame) {
            return -1;
        }
        h->codec->thread_count = 1;
        h->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        h->codec->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        if (avcodec_open2(h->codec, codec, NULL) < 0) {
            return -1;
        }
    }
    /*
     * A picture's slices reach the decoder together, once its last one is here, under the parameters that fit
     * them all: a choice a later slice disproves must not have decoded the earlier ones (the decoder takes no
     * new PPS within a picture). ponytail: a picture whose last slice never comes is dropped.
     */
    if (!((sl->slice1 >> 16) & 0x7f7f)) {
        g_ptr_array_set_size(h->pending, 0);
    }
    GByteArray *annexb = g_byte_array_sized_new(sl->len + 4);
    g_byte_array_append(annexb, (const uint8_t *)"\0\0\0\1", 4);
    g_byte_array_append(annexb, sl->nal, sl->len);
    g_ptr_array_add(h->pending, annexb);
    if (!sl->last) {
        return 0;
    }
    uint32_t key[3] = { sl->sps0, sl->pps0, sl->pic0 };
    g_autoptr(GByteArray) sps = make_sps(&h->params[h->chosen], sl);
    g_autoptr(GByteArray) pps = make_pps(&h->params[h->chosen], sl, pps_id);
    if (memcmp(key, h->key, sizeof(key))) {
        avcodec_flush_buffers(h->codec);
        memcpy(h->key, key, sizeof(key));
    }
    if (!h->sps || sps->len != h->sps->len || memcmp(sps->data, h->sps->data, sps->len) ||
        pps->len != h->pps->len || memcmp(pps->data, h->pps->data, pps->len)) {
        if (vxd_send(h, sps->data, sps->len) < 0 || vxd_send(h, pps->data, pps->len) < 0) {
            return -1;
        }
        g_clear_pointer(&h->sps, g_byte_array_unref);
        g_clear_pointer(&h->pps, g_byte_array_unref);
        h->sps = g_steal_pointer(&sps);
        h->pps = g_steal_pointer(&pps);
    }
    for (unsigned i = 0; i < h->pending->len; i++) {
        GByteArray *nal = h->pending->pdata[i];
        if (vxd_send(h, nal->data, nal->len) < 0) {
            g_ptr_array_set_size(h->pending, 0);
            return -1;
        }
    }
    g_ptr_array_set_size(h->pending, 0);
    int err = avcodec_receive_frame(h->codec, h->frame);
    if (err == AVERROR(EAGAIN)) {
        return 0;
    }
    if (err < 0 || (h->frame->format != AV_PIX_FMT_YUV420P && h->frame->format != AV_PIX_FMT_YUVJ420P)) {
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        pic->plane[i] = h->frame->data[i];
        pic->stride[i] = h->frame->linesize[i];
    }
    pic->width = h->frame->width;
    pic->height = h->frame->height;
    return 1;
}

#else

VXDH264 *vxd_h264_new(void) { return NULL; }
void vxd_h264_free(VXDH264 *h) { }
int vxd_h264_decode(VXDH264 *h, const VXDSlice *sl, VXDPicture *pic) { return -1; }

#endif
