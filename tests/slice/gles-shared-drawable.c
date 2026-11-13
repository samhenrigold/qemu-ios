/* Shared CA renderbuffer storage must survive cross-context rendering.
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c range /* ---------------------------------------------------------- GPU memory | /*\n * Profiling and strictness
 * SLICE hw/arm/gles-host.c range #define GLES_PRIVATE_NAME | static bool gles_is_drawable(
 * SLICE hw/arm/gles-host.c fn gles_reject gles_texture_object gles_is_drawable gles_host_fbo gles_attachment gles_drawable_depth_stencil gles_set_drawable gles_texture_begin gles_drawable_storage gles_refresh_drawable_binding gles_layer_storage gles_record_drawable_attachment gles_resolve_drawable
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include "slice.h"
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write)
{
    abort();
}
static void pixel(void)
{
    uint8_t p[4];
    glReadPixels(639, 1135, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    printf("far edge: %u,%u,%u,%u\n", p[0], p[1], p[2], p[3]);
    assert(abs(p[0] - 51) <= 1 && abs(p[1] - 153) <= 1 &&
           abs(p[2] - 204) <= 1 && p[3] == 255);
}
int main(void)
{
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj pf;
    GLint count;
    CGLContextObj ca, cb;
    assert(CGLChoosePixelFormat(attrs, &pf, &count) == kCGLNoError);
    assert(CGLCreateContext(pf, NULL, &ca) == kCGLNoError);
    assert(CGLCreateContext(pf, ca, &cb) == kCGLNoError);
    CGLDestroyPixelFormat(pf);
    GLESHost a = {0}, b = {0};
    a.rb_sized = b.rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
    a.charged = b.charged =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    a.fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);
    b.fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);
    CGLSetCurrentContext(ca);
    gh_current = &a;
    GLuint color;
    glGenRenderbuffersEXT(1, &color);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, color);
    a.bound_renderbuffer = color;
    assert(gles_layer_storage(640, 1136, false) == 0);
    assert(a.drawable_renderbuffer == color);
    CGLSetCurrentContext(cb);
    gh_current = &b;
    assert(gles_drawable_storage(320, 480, false) == 0);
    GLuint fb;
    glGenFramebuffersEXT(1, &fb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    b.bound_framebuffer = fb;
    gles_set_drawable(fb, color);
    assert(gles_is_drawable(fb));
    uint32_t attach[] = {GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                         GL_RENDERBUFFER_EXT, color};
    gles_record_drawable_attachment(attach);
    gles_refresh_drawable_binding();
    assert(!gles_is_drawable(fb));
    GLint bound;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &bound);
    assert(bound == fb);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) ==
           GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 640, 1136);
    glClearColor(.2, .6, .8, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    pixel();
    CGLSetCurrentContext(ca);
    gh_current = &a;
    CGLDestroyContext(cb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, a.fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    glColorMask(0, 0, 0, 0);
    assert(gles_resolve_drawable());
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &bound);
    assert(bound == a.fbo);
    assert(glIsEnabled(GL_SCISSOR_TEST));
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    assert(!mask[0] && !mask[1] && !mask[2] && !mask[3]);
    pixel();
    assert(glGetError() == GL_NO_ERROR);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ca);
    puts("shared drawable: PASS");
    return 0;
}
