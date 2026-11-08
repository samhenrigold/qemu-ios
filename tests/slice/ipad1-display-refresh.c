/* The A4 display's refresh shows the latched frame while its layers and size still hold, whatever their bases.
 *
 * CoreAnimation double-buffers UI1: once a swap latches, the next is queued with the other buffer's base, so a
 * refresh that matched the bases composed both layers live (under the BQL) every time, showing a frame the panel
 * hadn't latched yet. A layer turned on or off without a swap still shows at once, from the live layers.
 *
 * Mutations (named, each must fail this test): display_update compares the whole key (the bases too); it compares
 * key[0] alone (not which layers are on).
 *
 * SLICE:types hw/arm/s5l8930_display.c define (DP_LAYERS|DP_UI_BASE|DP_UI_FORMAT|DP_UI_ADDR|DP_UI_STRIDE|DP_UI_UNSCALED|DP_UI_DST_ORIGIN|DP_UI_SRC_END|DP_UI_DST_END|DP_UI_FORMAT_LINEAR|DP_UI_SRC_ORIGIN)[[:space:](]
 * SLICE:types hw/arm/s5l8930_display.c typedef UILayer
 * SLICE hw/arm/s5l8930_display.c fn scanout_layer layer_row_bytes layer_fetch layers_fetch layer_row layers video_compose blend compose front_latch gamma_row display_update
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 16
#define H 8
#define A 0x1000u
#define B 0x2000u
#define lduw_le_p(p) ((uint16_t)(((const uint8_t *)(p))[0] | ((const uint8_t *)(p))[1] << 8))
#define trace_event_get_state_backends(e) 0
#define TRACE_PRINTF(...) do { } while (0)

typedef struct { uint32_t regs[0x6000 / 4]; uint32_t swap_id; } DisplayPipe;
#include "types.h"

typedef struct { uint8_t data[W * H * 4]; } DisplaySurface;
typedef struct { DisplaySurface surface; } QemuConsole;

typedef struct S5L8930DisplayState {
    bool h5p;
    DisplayPipe pipe[1];
    QemuConsole *con;
    uint8_t *front, *front_next;
    size_t front_size;
    uint32_t front_key[4];
    bool front_valid;
    uint64_t front_gen;
    uint8_t *src, *src_next;
    size_t src_size, src_len;
    UILayer src_layers[2];
    uint8_t gamma8[3][256];
    bool gamma_identity;
    uint64_t shown_gen;
    DisplaySurface *shown_surface;
    int shown;
} S5L8930DisplayState;

static uint8_t ram[0x10000];
static unsigned updates;

static void panel_size(S5L8930DisplayState *s, unsigned *w, unsigned *h) { *w = W; *h = H; }
static void fb_read(S5L8930DisplayState *s, uint32_t va, uint8_t *dst, unsigned len, const uint32_t *tables)
{
    assert(va + len <= sizeof(ram));
    memcpy(dst, ram + va, len);
}
static uint32_t panel_word(const DisplayPipe *p, uint64_t addr, uint32_t v) { return v; }
static DisplaySurface *qemu_console_surface(QemuConsole *c) { return &c->surface; }
static int surface_width(DisplaySurface *d) { return W; }
static int surface_height(DisplaySurface *d) { return H; }
static int surface_bits_per_pixel(DisplaySurface *d) { return 32; }
static int surface_stride(DisplaySurface *d) { return W * 4; }
static uint8_t *surface_data(DisplaySurface *d) { return d->data; }
static void qemu_console_resize(QemuConsole *c, int w, int h) { abort(); }
static void dpy_gfx_update(QemuConsole *c, int x, int y, int w, int h) { updates++; }

#include "slice.h"

/* UI layer `l` on, opaque BGRA at `base`, the whole panel. */
static void layer(uint32_t *r, unsigned l, uint32_t base)
{
    r[(DP_UI_BASE(l) + DP_UI_ADDR) / 4] = base;
    r[(DP_UI_BASE(l) + DP_UI_STRIDE) / 4] = W * 4;
    r[(DP_UI_BASE(l) + DP_UI_FORMAT) / 4] |= DP_UI_FORMAT_LINEAR;
    r[(DP_UI_BASE(l) + DP_UI_SRC_END) / 4] = W << 16 | H;
}

int main(void)
{
    static QemuConsole con;
    static S5L8930DisplayState st = { .con = &con, .gamma_identity = true };
    S5L8930DisplayState *s = &st;
    uint32_t *r = s->pipe[0].regs;

    for (unsigned i = 0; i < W * H * 4; i++) {
        ram[A + i] = i % 4 == 3 ? 0xff : 0x11;
        ram[B + i] = i % 4 == 3 ? 0xff : 0x22;
        ram[0x4000 + i] = i % 4 == 3 ? 0xff : 0x33;
    }
    /* UI1's buffer A latched by a swap and shown. */
    r[DP_LAYERS / 4] = 0x200;
    layer(r, 1, A);
    front_latch(s);
    display_update(s);
    assert(updates == 1 && !memcmp(con.surface.data, ram + A, W * H * 4));

    /* The next swap queued with buffer B, not latched yet: the refresh keeps showing A, and draws nothing new. */
    layer(r, 1, B);
    display_update(s);
    assert(updates == 1 && !memcmp(con.surface.data, ram + A, W * H * 4));

    /* The VBL latches B: shown at the next refresh. */
    front_latch(s);
    display_update(s);
    assert(updates == 2 && !memcmp(con.surface.data, ram + B, W * H * 4));

    /* UI1 turned off and UI0 on at the same size, without a swap: shown live at once. */
    r[DP_LAYERS / 4] = 0x100;
    layer(r, 0, 0x4000);
    display_update(s);
    assert(updates == 3 && !memcmp(con.surface.data, ram + 0x4000, W * H * 4));
    puts("PASS: refresh shows the latched frame across a queued base; a layer change shows live");
    return 0;
}
