/* Camera-compatible BT.601 NV12 render-target conversion.
 * SLICE hw/arm/gles-host.c fn gles_bgra_to_nv12
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include "slice.h"
int main(void)
{
    uint8_t rgba[16], yuv[6];
    for (unsigned full = 0; full < 2; full++) {
        memset(rgba, 0, sizeof(rgba));
        assert(gles_bgra_to_nv12(2, 2, full, rgba, yuv));
        for (unsigned i = 0; i < 4; i++)
            assert(yuv[i] == (full ? 0 : 16));
        assert(yuv[4] == 128 && yuv[5] == 128);
        memset(rgba, 255, sizeof(rgba));
        assert(gles_bgra_to_nv12(2, 2, full, rgba, yuv));
        for (unsigned i = 0; i < 4; i++)
            assert(yuv[i] == (full ? 255 : 235));
        assert(yuv[4] == 128 && yuv[5] == 128);
        for (unsigned i = 0; i < 4; i++) {
            rgba[i * 4] = 0;
            rgba[i * 4 + 1] = 0;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = i;
        }
        assert(gles_bgra_to_nv12(2, 2, full, rgba, yuv));
        assert(yuv[0] == (full ? 77 : 82));
        assert(yuv[4] == (full ? 85 : 90));
        assert(yuv[5] == (full ? 255 : 240));
    }
    /* Chroma is shared across the four pixels; balanced colors remain neutral. */
    const uint8_t colors[16] = {0,   0, 255, 0, 0,   255, 0,   0,
                                255, 0, 0,   0, 255, 255, 255, 0};
    assert(gles_bgra_to_nv12(2, 2, true, colors, yuv));
    assert(yuv[4] == 128 && yuv[5] == 128);
    assert(!gles_bgra_to_nv12(1, 2, true, rgba, yuv));
    assert(!gles_bgra_to_nv12(2, 0, true, rgba, yuv));
    puts("ok");
}
