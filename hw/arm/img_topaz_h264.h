/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_IMG_TOPAZ_H264_H
#define HW_ARM_IMG_TOPAZ_H264_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct TopazH264 TopazH264;
TopazH264 *topaz_h264_new(unsigned width, unsigned height);
void topaz_h264_free(TopazH264 *encoder);
/* Contiguous NV12 input; the caller owns the returned Annex-B access unit. */
bool topaz_h264_encode(TopazH264 *encoder, const uint8_t *y, const uint8_t *uv,
                       unsigned stride, unsigned qp, bool idr, uint8_t **output,
                       size_t *length);
/* The decoded, reconstructed picture from the last completed access unit. */
bool topaz_h264_picture(TopazH264 *encoder, uint8_t *y, uint8_t *uv,
                        unsigned stride);
#endif
