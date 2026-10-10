/* An ES 2.0 context reports OES_depth_texture and OES_packed_depth_stencil where the firmware's GLEngine knows them,
 * and the host takes a packed depth-stencil texture.
 *
 * N.O.V.A. 3's engine (Glitch) maps its depth texture formats to GL only when GL_EXTENSIONS lists those two. The
 * front end's ES 2.0 list was PVRTC alone, so on the iPhone 4 the post-processing depth target "Post_RTT_Depth" came
 * back null and the game crashed on it when a mission loaded (issue 23). The list now takes each from the stock
 * GLEngine's own extension names: 4.2.1's has both, 3.2's only OES_packed_depth_stencil.
 *
 * SLICE:fe contrib/gles-public/opengles.c range enum { FE_MBX | static int fe_gpu
 * SLICE:fe contrib/gles-public/opengles.c fn fe_find fe_names
 * SLICE:fe contrib/gles-public/opengles.c range #define FE_X_MBX | /* The list for `gpu`
 * SLICE:fe contrib/gles-public/opengles.c fn fe_extensions
 * SLICE hw/arm/gles-host.c range /* The ES half-float type | /* The pixel type the host takes
 * CFLAGS -Wno-deprecated-declarations -framework OpenGL
 */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned slen(const char *s) { return (unsigned)strlen(s); }
#include "fe.h"
#include "slice.h"

static const char *list(const char *text, unsigned long size)
{
    static char out[1024];
    fe_extensions(out, sizeof out, FE_SGX2, text, size);
    return out;
}

int main(void)
{
    /* GLEngine __TEXT excerpts: 4.2.1's names both, 3.2's no OES_depth_texture (a longer name doesn't count). */
    static const char e421[] = "GL_ARB_shadow\0GL_OES_depth_texture\0GL_ARB_depth_texture\0GL_OES_packed_depth_stencil\0"
                               "GL_IMG_texture_compression_pvrtc\0";
    static const char e32[] = "GL_ARB_shadow\0GL_OES_depth_texture_cube_map\0GL_OES_packed_depth_stencil\0"
                              "GL_IMG_texture_compression_pvrtc\0";
    assert(!strcmp(list(e421, sizeof e421),
                   "GL_IMG_texture_compression_pvrtc GL_OES_depth_texture GL_OES_packed_depth_stencil"));
    assert(!strcmp(list(e32, sizeof e32), "GL_IMG_texture_compression_pvrtc GL_OES_packed_depth_stencil"));

    /* What the engine then allocates: DEPTH_COMPONENT/UNSIGNED_SHORT and DEPTH_STENCIL/UNSIGNED_INT_24_8 at the
     * iPhone 4's 960x640, attached as a framebuffer's depth (and stencil), as glTexImage2D's slot passes them on. */
    assert(gles_texel_bytes(GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT) == 2);
    assert(gles_texel_bytes(GLES_DEPTH_STENCIL_OES, GLES_UNSIGNED_INT_24_8_OES) == 4);
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj format;
    GLint count;
    CGLContextObj ctx;
    assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(ctx) == kCGLNoError);
    static const GLenum fmt[2] = {GL_DEPTH_COMPONENT, GLES_DEPTH_STENCIL_OES};
    static const GLenum type[2] = {GL_UNSIGNED_SHORT, GLES_UNSIGNED_INT_24_8_OES};
    for (int i = 0; i < 2; i++) {
        GLuint fb, color, depth;
        glGenFramebuffersEXT(1, &fb);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
        glGenRenderbuffersEXT(1, &color);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, color);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 960, 640);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, color);
        glGenTextures(1, &depth);
        glBindTexture(GL_TEXTURE_2D, depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, fmt[i], 960, 640, 0, fmt[i], type[i], NULL);
        assert(glGetError() == GL_NO_ERROR);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, depth, 0);
        if (i) glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_TEXTURE_2D, depth, 0);
        assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    }
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    puts("PASS: ES 2.0 lists the firmware's depth texture extensions and the host takes both depth texture formats");
}
