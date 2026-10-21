/* Exercise the production EGL lifetime adapter, without a guest decoder. */
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <glib.h>
#define GL_GLES_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES3/gl3.h>
#include "../../hw/arm/gles-host-angle.c.inc"

/* The drawable gles_host_init builds, through the OES names it calls: complete in either API. */
static void drawable(void)
{
    GLuint fbo, tex, depth;
    glGenFramebuffersOES(1, &fbo);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, tex, 0);
    glGenRenderbuffersOES(1, &depth);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, depth);
    glRenderbufferStorageOES(GL_RENDERBUFFER_OES, GL_DEPTH24_STENCIL8_OES, 64, 32);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_DEPTH_ATTACHMENT_OES, GL_RENDERBUFFER_OES, depth);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_STENCIL_ATTACHMENT_OES, GL_RENDERBUFFER_OES, depth);
    assert(glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES);
    assert(glIsFramebufferOES(fbo) && glIsRenderbufferOES(depth));
    assert(glGetError() == GL_NO_ERROR);
}

/* Unsized images the executor stores as RGBA, as ES stores them: no error, and BGRA texels
 * render back in place. */
static void unsized_storage(void)
{
    static const unsigned char bgra[] = {200, 100, 50, 255}, l8[4] = {90, 90, 90, 90};
    GLuint tex, fbo;
    unsigned char got[4];
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, l8);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_ALPHA, GL_UNSIGNED_BYTE, l8);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, l8);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, l8);
    assert(glGetError() == GL_NO_ERROR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
    assert(glGetError() == GL_NO_ERROR);
    glGenFramebuffersOES(1, &fbo);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, tex, 0);
    assert(glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    assert(got[0] == 50 && got[1] == 100 && got[2] == 200 && got[3] == 255);
}

/* The executor's APPLE_fence calls, on NV_fence. */
static void fences(void)
{
    GLuint f[2];
    glGenFencesAPPLE(2, f);
    glSetFenceAPPLE(f[0]);
    assert(glIsFenceAPPLE(f[0]));
    glFinishFenceAPPLE(f[0]);
    assert(glTestFenceAPPLE(f[0]));
    assert(glTestObjectAPPLE(GL_FENCE_APPLE, f[0]) && glTestObjectAPPLE(GL_TEXTURE, 1));
    glFinishObjectAPPLE(GL_FENCE_APPLE, f[0]);
    glDeleteFencesAPPLE(2, f);
    assert(glGetError() == GL_NO_ERROR);
}

/* A guest ES 2.0 shader as the guest wrote it (precision statements, no #version). */
static void es2_program(void)
{
    static const char *vs = "attribute vec4 p; void main() { gl_Position = p; }";
    static const char *fs = "precision mediump float; uniform lowp vec4 c; void main() { gl_FragColor = c; }";
    GLuint s[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) }, p = glCreateProgram();
    GLint ok = 0;
    glShaderSource(s[0], 1, &vs, NULL);
    glShaderSource(s[1], 1, &fs, NULL);
    for (int i = 0; i < 2; i++) {
        glCompileShader(s[i]);
        glGetShaderiv(s[i], GL_COMPILE_STATUS, &ok);
        assert(ok);
        glAttachShader(p, s[i]);
    }
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(ok && glGetError() == GL_NO_ERROR);
}

int main(void)
{
    CGLContextObj root, child, isolated, es2;
    assert(gles_angle_create(NULL, false, &root));
    assert(gles_angle_create(root, false, &child));
    assert(gles_angle_create(NULL, false, &isolated));
    CGLRetainContext(root); /* sharegroup root ownership */
    assert(!CGLSetCurrentContext(root));
    GLuint texture; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    unsigned char pixels[] = {17, 31, 47, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glEnable(GL_BLEND);
    assert(glGetError() == GL_NO_ERROR);
    assert(!CGLSetCurrentContext(child));
    assert(glIsTexture(texture) && !glIsEnabled(GL_BLEND));
    CGLReleaseContext(root); CGLReleaseContext(root);
    assert(glIsTexture(texture));
    GLuint fbo; glGenFramebuffersOES(1, &fbo); glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, texture, 0);
    assert(glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES);
    unsigned char got[4]; glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    assert(!memcmp(got, pixels, 4) && glGetError() == GL_NO_ERROR);
    drawable();
    unsized_storage();
    fences();
    assert(!CGLSetCurrentContext(isolated) && !glIsTexture(texture));

    /* An ES 2.0 guest context, in the ES1 child's group: programmable, and the same drawable. */
    assert(gles_angle_create(child, true, &es2) && !CGLSetCurrentContext(es2));
    assert(glIsTexture(texture));
    assert(!strncmp((const char *)glGetString(GL_VERSION), "OpenGL ES 3", 11));
    es2_program();
    drawable();
    unsized_storage();
    fences();
    CGLReleaseContext(es2);
    CGLReleaseContext(child); CGLReleaseContext(isolated);
    assert(!angle_current);
    puts("PASS: actual EGL adapter owns context state, sharegroup lifetime, FBO readback and destruction; "
         "ES 2.0 contexts compile guest GLSL and build the drawable, both APIs store unsized images and fence");
}
