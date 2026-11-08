/* Generated NV12 pictures survive host encoding and independent H.264 decode.
 * No guest firmware, canned NALs or media assets are used.
 * SLICE hw/arm/img_topaz_h264.c range struct TopazH264 { | void topaz_h264_free
 * SLICE hw/arm/img_topaz_h264.c fn topaz_h264_free topaz_h264_new topaz_h264_encode topaz_h264_picture
 * CFLAGS -I$ROOT/hw/arm
 * PKG glib-2.0 libavcodec libavutil
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include "img_topaz_h264.h"
#include "slice.h"
int main(void)
{
    av_log_set_level(AV_LOG_ERROR);
    assert(!topaz_h264_new(63, 48));
    TopazH264 *encoder = topaz_h264_new(64, 48);
    assert(encoder);
    AVCodecContext *decoder =
        avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    decoder->thread_count = 1;
    assert(avcodec_open2(decoder, decoder->codec, NULL) == 0);
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    uint8_t y[64 * 48], uv[64 * 24];
    memset(uv, 128, sizeof(uv));
    for (unsigned pic = 0; pic < 3; pic++) {
        for (unsigned row = 0; row < 48; row++)
            for (unsigned col = 0; col < 64; col++)
                y[row * 64 + col] = (col < 32) ^ (pic & 1) ? 64 : 192;
        uint8_t *data = NULL;
        size_t n = 0;
        assert(topaz_h264_encode(encoder, y, uv, 64, 24, pic < 2, &data, &n));
        assert(n > 16);
        unsigned starts = 0, parameter_sets = 0, pictures = 0;
        for (size_t i = 0; i + 4 < n; i++)
            if (!memcmp(data + i, "\0\0\1", 3)) {
                assert(i > 0 &&
                       data[i - 1] == 0); /* Every prefix has four bytes. */
                assert((data[i + 3] & 31) !=
                       6); /* No optional identification SEI. */
                unsigned type = data[i + 3] & 31;
                parameter_sets += type == 7 || type == 8;
                pictures += type == 1 || type == 5;
                if (i < 512)
                    starts++;
            }
        assert(pictures == 1);
        assert(parameter_sets == (pic == 0 ? 2 : 0));
        assert(
            pic == 0
                ? starts >= 3
                : starts >=
                      1); /* First buffer supplies avcC; later IDRs supply pictures. */
        assert(av_new_packet(packet, n) == 0);
        memcpy(packet->data, data, n);
        g_free(data);
        assert(avcodec_send_packet(decoder, packet) == 0);
        av_packet_unref(packet);
        assert(avcodec_receive_frame(decoder, frame) == 0);
        assert(frame->width == 64 && frame->height == 48);
        for (unsigned col = 16; col < 64; col += 32) {
            int expected = (col < 32) ^ (pic & 1) ? 64 : 192;
            assert(abs(frame->data[0][24 * frame->linesize[0] + col] -
                       expected) <= 4);
        }
        uint8_t ry[64 * 48], ruv[64 * 24];
        assert(topaz_h264_picture(encoder, ry, ruv, 64));
        for (unsigned row = 0; row < 48; row++)
            assert(!memcmp(ry + row * 64,
                           frame->data[0] + row * frame->linesize[0], 64));
        for (unsigned row = 0; row < 24; row++)
            for (unsigned col = 0; col < 32; col++) {
                assert(ruv[row * 64 + col * 2] ==
                       frame->data[1][row * frame->linesize[1] + col]);
                assert(ruv[row * 64 + col * 2 + 1] ==
                       frame->data[2][row * frame->linesize[2] + col]);
            }
        av_frame_unref(frame);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    topaz_h264_free(encoder);
    puts("ok");
}
