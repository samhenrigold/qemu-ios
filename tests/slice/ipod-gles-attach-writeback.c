/* A surface drawn into and then swapped out of its framebuffer's color attachment is still written back
 * at the frame's flush: the attachment handlers mark the outgoing surface dirty before they replace it.
 * Without the mark the flush's sync sees only the new attachment, and the old surface's guest memory
 * keeps its stale pixels. Adapted from blake-lucas/qemu-ios b0d0c475ee's surface-attachments test.
 *
 * SLICE hw/arm/gles-host.c define GLES_PRIVATE_NAME
 * SLICE hw/arm/gles-host.c fn gles_guest_name_ok gles_sync_surface_1
 * SLICE:attach hw/arm/gles-host.c range     case GLES_SLOT_FB_RENDERBUFFER: { |     case GLES_SLOT_DELETE_RENDERBUFFERS:
 * PKG glib-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

typedef unsigned GLuint;
typedef unsigned GLenum;
typedef int GLint;
typedef struct CPUState CPUState;
enum {
    GL_FRAMEBUFFER_EXT = 1, GL_COLOR_ATTACHMENT0_EXT, GL_DEPTH_ATTACHMENT_EXT,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT,
    GL_TEXTURE, GL_RENDERBUFFER_EXT,
    GLES_SURFACE_RGBA16F, GLES_SURFACE_RGB555_BE, GLES_SURFACE_ARGB64, GLES_SURFACE_AG32, GLES_SURFACE_NV12,
    GLES_SURFACE_BGRA32, GLES_SURFACE_RGBA32, GLES_SURFACE_RGB565, GLES_SURFACE_RGB555,
    GLES_SLOT_FB_RENDERBUFFER, GLES_SLOT_FB_TEXTURE_2D, GL_INVALID_OPERATION,
};
typedef struct GLESSurface { bool dirty, detached; unsigned format; } GLESSurface;
static struct { unsigned bound_framebuffer; bool fb_dirty; GHashTable *surfaces, *rb_sized; } gh;

static GLESSurface surf[3];             /* textures 1 and 2 are surfaces */
static unsigned color;                  /* the color attachment: a texture name, 0 for a renderbuffer */
static bool drawable;
static unsigned refusals;

static void glGetFramebufferAttachmentParameterivEXT(GLenum t, GLenum point, GLenum pname, GLint *v)
{
    assert(t == GL_FRAMEBUFFER_EXT && point == GL_COLOR_ATTACHMENT0_EXT);
    *v = pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT ? (color ? GL_TEXTURE : GL_RENDERBUFFER_EXT)
                                                            : (GLint)color;
}
static bool gles_refuse(const char *fmt, ...) { refusals++; return true; }
static int64_t gles_reject(GLenum e) { return -1; }
static char *gles_fourcc(unsigned f, char *s) { strcpy(s, "bad"); return s; }
static void gles_set_drawable(unsigned fb, bool v) { drawable = v; if (v) color = 0; }
static bool gles_is_drawable(unsigned fb) { return drawable; }
static void gles_trace_attach(const char *n, unsigned p, unsigned o) {}
static void glFramebufferTexture2DEXT(GLenum t, GLenum p, GLenum tt, GLuint name, int lvl)
{ if (p == GL_COLOR_ATTACHMENT0_EXT) color = name; }
static void glFramebufferRenderbufferEXT(GLenum t, GLenum p, GLenum rt, GLuint name)
{ if (p == GL_COLOR_ATTACHMENT0_EXT) color = 0; }

#define TRACE_PRINTF(...) ((void)0)
static void gles_record_drawable_attachment(const uint32_t *a) { (void)a; }
static bool gles_a6;   /* the attach path is the same on every board; only the native record is the A6's */
#include "slice.h"
static int64_t gles_sync_surface(CPUState *cpu) { return gles_sync_surface_1(cpu); }

static int64_t call(uint32_t slot, const uint32_t *a)
{
    CPUState *cpu = NULL;
    switch (slot) {
#include "attach.h"
    default: assert(0);
    }
    return 0;
}

static void reset(void)
{
    memset(surf, 0, sizeof surf);
    g_hash_table_remove_all(gh.surfaces);
    g_hash_table_remove_all(gh.rb_sized);
    gh.bound_framebuffer = 7; gh.fb_dirty = false; color = 1; drawable = false; refusals = 0;
    for (unsigned i = 1; i <= 2; i++) {
        surf[i].format = GLES_SURFACE_BGRA32;
        g_hash_table_insert(gh.surfaces, GUINT_TO_POINTER(i), &surf[i]);
    }
}

int main(void)
{
    gh.surfaces = g_hash_table_new(g_direct_hash, g_direct_equal);
    gh.rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
    const uint32_t tex2[] = { GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE, 2, 0 };
    const uint32_t rb[] = { GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, 10, 0 };

    /* surface 1 drawn into, then texture 2 attached: 1 is marked, 2 isn't yet, and the flush's own
     * sync then marks 2 */
    reset();
    assert(call(GLES_SLOT_FB_TEXTURE_2D, tex2) == 0);
    assert(color == 2 && surf[1].dirty && !surf[2].dirty && gh.fb_dirty);
    assert(gles_sync_surface(NULL) == 0 && surf[2].dirty);

    /* surface 1 swapped for the CA drawable (a renderbuffer without storage) */
    reset();
    assert(call(GLES_SLOT_FB_RENDERBUFFER, rb) == 0 && surf[1].dirty && drawable);

    /* ... or for a sized renderbuffer, which is attached for real */
    reset();
    g_hash_table_add(gh.rb_sized, GUINT_TO_POINTER(10));
    assert(call(GLES_SLOT_FB_RENDERBUFFER, rb) == 0 && surf[1].dirty && !drawable && color == 0);

    for (uint32_t slot = GLES_SLOT_FB_RENDERBUFFER; slot <= GLES_SLOT_FB_TEXTURE_2D; slot++) {
        const uint32_t *a = slot == GLES_SLOT_FB_TEXTURE_2D ? tex2 : rb;
        /* a surface the bridge can't write back refuses the change and leaves the attachment */
        reset(); surf[1].format = 0;
        assert(call(slot, a) == -1 && refusals == 1 && color == 1 && !gh.fb_dirty);
        /* a depth attachment doesn't touch the color surface */
        reset();
        uint32_t depth[5]; memcpy(depth, a, sizeof depth); depth[1] = GL_DEPTH_ATTACHMENT_EXT;
        assert(call(slot, depth) == 0 && !surf[1].dirty && color == 1);
    }

    /* a detached surface's texture stops being a surface; the default framebuffer has none */
    reset(); surf[1].detached = true;
    assert(call(GLES_SLOT_FB_TEXTURE_2D, tex2) == 0 && !g_hash_table_contains(gh.surfaces, GUINT_TO_POINTER(1)));
    reset(); gh.bound_framebuffer = 0;
    assert(call(GLES_SLOT_FB_TEXTURE_2D, tex2) == 0 && !surf[1].dirty);

    puts("PASS: a surface swapped out of its framebuffer's color attachment is marked for writeback");
    return 0;
}
