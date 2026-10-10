/* The front end's layer bind and present (fe_bind_layer, fe_present) against a CoreAnimation drawable double: binding
 * the same layer again while CA still holds both of its buffers succeeds at the size they have, the frames until a
 * buffer comes free are dropped, and the first free one is rendered into (Contre Jour 1.01 rebinding its layer on
 * 4.2.1). A new layer with no buffer still fails.
 *
 * SLICE contrib/gles-public/opengles.c fn fe_preflight fe_bind_layer fe_present
 * CFLAGS -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GLES_OP_DRAWABLE_STORAGE 0x1008
#define CA_FOURCC_BGRA 0x42475241
#define A(...) (const unsigned[]){__VA_ARGS__}
typedef struct { unsigned host; } GuestGC;
typedef struct {
    void *gc, *drawable, *block[8], *ref;
    unsigned base, stride, width, height, format;
    unsigned char need_buffer;
} ca_view_t;
typedef int (*ca_bind_fn)(void *drawable, unsigned fourcc, void **block);
static ca_view_t view;
static unsigned char pixels[960 * 4 * 2];
static unsigned free_buffers = 1, detaches, refusals, storage_w, storage_h, presented;
static ca_view_t *ca_view_for_gc(void *gc, int create) { return &view; }
static void ca_detach_view(ca_view_t *v) { detaches++; memset(v, 0, sizeof *v); }
static void iosurface_init(void) {}
static int ca_create_buffer(void *ctx, void *surface) { return 1; }
static int ca_destroy_buffer(void *ctx, void *surface) { return 1; }
static void refused(const char *a, const char *b, unsigned n) { refusals++; }
/* mbxshim's: one CA nextBuffer a frame; a free buffer is captured at 960x640. */
static int ca_next_buffer(ca_view_t *v)
{
    if (!v->need_buffer) return v->ref != 0;
    if (!free_buffers) return 0;
    free_buffers--;
    v->need_buffer = 0; v->ref = (void *)1; v->base = (unsigned)(unsigned long)pixels;
    v->width = 960; v->height = 640; v->stride = 0;
    return 1;
}
static int GLESPresentView(void *gc, void *view_) { assert(view.ref); presented++; view.need_buffer = 1; return 1; }
static long long qc(unsigned op, void *gc, unsigned argc, const unsigned *a)
{ assert(op == GLES_OP_DRAWABLE_STORAGE && argc == 2); storage_w = a[0]; storage_h = a[1]; return 0; }
static int bind(void *drawable, unsigned fourcc, void **block) { assert(fourcc == CA_FOURCC_BGRA); return 1; }
#include "slice.h"
int main(void)
{
    GuestGC gc = {1};
    void *layer[5] = {(void *)1, (void *)bind}, *other[5] = {(void *)1, (void *)bind};   /* version 1 closures */
    assert(fe_bind_layer(&gc, layer) && storage_w == 960 && storage_h == 640);
    assert(fe_present(&gc) && presented == 1);
    /* The same layer again, both buffers still out: the bind keeps their size, and frames wait for one. */
    storage_w = 0;
    assert(fe_bind_layer(&gc, layer) && !refusals && storage_w == 960 && storage_h == 640 && view.drawable == layer);
    assert(fe_present(&gc) && fe_present(&gc) && presented == 1);
    free_buffers = 1;
    assert(fe_present(&gc) && presented == 2);
    /* Another layer with nothing to give fails, as before. */
    detaches = 0;
    assert(!fe_bind_layer(&gc, other) && refusals == 1 && detaches == 2 && !view.drawable);
    puts("PASS: a rebound layer waits for its next free buffer at the next frame; a new layer with none still fails");
}
