/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LTM_PVRTC_H
#define LTM_PVRTC_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller validates the compressed byte count before invoking this decoder. */
bool ltm_pvrtc_decode(const uint8_t *src, uint32_t width, uint32_t height,
                      int bpp, bool alpha, bool padded, uint8_t *dst);
#ifdef __cplusplus
}
#endif
#endif
