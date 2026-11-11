/* H5P disjoint UI regions cover a video plane without stretching region zero.
 * All register values and pixels here are synthetic.
 * SLICE:types hw/arm/s5l8930_display.c define DP_
 * SLICE:types hw/arm/s5l8930_display.c typedef UILayer
 * SLICE hw/arm/s5l8930_display.c fn scanout_layer layer_row_bytes layer_fetch layer_row blend
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define W 32
#define H 16
#define lduw_le_p(p)                                                           \
    ((uint16_t)(((const uint8_t *)(p))[0] | ((const uint8_t *)(p))[1] << 8))
typedef struct {
    uint32_t regs[0x6000 / 4];
} DisplayPipe;
typedef struct {
    bool h5p;
    DisplayPipe pipe[1];
} S5L8930DisplayState;
#include "types.h"
static uint8_t ram[0x4000];
static void fb_read(S5L8930DisplayState *s, uint32_t va, uint8_t *dst,
                    unsigned len, const uint32_t *tables)
{
    assert(va + len <= sizeof(ram));
    memcpy(dst, ram + va, len);
}
#include "slice.h"
static unsigned corner(unsigned x, unsigned y)
{
    return x << 16 | y;
}
int main(void)
{
    S5L8930DisplayState s = {.h5p = true};
    uint32_t *ui = s.pipe[0].regs + 0x5000 / 4;
    s.pipe[0].regs[DP_LAYERS / 4] = 0x200;
    ui[0x40 / 4] = 1;
    ui[0x44 / 4] = 0x1000;
    ui[0x48 / 4] = W * 4 | 3;
    ui[0x50 / 4] = ui[0x58 / 4] = corner(W, H);
    ui[0x5c / 4] = 0x107;
    ui[0x60 / 4] = corner(0, 0);
    ui[0x70 / 4] = corner(W, 2);
    ui[0x64 / 4] = corner(10, 6);
    ui[0x74 / 4] = corner(14, 8);
    ui[0x68 / 4] = corner(0, 14);
    ui[0x78 / 4] = corner(W, H);
    UILayer layers[2] = {0};
    assert(scanout_layer(&s, 1, W, H, &layers[1]));
    assert(layers[1].sw == W && layers[1].sh == H && layers[1].regions == 7);
    uint32_t background[W * H], out[W * H];
    uint8_t fetched[W * H * 4];
    bool on[2] = {false, true};
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x++) {
            /* An opaque marker outside the regions must NOT obscure video. */
            uint32_t pixel = 0xff800000u | (y << 8) | x;
            memcpy(ram + 0x1000 + (y * W + x) * 4, &pixel, 4);
            background[y * W + x] = 0xff0011cc;
        }
    }
    assert(layer_fetch(&s, &layers[1], fetched) == sizeof(fetched));
    blend(layers, on, fetched, W, H, out, background);
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x++) {
            bool covered =
                y < 2 || y >= 14 || (x >= 10 && x < 14 && y >= 6 && y < 8);
            assert(out[y * W + x] == (covered ? (0xff800000u | y << 8 | x)
                                              : background[y * W + x]));
        }
    }
    /* Single-region legacy programming keeps its source-rectangle behavior. */
    ui[0x5c / 4] = 0x101;
    assert(scanout_layer(&s, 1, W, H, &layers[1]));
    assert(!layers[1].regions && layers[1].sh == 2);
    /* Invalid multi-region geometry never becomes a full-screen scanout. */
    ui[0x5c / 4] = 0x103;
    ui[0x60 / 4] = ui[0x70 / 4];
    ui[0x64 / 4] = ui[0x74 / 4];
    assert(!scanout_layer(&s, 1, W, H, &layers[1]));
    puts(
        "ok: H5P multi-region UI over video; holes and legacy rectangles preserved");
}
