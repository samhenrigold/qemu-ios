/* A UI layer leaves an iPhone app's compatibility window for the whole panel again (LightTouchMac issue 87).
 *
 * 3.2's AppleDisplayPipe places a layer off 0,0 with DST_ORIGIN and DST_END (UNSCALED 0) and a layer at 0,0 with
 * UNSCALED 1 alone, leaving both corners as they were. The sequence below is what it programs for SpringBoard's
 * full-screen layer, then a 1x iPhone app centered and scaled 2x, then SpringBoard again: the panel must show the
 * whole layer, not the whole layer squeezed into the app's window. 4.x programs UNSCALED 0 and both corners on every
 * swap, and that must keep working.
 *
 * Mutation (named, must fail this test): drop the UNSCALED check in scanout_layer.
 *
 * SLICE:types hw/arm/s5l8930_display.c define (DP_LAYERS|DP_UI_BASE|DP_UI_FORMAT|DP_UI_ADDR|DP_UI_STRIDE|DP_UI_UNSCALED|DP_UI_DST_ORIGIN|DP_UI_SRC_END|DP_UI_DST_END|DP_UI_FORMAT_LINEAR|DP_UI_SRC_ORIGIN)[[:space:](]
 * SLICE:types hw/arm/s5l8930_display.c typedef UILayer
 * SLICE hw/arm/s5l8930_display.c fn scanout_layer layer_row_bytes layer_fetch layers_fetch layer_row layers video_compose blend front_latch
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Strides are whole 64-byte units (the pipe drops the low 6 bits), so the app is 16 pixels wide. */
#define W 64
#define H 48
#define HOME 0x1000u
#define APP 0xc000u
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

static uint8_t ram[0x10000];

static void panel_size(S5L8930DisplayState *s, unsigned *w, unsigned *h) { *w = W; *h = H; }
static void fb_read(S5L8930DisplayState *s, uint32_t va, uint8_t *dst, unsigned len, const uint32_t *tables)
{
    assert(va + len <= sizeof(ram));
    memcpy(dst, ram + va, len);
}
static uint32_t panel_word(const DisplayPipe *p, uint64_t addr, uint32_t v) { return v; }

#include "slice.h"

static uint32_t *ui;

/* One swap's UI0 window packets, as the kernel writes them. */
static void swap(uint32_t base, unsigned sw, unsigned sh, bool unscaled, uint32_t org, uint32_t end)
{
    ui[DP_UI_ADDR / 4] = base;
    ui[DP_UI_STRIDE / 4] = sw * 4;
    ui[DP_UI_FORMAT / 4] = DP_UI_FORMAT_LINEAR;
    ui[DP_UI_SRC_END / 4] = sw << 16 | sh;
    ui[DP_UI_UNSCALED / 4] = unscaled;
    if (!unscaled) {
        ui[DP_UI_DST_ORIGIN / 4] = org;
        ui[DP_UI_DST_END / 4] = end;
    }
}

/* The panel shows the whole HOME buffer, pixel for pixel. */
static void home_shown(S5L8930DisplayState *s)
{
    front_latch(s);
    assert(s->front_valid && !memcmp(s->front, ram + HOME, W * H * 4));
}

int main(void)
{
    static S5L8930DisplayState st;
    S5L8930DisplayState *s = &st;
    uint32_t *r = s->pipe[0].regs;

    ui = r + DP_UI_BASE(0) / 4;
    for (unsigned i = 0; i < sizeof(ram); i++) {
        ram[i] = (i & 3) == 3 ? 0xff : i * 7 + 3;
    }
    r[DP_LAYERS / 4] = 0x100;

    /* 3.2: Home at 0,0, then a 16x12 app scaled 2x into the middle (16,12)-(48,36), then Home again. */
    swap(HOME, W, H, true, 0, 0);
    home_shown(s);
    swap(APP, 16, 12, false, 16 << 16 | 12, 48 << 16 | 36);
    front_latch(s);
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x++) {
            const uint8_t *p = s->front + (y * W + x) * 4;
            bool in = x >= 16 && x < 48 && y >= 12 && y < 36;
            uint32_t want = in ? APP + ((y - 12) / 2 * 16 + (x - 16) / 2) * 4 : 0;
            assert(in ? !memcmp(p, ram + want, 4) : !memcmp(p, "\0\0\0\0", 4));
        }
    }
    swap(HOME, W, H, true, 0, 0);
    home_shown(s);

    /* 4.x: the corners on every swap, UNSCALED 0. */
    swap(APP, 16, 12, false, 16 << 16 | 12, 48 << 16 | 36);
    front_latch(s);
    swap(HOME, W, H, false, 0, W << 16 | H);
    home_shown(s);

    /* H5P uses +60/+70 for source corners, +54/+58 for destination, and
     * seven stride flag bits. +4c bit0 is no longer H3's UNSCALED switch. */
    s->h5p = true;
    memset(ui, 0, 0x100);
    ui[DP_UI_ADDR / 4] = HOME;
    ui[DP_UI_FORMAT / 4] = 0x1001;
    ui[DP_UI_STRIDE / 4] = W * 4 | 0x43;
    ui[0x4c / 4] = 1;
    ui[0x60 / 4] = 8 << 16 | 4;
    ui[0x70 / 4] = 24 << 16 | 16;
    ui[0x54 / 4] = 16 << 16 | 12;
    ui[0x58 / 4] = 48 << 16 | 36;
    front_latch(s);
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x++) {
            bool in = x >= 16 && x < 48 && y >= 12 && y < 36;
            const uint8_t *pixel = s->front + (y * W + x) * 4;
            uint32_t want = HOME + ((4 + (y - 12) / 2) * W + 8 + (x - 16) / 2) * 4;
            assert(in ? !memcmp(pixel, ram + want, 4) : !memcmp(pixel, "\0\0\0\0", 4));
        }
    }
    ui[0x60 / 4] = 0x1000 << 16;
    ui[0x70 / 4] = 0x1010 << 16 | 12;
    UILayer layer;
    assert(scanout_layer(s, 0, W, H, &layer) && layer.sx == 0x1000 && layer.sw == 16);
    return 0;
}
