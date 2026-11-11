/* H5P NV12 video: independently mapped planes, firmware-relative plane
 * offsets, and the shifted full-range CSC matrix.
 * PKG glib-2.0
 * SLICE:types hw/arm/s5l8930_display.c define DP_
 * SLICE:types hw/arm/s5l8930_display.c typedef UILayer
 * SLICE hw/arm/s5l8930_display.c fn h5p_ui_xlate fb_read video_compose
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MEMTXATTRS_UNSPECIFIED 0
typedef uint64_t hwaddr;
typedef struct {
    uint32_t regs[0x6000 / 4];
} DisplayPipe;
typedef struct {
    bool h5p;
    DisplayPipe pipe[1];
} S5L8930DisplayState;
static uint8_t ram[0x10000];
static int address_space_memory;
static uint32_t address_space_ldl_le(int *as, hwaddr pa, int attrs, void *res)
{
    uint32_t value;
    assert(pa + 4 <= sizeof(ram));
    memcpy(&value, ram + pa, 4);
    return value;
}
static void address_space_read(int *as, hwaddr pa, int attrs, void *dst,
                               unsigned len)
{
    assert(pa + len <= sizeof(ram));
    memcpy(dst, ram + pa, len);
}
static hwaddr dart_xlate(S5L8930DisplayState *s, uint32_t va)
{
    return va;
}
#include "types.h"
#include "slice.h"

static void put(uint32_t pa, uint32_t value)
{
    memcpy(ram + pa, &value, 4);
}

int main(void)
{
    S5L8930DisplayState s = {.h5p = true};
    uint32_t *r = s.pipe[0].regs, out[8];
    r[DP_LAYERS / 4] = 0x400;
    r[0x3094 / 4] = 4 << 16 | 2;
    r[0x309c / 4] = 4 << 16 | 2;
    r[0x3088 / 4] = r[0x308c / 4] = 16 | 2;
    r[0x307c / 4] = 0x80;
    r[0x3080 / 4] = 0x85c80;
    r[0x3d58 / 4] = r[0x3d5c / 4] = 1;
    r[0x3d70 / 4] = 0x1000;
    r[0x3df0 / 4] = 0x2000;
    put(0x1000, 0x8001);
    put(0x2000, 0x9001);
    r[0x3020 / 4] = 0xfffff800;
    r[0x3024 / 4] = r[0x3028 / 4] = 0xfffffe00;
    const int matrix[9] = {4096, 0, 5743, 4096, -1410, -2925, 4096, 7258, 0};
    for (unsigned i = 0; i < 9; i++)
        r[0x302c / 4 + i] = matrix[i];
    for (unsigned y = 0; y < 2; y++)
        for (unsigned x = 0; x < 4; x++)
            ram[0x8080 + y * 16 + x] = x < 2 ? 255 : 76;
    ram[0x9c80] = ram[0x9c81] = 128;
    ram[0x9c82] = 85;
    ram[0x9c83] = 255;
    assert(video_compose(&s, 4, 2, out));
    for (unsigned i = 0; i < 8; i++) {
        unsigned R = out[i] >> 16 & 255, G = out[i] >> 8 & 255,
                 B = out[i] & 255;
        if ((i % 4) < 2)
            assert(R == 255 && G == 255 && B == 255);
        else
            assert(R >= 250 && G < 5 && B < 5);
    }
    /* Native movie drags crop to odd visible sizes while the NV12 strides
  * remain aligned. The last chroma pair and odd final row still exist. */
    r[0x3094 / 4] = r[0x309c / 4] = 3 << 16 | 3;
    for (unsigned y = 0; y < 3; y++)
        for (unsigned x = 0; x < 4; x++)
            ram[0x8080 + y * 16 + x] = x < 2 ? 255 : 76;
    memcpy(ram + 0x9c90, ram + 0x9c80, 4);
    uint32_t odd[9];
    assert(video_compose(&s, 3, 3, odd));
    for (unsigned i = 0; i < 9; i++) {
        unsigned R = odd[i] >> 16 & 255, G = odd[i] >> 8 & 255,
                 B = odd[i] & 255;
        if (i % 3 < 2)
            assert(R == 255 && G == 255 && B == 255);
        else
            assert(R >= 250 && G < 5 && B < 5);
    }
    puts("ok");
}
