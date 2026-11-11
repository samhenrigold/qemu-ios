/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Host compression backend for the virtual Topaz firmware interface.
 * Only guest-provided pixels cross this interface; no device firmware or
 * precomputed bitstreams are bundled with the emulator.
 */
#include "qemu/osdep.h"
#include "img_topaz_h264.h"
#ifdef IT_HAVE_AVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
struct TopazH264 {
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    AVCodecContext *decoder;
    AVFrame *reconstructed;
    int64_t frames;
};
void topaz_h264_free(TopazH264 *s)
{
    if (!s)
        return;
    av_packet_free(&s->packet);
    av_frame_free(&s->frame);
    avcodec_free_context(&s->codec);
    av_frame_free(&s->reconstructed);
    avcodec_free_context(&s->decoder);
    g_free(s);
}
TopazH264 *topaz_h264_new(unsigned width, unsigned height)
{
    if (!width || !height || width > 4096 || height > 4096 ||
        ((width | height) & 1))
        return NULL;
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
        return NULL;
    TopazH264 *s = g_new0(TopazH264, 1);
    s->codec = avcodec_alloc_context3(codec);
    s->frame = av_frame_alloc();
    s->packet = av_packet_alloc();
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    s->decoder = decoder ? avcodec_alloc_context3(decoder) : NULL;
    s->reconstructed = av_frame_alloc();
    if (!s->codec || !s->frame || !s->packet || !s->decoder ||
        !s->reconstructed)
        goto fail;
    s->decoder->thread_count = 1;
    if (avcodec_open2(s->decoder, decoder, NULL) < 0)
        goto fail;
    s->codec->width = width;
    s->codec->height = height;
    s->codec->pix_fmt = AV_PIX_FMT_YUV420P;
    s->codec->time_base = (AVRational){1, 30};
    s->codec->framerate = (AVRational){30, 1};
    s->codec->gop_size = 30;
    s->codec->max_b_frames = 0;
    s->codec->thread_count = 1;
    av_opt_set(s->codec->priv_data, "qp", "24", 0);
    av_opt_set(s->codec->priv_data, "preset", "ultrafast", 0);
    av_opt_set(s->codec->priv_data, "tune", "zerolatency", 0);
    av_opt_set(s->codec->priv_data, "profile", "baseline", 0);
    av_opt_set(s->codec->priv_data, "x264-params",
               "annexb=1:repeat-headers=1:scenecut=0", 0);
    if (avcodec_open2(s->codec, codec, NULL) < 0)
        goto fail;
    s->frame->format = s->codec->pix_fmt;
    s->frame->width = width;
    s->frame->height = height;
    if (av_frame_get_buffer(s->frame, 32) < 0)
        goto fail;
    return s;
fail:
    topaz_h264_free(s);
    return NULL;
}
bool topaz_h264_encode(TopazH264 *s, const uint8_t *y, const uint8_t *uv,
                       unsigned stride, unsigned qp, bool idr, uint8_t **output,
                       size_t *length)
{
    *output = NULL;
    *length = 0;
    if (!s || !y || !uv || stride < (unsigned)s->frame->width ||
        av_frame_make_writable(s->frame) < 0)
        return false;
    unsigned w = s->frame->width, h = s->frame->height;
    for (unsigned row = 0; row < h; row++) {
        memcpy(s->frame->data[0] + row * s->frame->linesize[0],
               y + row * stride, w);
    }
    for (unsigned row = 0; row < h / 2; row++) {
        uint8_t *u = s->frame->data[1] + row * s->frame->linesize[1];
        uint8_t *v = s->frame->data[2] + row * s->frame->linesize[2];
        for (unsigned col = 0; col < w / 2; col++) {
            u[col] = uv[row * stride + col * 2];
            v[col] = uv[row * stride + col * 2 + 1];
        }
    }
    s->frame->pts = s->frames++;
    s->frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    s->frame->quality = MIN(qp, 51u) * FF_QP2LAMBDA;
    if (avcodec_send_frame(s->codec, s->frame) < 0)
        return false;
    av_packet_unref(s->packet);
    if (avcodec_receive_packet(s->codec, s->packet) < 0 ||
        s->packet->size <= 0 || s->packet->size > 16 * 1024 * 1024)
        return false;
    /* Native clients can read the hardware reconstruction and use it as a
     * later source (notably iOS 7's IntraWasInter pass). Reconstruct from the
     * actual coded picture, including quantization, before stripping avc1
     * parameter sets from the guest-facing access unit. */
    av_frame_unref(s->reconstructed);
    if (avcodec_send_packet(s->decoder, s->packet) < 0 ||
        avcodec_receive_frame(s->decoder, s->reconstructed) < 0)
        return false;
    /* The firmware interface uses four-byte start codes for every NAL.
     * In particular the guest searches a bounded prefix for SPS/PPS followed
     * by a third start code. x264's three-byte prefixes and optional long
     * identification SEI would hide that boundary from the guest parser. */
    GByteArray *access = g_byte_array_new();
    const uint8_t *data = s->packet->data;
    size_t size = s->packet->size, start = 0;
    while (start < size) {
        unsigned prefix =
            start + 4 <= size && !memcmp(data + start, "\0\0\0\1", 4) ? 4
            : start + 3 <= size && !memcmp(data + start, "\0\0\1", 3) ? 3
                                                                      : 0;
        if (!prefix || start + prefix == size) {
            g_byte_array_unref(access);
            return false;
        }
        size_t next = start + prefix + 1;
        while (next + 3 <= size && memcmp(data + next, "\0\0\1", 3) &&
               !(next + 4 <= size && !memcmp(data + next, "\0\0\0\1", 4)))
            next++;
        if (next + 3 > size)
            next = size;
        unsigned type = data[start + prefix] & 31;
        /* The native client extracts parameter sets from the first coded
         * buffer into avcC. Later avc1 samples must contain picture NALs;
         * repeating headers there makes the native playback parser skip
         * the keyframe, leaving subsequent pictures without a reference. */
        if (type != 6 && (s->frames == 1 || (type != 7 && type != 8))) {
            static const uint8_t code[4] = {0, 0, 0, 1};
            g_byte_array_append(access, code, sizeof(code));
            g_byte_array_append(access, data + start + prefix,
                                next - start - prefix);
        }
        start = next;
    }
    *length = access->len;
    *output = g_byte_array_free(access, false);
    return true;
}
bool topaz_h264_picture(TopazH264 *s, uint8_t *y, uint8_t *uv, unsigned stride)
{
    if (!s || !y || !uv || !s->reconstructed->data[0] ||
        s->reconstructed->format != AV_PIX_FMT_YUV420P ||
        stride < (unsigned)s->reconstructed->width)
        return false;
    AVFrame *f = s->reconstructed;
    for (unsigned row = 0; row < f->height; row++) {
        memcpy(y + row * stride, f->data[0] + row * f->linesize[0], f->width);
    }
    for (unsigned row = 0; row < f->height / 2; row++) {
        for (unsigned col = 0; col < f->width / 2; col++) {
            uv[row * stride + col * 2] = f->data[1][row * f->linesize[1] + col];
            uv[row * stride + col * 2 + 1] =
                f->data[2][row * f->linesize[2] + col];
        }
    }
    return true;
}
#else
struct TopazH264 {
    int unused;
};
TopazH264 *topaz_h264_new(unsigned w, unsigned h)
{
    return NULL;
}
void topaz_h264_free(TopazH264 *s)
{
}
bool topaz_h264_encode(TopazH264 *s, const uint8_t *y, const uint8_t *uv,
                       unsigned stride, unsigned qp, bool idr, uint8_t **output,
                       size_t *length)
{
    *output = NULL;
    *length = 0;
    return false;
}
bool topaz_h264_picture(TopazH264 *s, uint8_t *y, uint8_t *uv, unsigned stride)
{
    return false;
}
#endif
