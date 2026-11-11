/* glGetFramebufferAttachmentParameteriv: every board but the A6 answers the bound renderbuffer, whatever was asked,
 * as qualified; the A6 (gles_a6) reports the CA drawable's own renderbuffer and the host's answer otherwise.
 * SLICE hw/arm/gles-host.c range case GLES_SLOT_GET_FB_ATTACH_PARAM: | int64_t r;
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GL_FRAMEBUFFER_EXT 0x8D40
#define GL_COLOR_ATTACHMENT0_EXT 0x8CE0
#define GL_RENDERBUFFER_EXT 0x8D41
#define GL_TEXTURE 0x1702
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT 0x8CD1
#define GL_INVALID_OPERATION 0x0502
#define GL_INVALID_ENUM 0x0500
#define GLES_SLOT_GET_FB_ATTACH_PARAM 1
typedef int GLint;
typedef struct { int unused; } CPUState;
static struct { uint32_t bound_framebuffer, bound_renderbuffer; GHashTable *fbo_drawable; } gh;
static bool gles_a6;
static uint32_t written;
static int64_t rejected;
static int gles_guest_rw(CPUState *cpu, uint32_t va, uint8_t *buf, size_t len, int write)
{
    assert(write && len == 4);
    memcpy(&written, buf, 4);
    return 0;
}
static int64_t gles_reject(unsigned e) { rejected = e; return -1; }
static bool gles_is_drawable(uint32_t fb) { return g_hash_table_contains(gh.fbo_drawable, GUINT_TO_POINTER(fb)); }
static void glGetFramebufferAttachmentParameterivEXT(unsigned t, unsigned a, unsigned p, GLint *v)
{
    *v = p == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT ? GL_TEXTURE : 99;   /* the host's private surrogate */
}
static int64_t call(uint32_t pname)
{
    CPUState state, *cpu = &state;
    uint32_t a[] = { GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, pname, 0x1000 };
    written = 0;
    rejected = 0;
    switch (GLES_SLOT_GET_FB_ATTACH_PARAM) {
#include "slice.h"
        return -2; /* closes the slice's trailing `default: {` */
        }
    }
    return -2;
}
int main(void)
{
    gh.fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);
    gh.bound_renderbuffer = 7;
    g_hash_table_insert(gh.fbo_drawable, GUINT_TO_POINTER(3), GUINT_TO_POINTER(5));
    /* Off the A6: the bound renderbuffer, framebuffer 0 and the drawable alike, as before */
    gh.bound_framebuffer = 0;
    assert(call(GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT) == 0 && written == 7);
    gh.bound_framebuffer = 3;
    assert(call(GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT) == 0 && written == 7);
    assert(call(GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT) == 0 && written == 7);
    /* The A6: the drawable's renderbuffer, its type, and framebuffer 0 refused */
    gles_a6 = true;
    assert(call(GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT) == 0 && written == GL_RENDERBUFFER_EXT);
    assert(call(GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT) == 0 && written == 5);
    gh.bound_framebuffer = 0;
    assert(call(GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT) == -1 && rejected == GL_INVALID_OPERATION);
    puts("PASS: the attachment query answers as qualified off the A6, and the drawable's renderbuffer on it");
    return 0;
}
