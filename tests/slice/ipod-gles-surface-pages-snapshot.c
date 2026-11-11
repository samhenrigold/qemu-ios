/* A snapshot keeps each surface's page list (v7), so a restored surface's frames are written back to its
 * pages as before the save, not through the MMU of whatever process flushes them: CoreAnimation binds the
 * surface it composites into once, and the restored iPod touch 2G kept showing the frame it was saved with.
 * The list is input like the rest of the file: one longer or shorter than the surface's rows span, or with a
 * page that isn't RAM, fails the load instead of indexing past it at the next write-back.
 *
 * SLICE hw/arm/gles-host-snapshot.c.inc range typedef struct { GByteArray | /* ---- per-context GL state
 * SLICE hw/arm/gles-host.c fn gles_surface_range gles_surface_npages
 * SLICE hw/arm/gles-host-snapshot.c.inc fn snap_put_pages snap_get_pages
 * PKG glib-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>

#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1u << TARGET_PAGE_BITS)
#define TARGET_PAGE_MASK (~(uint64_t)(TARGET_PAGE_SIZE - 1))
typedef uint64_t ram_addr_t;
typedef struct {
    uint32_t base, stride, width, height, format, uv, uvstride;
    unsigned target;
    bool window, dirty;
    uint64_t gen;
    unsigned npages;
    bool detached;
    ram_addr_t pages[];
} GLESSurface;
static unsigned gles_surface_bpp(uint32_t fmt) { return fmt == 565 ? 2 : 4; }
static ram_addr_t not_ram = 1;          /* a page snap_page_ram refuses */
static bool snap_page_ram(ram_addr_t ram) { return ram != not_ram && !(ram & (TARGET_PAGE_SIZE - 1)); }

#include "slice.h"

/* A 320x480 BGRA framebuffer surface at 0x0f800000 (stride 1280): 150 pages. */
static GLESSurface *framebuffer(unsigned npages)
{
    GLESSurface *s = g_malloc0(sizeof(GLESSurface) + npages * sizeof(ram_addr_t));
    s->base = 0x0f800000; s->stride = 1280; s->width = 320; s->height = 480; s->format = 32;
    s->npages = npages;
    for (unsigned i = 0; i < npages; i++) s->pages[i] = 0x8000000 + (ram_addr_t)i * TARGET_PAGE_SIZE;
    return s;
}

/* Saves one surface as `saved`, loads it into a table holding it as the surfaces map leaves it (no pages). */
static GLESSurface *roundtrip(GLESSurface *saved, bool *bad)
{
    GHashTable *t = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    g_hash_table_insert(t, GUINT_TO_POINTER(7), saved);
    SnapW w = { g_byte_array_new() };
    snap_put_pages(&w, t);
    GLESSurface *bare = framebuffer(0);
    g_hash_table_replace(t, GUINT_TO_POINTER(7), bare);
    SnapR r = { w.b->data, w.b->len, 0, false };
    snap_get_pages(&r, t);
    *bad = r.bad || r.off != r.n;
    GLESSurface *in = g_hash_table_lookup(t, GUINT_TO_POINTER(7));
    GLESSurface *got = g_memdup2(in, sizeof(GLESSurface) + in->npages * sizeof(ram_addr_t));
    g_byte_array_free(w.b, true);
    g_hash_table_destroy(t);
    return got;
}

int main(void)
{
    bool bad;
    assert(gles_surface_npages(0x0f800000, 1280, 320, 480, 4, false) == 150);
    GLESSurface *got = roundtrip(framebuffer(150), &bad);
    assert(!bad && got->npages == 150 && got->pages[0] == 0x8000000 &&
           got->pages[149] == 0x8000000 + 149 * (ram_addr_t)TARGET_PAGE_SIZE && got->base == 0x0f800000);
    g_free(got);
    /* An untracked surface (npages 0) stays untracked. */
    got = roundtrip(framebuffer(0), &bad);
    assert(!bad && got->npages == 0);
    g_free(got);
    /* One page short, or one too many, of what the rows span. */
    GLESSurface *s = framebuffer(149);
    g_free(roundtrip(s, &bad));
    assert(bad);
    s = framebuffer(151);
    g_free(roundtrip(s, &bad));
    assert(bad);
    /* A page that isn't RAM. */
    s = framebuffer(150);
    not_ram = s->pages[75];
    got = roundtrip(s, &bad);
    assert(bad && got->npages == 0);
    g_free(got);
    return 0;
}
