/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest upload policy around Imagination's unmodified reference decoder.
 * The SDK assumes aligned native-endian words and spec-padded mip tails.
 * Guest bytes are little-endian and some legacy drivers send compact tails.
 */
#include "pvrtc.h"
#include "PVRTDecompress.cpp"
#include <new>

extern "C" bool ltm_pvrtc_decode(const uint8_t *src, uint32_t w, uint32_t h,
                                 int bpp, bool alpha, bool padded, uint8_t *dst)
{
    if (!src || !dst || !w || !h || w > 4096 || h > 4096 ||
        (w & (w - 1)) || (h & (h - 1)) || (bpp != 2 && bpp != 4)) {
        return false;
    }
    const uint32_t block_width = bpp == 2 ? 8 : 4;
    const uint32_t minimum = padded ? 2 : 1;
    const uint32_t bw = std::max(w / block_width, minimum);
    const uint32_t bh = std::max(h / 4, minimum);
    const uint32_t out_bw = std::max(bw, 2u);
    const uint32_t out_bh = std::max(bh, 2u);
    try {
        std::vector<uint32_t> words(size_t(out_bw) * out_bh * 2);
        for (uint32_t y = 0; y < out_bh; ++y) {
            for (uint32_t x = 0; x < out_bw; ++x) {
                const uint8_t *in = src + pvr::TwiddleUV(bw, bh, x % bw, y % bh) * 8;
                const uint32_t offset = pvr::TwiddleUV(out_bw, out_bh, x, y) * 2;
                for (uint32_t i = 0; i < 2; ++i, in += 4) {
                    words[offset + i] = uint32_t(in[0]) | uint32_t(in[1]) << 8 |
                                        uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24;
                }
            }
        }
        pvr::PVRTDecompressPVRTC(words.data(), bpp == 2, w, h, dst);
        if (!alpha) {
            for (size_t i = 3; i < size_t(w) * h * 4; i += 4) {
                dst[i] = 255;
            }
        }
        return true;
    } catch (const std::bad_alloc &) {
        return false;
    }
}
