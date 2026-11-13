/* The drawable framebuffer is what the app set up, as on the device: the color format its layer asked for, and depth
 * and stencil only when the app attached them.
 *
 * The host drawable was RGBA8 with a private 24-bit depth and 8-bit stencil whatever the app attached or its layer's
 * kEAGLDrawablePropertyColorFormat said, so a 2D engine that left GL_DEPTH_TEST on with no depth buffer lost every
 * sprite drawn at the depth of the one before (the device passes every depth test without one), a stencil test ran
 * against a buffer the app never made, and GL_DST_ALPHA blending on an RGB565 layer read back the alpha written where
 * the device reads 1.0. Now the drawable's color is RGB for a 565 layer, and gh.fbo takes the named framebuffer's own
 * depth and stencil attachments, or none (framebuffer 0, an EGL surface, none).
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c range /* ---------------------------------------------------------- GPU memory | /*\n * Profiling and strictness
 * SLICE hw/arm/gles-host.c range #define GLES_PRIVATE_NAME | static bool gles_is_drawable(
 * SLICE hw/arm/gles-host.c fn gles_reject gles_texture_object gles_is_drawable gles_host_fbo gles_attachment gles_drawable_depth_stencil gles_set_drawable gles_texture_begin gles_drawable_storage gles_record_drawable_attachment
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
int gles_guest_rw(CPUState *cpu, vaddr a, void *p, size_t n, bool write) { return -1; }
static bool gles_a6;
#include "slice.h"

/* Two quads at one depth, red then green, over a black clear with alpha 0; the centre pixel. */
static void scene(uint8_t px[4])
{
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    for (unsigned i = 0; i < 2; i++) {
        glColor4f(!i, i, 0, 0.5f);
        glBegin(GL_QUADS);
        glVertex3f(-1, -1, 0); glVertex3f(1, -1, 0); glVertex3f(1, 1, 0); glVertex3f(-1, 1, 0);
        glEnd();
    }
    glDisable(GL_DEPTH_TEST);
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static GLint bits(GLenum pname)
{
    GLint v = -1;
    glGetIntegerv(pname, &v);
    return v;
}

/* glFramebufferRenderbuffer on the bound drawable framebuffer, as GLES_SLOT_FB_RENDERBUFFER takes it there. */
static void attach(GLenum point, GLuint rb)
{
    uint32_t a[] = { GL_FRAMEBUFFER_EXT, point, GL_RENDERBUFFER_EXT, rb };
    gles_record_drawable_attachment(a);
    gles_drawable_depth_stencil();
}

int main(void)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf; GLint count; CGLContextObj ctx;
    assert(CGLChoosePixelFormat(attrs, &pf, &count) == kCGLNoError);
    assert(CGLCreateContext(pf, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(pf); assert(CGLSetCurrentContext(ctx) == kCGLNoError);
    gh.rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
    gh.charged = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    gh.fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);
    uint8_t px[4];

    /* the app's framebuffer: a color renderbuffer CA gives storage to (the drawable), nothing else yet */
    GLuint fb, color, depth16, d24s8, stencil8;
    glGenFramebuffersEXT(1, &fb);
    glGenRenderbuffersEXT(1, &color);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    gh.bound_framebuffer = fb;
    assert(!gles_drawable_storage(64, 64, false));
    gles_set_drawable(fb, color);
    attach(GL_COLOR_ATTACHMENT0_EXT, color);
    assert(gles_is_drawable(fb) && bits(GL_FRAMEBUFFER_BINDING_EXT) == (GLint)gh.fbo);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);

    /* no depth attached: the depth test passes everything, so the second quad shows; RGBA keeps alpha */
    assert(bits(GL_DEPTH_BITS) == 0 && bits(GL_STENCIL_BITS) == 0 && bits(GL_ALPHA_BITS) == 8);
    scene(px);
    assert(px[0] == 0 && px[1] == 255 && px[3] == 128);

    /* a 16-bit depth renderbuffer attached: the test applies, and the first quad wins */
    glGenRenderbuffersEXT(1, &depth16);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, depth16);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT16, 64, 64);
    attach(GL_DEPTH_ATTACHMENT_EXT, depth16);
    assert(bits(GL_FRAMEBUFFER_BINDING_EXT) == (GLint)gh.fbo && bits(GL_DEPTH_BITS) == 16 && bits(GL_STENCIL_BITS) == 0);
    scene(px);
    assert(px[0] == 255 && px[1] == 0);

    /* packed depth and stencil on both points, then stencil alone (OES_stencil8) */
    glGenRenderbuffersEXT(1, &d24s8);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, d24s8);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, 64, 64);
    attach(GL_DEPTH_ATTACHMENT_EXT, d24s8);
    attach(GL_STENCIL_ATTACHMENT_EXT, d24s8);
    assert(bits(GL_DEPTH_BITS) == 24 && bits(GL_STENCIL_BITS) == 8);
    glGenRenderbuffersEXT(1, &stencil8);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, stencil8);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_STENCIL_INDEX8_EXT, 64, 64);
    attach(GL_DEPTH_ATTACHMENT_EXT, 0);
    attach(GL_STENCIL_ATTACHMENT_EXT, stencil8);
    assert(bits(GL_DEPTH_BITS) == 0 && bits(GL_STENCIL_BITS) == 8);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);

    /* framebuffer 0 (an EGL surface) has neither, and binding the app's again gives its own back */
    gh.bound_framebuffer = 0;
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gles_host_fbo(0));
    gles_drawable_depth_stencil();
    assert(bits(GL_STENCIL_BITS) == 0);
    gh.bound_framebuffer = fb;
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gles_host_fbo(fb));
    gles_drawable_depth_stencil();
    assert(bits(GL_STENCIL_BITS) == 8);

    /* a resize keeps them (the new gh.fbo takes the bound framebuffer's); a deleted one is gone, with no GL error */
    GLuint before = gh.fbo;
    assert(!gles_drawable_storage(32, 48, false) && gh.fbo != before && bits(GL_STENCIL_BITS) == 8);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glDeleteRenderbuffersEXT(1, &stencil8);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gh.fbo);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, 0);
    gles_drawable_depth_stencil();
    assert(bits(GL_STENCIL_BITS) == 0 && glGetError() == GL_NO_ERROR);

    /* a 565 layer: no alpha, so a read gives 255 and GL_DST_ALPHA blends with 1.0, as on the device */
    assert(!gles_drawable_storage(64, 64, true) && gh.drawable_rgb && bits(GL_ALPHA_BITS) == 0);
    scene(px);
    assert(px[1] == 255 && px[3] == 255);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_ALPHA, GL_ZERO);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    glDisable(GL_BLEND);
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    assert(px[0] == 255 && px[1] == 255 && px[2] == 255);
    /* the same size in RGBA again is a new drawable, with alpha */
    before = gh.fbo;
    assert(!gles_drawable_storage(64, 64, false) && gh.fbo != before && !gh.drawable_rgb && bits(GL_ALPHA_BITS) == 8);
    assert(glGetError() == GL_NO_ERROR);
    puts("PASS: the drawable has the layer's color format and the app's own depth and stencil, or none");
    return 0;
}
