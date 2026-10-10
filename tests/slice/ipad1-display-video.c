/* The A4 display's video layer (video_compose): NV12 at +0x307c/+0x3080, scaled into its destination rectangle and
 * converted by the guest's own matrix (the values 4.2.1's MPMoviePlayer programs, BT.601 limited range), and
 * transparent black outside it.
 *
 * SLICE:types hw/arm/s5l8930_display.c define (DP_LAYERS)[[:space:](]
 * SLICE hw/arm/s5l8930_display.c fn video_compose
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t regs[0x6000 / 4]; } DisplayPipe;
typedef struct S5L8930DisplayState { DisplayPipe pipe[1]; } S5L8930DisplayState;
#include "types.h"

static uint8_t ram[0x10000];
static void fb_read(S5L8930DisplayState *s, uint32_t va, uint8_t *dst, unsigned len)
{
    assert(va + len <= sizeof(ram));
    memcpy(dst, ram + va, len);
}
#include "slice.h"

enum { W = 8, H = 6, Y = 0x1000, UV = 0x2000 };

int main(void)
{
    static S5L8930DisplayState st;
    uint32_t *r = st.pipe[0].regs, out[W * H];
    static const uint16_t matrix[9] = { 0x12a1, 0, 0x1989, 0x12a1, 0xf9bc, 0xf2fe, 0x12a1, 0x2046, 0 };

    /* A 4x2 source, one chroma sample for each 2x2: white in the left half, red in the right. */
    for (unsigned x = 0; x < 4; x++) {
        ram[Y + x] = ram[Y + 16 + x] = x < 2 ? 235 : 81;
    }
    ram[UV + 0] = 128; ram[UV + 1] = 128;      /* white's Cb, Cr */
    ram[UV + 2] = 90;  ram[UV + 3] = 240;      /* red's */
    r[DP_LAYERS / 4] = 0x400;
    r[0x307c / 4] = Y; r[0x3080 / 4] = UV; r[0x3088 / 4] = 16 | 2; r[0x308c / 4] = 16;
    r[0x3094 / 4] = 4 << 16 | 2;               /* 4x2 source */
    r[0x309c / 4] = 4 << 16 | 4;               /* to 4x4 */
    r[0x30a0 / 4] = 2 << 16 | 1;               /* at (2, 1) */
    for (int i = 0; i < 9; i++) {
        r[0x3024 / 4 + i] = matrix[i];
    }
    assert(video_compose(&st, W, H, out));
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x++) {
            uint32_t px = out[y * W + x];
            if (x < 2 || x >= 6 || y < 1 || y >= 5) {
                assert(px == 0);               /* outside the layer */
            } else if (x < 4) {
                assert(px == 0xffffffffu);     /* white, opaque */
            } else {
                unsigned R = px >> 16 & 0xff, G = px >> 8 & 0xff, B = px & 0xff;
                assert(px >> 24 == 0xff && R >= 250 && G <= 5 && B <= 5);
            }
        }
    }
    r[DP_LAYERS / 4] = 0x300;                  /* the layer off */
    assert(!video_compose(&st, W, H, out));
    printf("ok\n");
    return 0;
}
