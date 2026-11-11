/* A UI layer's stride and source rectangle, as AppleDisplayPipe programs them for a tiled surface and a linear one.
 *
 * FORMAT bit 0 says linear; clear, the layer is tiled and STRIDE holds bytes per row << 4 (a row of tiles, 16 rows),
 * as 5.1.1's program_hardware writes it (9B206 0x80878c84) for CoreAnimation's tiled surfaces (n90ap 9B206's 640x960
 * at 2560 bytes a row: 0xa002). A linear layer's STRIDE is its bytes per row. A tiled layer's source origin is in
 * SRC_ORIGIN, the address the surface's own; SRC_END is the source rectangle's far corner (origin plus size).
 *
 * Mutations (named, must fail this test): scanout_layer's old guess (a stride whose sixteenth still holds a row is
 * taken as << 4) instead of the format's bit; SRC_ORIGIN ignored (the layer read from the surface's 0,0, SRC_END as
 * its size).
 *
 * SLICE:types hw/arm/s5l8930_display.c define (DP_LAYERS|DP_UI_BASE|DP_UI_FORMAT|DP_UI_FORMAT_LINEAR|DP_UI_ADDR|DP_UI_STRIDE|DP_UI_UNSCALED|DP_UI_SRC_ORIGIN|DP_UI_DST_ORIGIN|DP_UI_SRC_END|DP_UI_DST_END)[[:space:](]
 * SLICE:types hw/arm/s5l8930_display.c typedef UILayer
 * SLICE hw/arm/s5l8930_display.c fn scanout_layer layer_row_bytes layer_fetch layers_fetch layer_row layers video_compose blend front_latch
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 32
#define H 24
#define SURF 0x1000u
#define lduw_le_p(p) ((uint16_t)(((const uint8_t *)(p))[0] | ((const uint8_t *)(p))[1] << 8))
#define trace_event_get_state_backends(e) 0
#define TRACE_PRINTF(...) do { } while (0)

typedef struct { uint32_t regs[0x6000 / 4]; uint32_t swap_id; } DisplayPipe;
#include "types.h"

typedef struct S5L8930DisplayState {
    bool h5p;
    DisplayPipe pipe[1];
    uint8_t *front, *front_next;
    size_t front_size;
    uint32_t front_key[4];
    bool front_valid;
    uint64_t front_gen;
    uint8_t *src, *src_next;
    size_t src_size, src_len;
    UILayer src_layers[2];
} S5L8930DisplayState;

static uint8_t ram[0x40000];

static void panel_size(S5L8930DisplayState *s, unsigned *w, unsigned *h) { *w = W; *h = H; }
static void fb_read(S5L8930DisplayState *s, uint32_t va, uint8_t *dst, unsigned len, const uint32_t *tables)
{
    assert(va + len <= sizeof(ram));
    memcpy(dst, ram + va, len);
}
static uint32_t panel_word(const DisplayPipe *p, uint64_t addr, uint32_t v) { return v; }

#include "slice.h"

/* The surface's pixel (x, y): its row at `pitch` bytes, as the CPU and the GL bridge write it. */
static uint32_t px(unsigned x, unsigned y) { return 0xff000000u | y << 12 | x; }

static void fill(unsigned w, unsigned h, unsigned pitch)
{
    memset(ram, 0, sizeof(ram));
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            memcpy(ram + SURF + y * pitch + x * 4, &(uint32_t){ px(x, y) }, 4);
}

/* The panel shows the surface's (sx, sy, w, h) at the panel's (dx, dy), black elsewhere. */
static void shown(S5L8930DisplayState *s, unsigned sx, unsigned sy, unsigned w, unsigned h, unsigned dx, unsigned dy)
{
    front_latch(s);
    assert(s->front_valid);
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            uint32_t got;
            bool in = x >= dx && x < dx + w && y >= dy && y < dy + h;
            memcpy(&got, s->front + (y * W + x) * 4, 4);
            assert(got == (in ? px(sx + x - dx, sy + y - dy) : 0));
        }
}

int main(void)
{
    static S5L8930DisplayState st;
    S5L8930DisplayState *s = &st;
    uint32_t *r = s->pipe[0].regs, *ui = r + DP_UI_BASE(0) / 4;

    r[DP_LAYERS / 4] = 0x100;

    /* A linear 16x8 overlay out of a surface 1024 bytes a row (256 pixels), at (8, 4): its stride's sixteenth is
     * its own row (64 bytes), which the old guess took for a tiled stride. */
    fill(256, 8, 1024);
    ui[DP_UI_FORMAT / 4] = DP_UI_FORMAT_LINEAR;
    ui[DP_UI_ADDR / 4] = SURF;
    ui[DP_UI_STRIDE / 4] = 1024 | 2;
    ui[DP_UI_SRC_END / 4] = 16 << 16 | 8;
    ui[DP_UI_DST_ORIGIN / 4] = 8 << 16 | 4;
    ui[DP_UI_DST_END / 4] = 24 << 16 | 12;
    shown(s, 0, 0, 16, 8, 8, 4);

    /* Tiled, the whole panel: 32 pixels at 256 bytes a row (one tile across), STRIDE 256 << 4. */
    fill(W, H, 256);
    ui[DP_UI_FORMAT / 4] = 0;
    ui[DP_UI_STRIDE / 4] = 256 << 4 | 2;
    ui[DP_UI_SRC_END / 4] = W << 16 | H;
    ui[DP_UI_DST_ORIGIN / 4] = 0;
    ui[DP_UI_DST_END / 4] = W << 16 | H;
    shown(s, 0, 0, W, H, 0, 0);

    /* Tiled, a 12x10 crop of it from (5, 7), placed at (3, 2): the address stays the surface's. */
    ui[DP_UI_SRC_ORIGIN / 4] = 5 << 16 | 7;
    ui[DP_UI_SRC_END / 4] = 17 << 16 | 17;
    ui[DP_UI_DST_ORIGIN / 4] = 3 << 16 | 2;
    ui[DP_UI_DST_END / 4] = 15 << 16 | 12;
    shown(s, 5, 7, 12, 10, 3, 2);
    puts("PASS: a linear overlay at a stride 16 rows long; a tiled layer's stride (<< 4) and its source origin");
    return 0;
}
