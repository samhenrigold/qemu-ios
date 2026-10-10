/* The A4 display's latch composes the panel only when its layers or their pixels changed.
 *
 * A quiet panel relatches every VBL (front_latch), so an unchanged frame must leave the front alone without
 * composing it again, and any change to a layer's pixels or placement must still reach the front at the
 * next latch, as the Accessibility Zoom path needs.
 *
 * Mutation (named, must fail this test): drop the early return in front_latch (the compose runs every latch).
 *
 * SLICE:types hw/arm/s5l8930_display.c define (DP_LAYERS|DP_UI_BASE|DP_UI_FORMAT|DP_UI_ADDR|DP_UI_STRIDE|DP_UI_DST_ORIGIN|DP_UI_SRC_SIZE|DP_UI_DST_END)[[:space:](]
 * SLICE:types hw/arm/s5l8930_display.c typedef UILayer
 * SLICE hw/arm/s5l8930_display.c fn scanout_layer layer_row_bytes layer_fetch layers_fetch layer_row layers video_compose blend front_latch
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define W 16
#define H 8
#define BASE 0x1000u
#define lduw_le_p(p) ((uint16_t)(((const uint8_t *)(p))[0] | ((const uint8_t *)(p))[1] << 8))
#define trace_event_get_state_backends(e) 0
#define TRACE_PRINTF(...) do { } while (0)

typedef struct { uint32_t regs[0x6000 / 4]; uint32_t swap_id; } DisplayPipe;
#include "types.h"

typedef struct S5L8930DisplayState {
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
static unsigned reads;

/* The two stubs: a fixed panel, and guest memory at IOVA = offset into ram. */
static void panel_size(S5L8930DisplayState *s, unsigned *w, unsigned *h) { *w = W; *h = H; }
static void fb_read(S5L8930DisplayState *s, uint32_t va, uint8_t *dst, unsigned len)
{
    assert(va + len <= sizeof(ram));
    memcpy(dst, ram + va, len);
    reads++;
}
static uint32_t panel_word(const DisplayPipe *p, uint64_t addr, uint32_t v) { return v; }

#include "slice.h"

int main(void)
{
    static S5L8930DisplayState st;
    S5L8930DisplayState *s = &st;
    uint32_t *r = s->pipe[0].regs;
    uint8_t want[W * H * 4];

    for (unsigned i = 0; i < sizeof(ram); i++) {
        ram[i] = i * 7 + 3;
    }
    /* UI1 on, opaque BGRA at BASE, the whole panel, packed rows. */
    r[DP_LAYERS / 4] = 0x200;
    r[(DP_UI_BASE(1) + DP_UI_ADDR) / 4] = BASE;
    r[(DP_UI_BASE(1) + DP_UI_STRIDE) / 4] = W * 4;
    r[(DP_UI_BASE(1) + DP_UI_SRC_SIZE) / 4] = W << 16 | H;
    for (unsigned i = 0; i < W * H; i++) {
        ram[BASE + i * 4 + 3] = 0xff;
    }

    front_latch(s);
    assert(s->front_valid && s->front_gen == 1);
    assert(!memcmp(s->front, ram + BASE, sizeof(want)));

    /* Nothing changed: the front stays and nothing is composed into the scratch. */
    memset(s->front_next, 0x5a, sizeof(want));
    reads = 0;
    front_latch(s);
    assert(s->front_gen == 1 && reads == 1);
    for (unsigned i = 0; i < sizeof(want); i++) {
        assert(s->front_next[i] == 0x5a);
    }
    assert(!memcmp(s->front, ram + BASE, sizeof(want)));

    /* One pixel written in place (zoom: no swap): the next latch shows it. */
    ram[BASE + 5 * W * 4 + 9 * 4] ^= 0x80;
    front_latch(s);
    assert(s->front_gen == 2 && !memcmp(s->front, ram + BASE, sizeof(want)));

    /* Same bytes, the layer moved right by 4: composed again, columns 0-3 clear. */
    r[(DP_UI_BASE(1) + DP_UI_DST_ORIGIN) / 4] = 4 << 16;
    r[(DP_UI_BASE(1) + DP_UI_SRC_SIZE) / 4] = (W - 4) << 16 | H;
    r[(DP_UI_BASE(1) + DP_UI_STRIDE) / 4] = W * 4;
    front_latch(s);
    assert(s->front_gen == 3);
    memset(want, 0, sizeof(want));
    for (unsigned y = 0; y < H; y++) {
        memcpy(want + (y * W + 4) * 4, ram + BASE + y * W * 4, (W - 4) * 4);
    }
    assert(!memcmp(s->front, want, sizeof(want)));

    /* A padded layer reads row by row, and still skips when unchanged. */
    reads = 0;
    front_latch(s);
    assert(s->front_gen == 3 && reads == H);

    /* UI1 over UI0, both whole-panel BGRA: UI0 copied, UI1 opaque replaces it, half alpha blends
     * premultiplied source-over, zero alpha shows UI0. */
    const uint32_t base0 = 0x4000;
    r[DP_LAYERS / 4] = 0x300;
    r[(DP_UI_BASE(0) + DP_UI_ADDR) / 4] = base0;
    r[(DP_UI_BASE(0) + DP_UI_STRIDE) / 4] = W * 4;
    r[(DP_UI_BASE(1) + DP_UI_DST_ORIGIN) / 4] = 0;
    r[(DP_UI_BASE(1) + DP_UI_SRC_SIZE) / 4] = W << 16 | H;
    for (unsigned i = 0; i < W * H; i++) {
        ram[base0 + i * 4 + 3] = 0xff;
    }
    ram[BASE + 3] = 0x80;
    ram[BASE + 4 + 3] = 0;
    front_latch(s);
    assert(s->front_gen == 4);
    const uint8_t *f = s->front, *t = ram + BASE, *b = ram + base0;
    for (int c = 0; c < 3; c++) {
        unsigned v = t[c] + (b[c] * (255 - 0x80) + 127) / 255;
        assert(f[c] == (v > 255 ? 255 : v));
        assert(f[4 + c] == b[4 + c]);
        assert(f[8 + c] == t[8 + c]);
    }
    assert(f[3] == 0xff && !memcmp(f + 8, t + 8, (W * H - 2) * 4));

    /* The video layer alone, both UI layers off (7.1.2's full-screen movie): it still reaches the panel. A 4x2 NV12
     * source of luma 235, scaled to the whole panel with a matrix that gives each channel luma - 16. */
    const uint32_t vy = 0x8000, vuv = 0x8100;
    r[DP_LAYERS / 4] = 0x400;
    memset(ram + vy, 235, 32);
    memset(ram + vuv, 128, 16);
    r[0x307c / 4] = vy; r[0x3080 / 4] = vuv; r[0x3088 / 4] = r[0x308c / 4] = 16;
    r[0x3094 / 4] = 4 << 16 | 2; r[0x309c / 4] = W << 16 | H; r[0x30a0 / 4] = 0;
    for (int i = 0; i < 9; i++) {
        r[0x3024 / 4 + i] = i % 3 ? 0 : 0x1000;
    }
    front_latch(s);
    assert(s->front_valid && s->front_gen == 5);
    for (unsigned i = 0; i < W * H; i++) {
        assert(s->front[i * 4] == 219 && s->front[i * 4 + 1] == 219 && s->front[i * 4 + 2] == 219);
    }
    return 0;
}
