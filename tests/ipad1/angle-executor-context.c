/* Exercise the production EGL lifetime adapter, without a guest decoder. */
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <glib.h>
#define GL_GLES_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include "../../hw/arm/gles-host-angle.c.inc"
int main(void)
{
    CGLContextObj root, child, isolated;
    assert(gles_angle_create(NULL, &root));
    assert(gles_angle_create(root, &child));
    assert(gles_angle_create(NULL, &isolated));
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
    assert(!CGLSetCurrentContext(isolated) && !glIsTexture(texture));
    CGLReleaseContext(child); CGLReleaseContext(isolated);
    assert(!angle_current);
    puts("PASS: actual EGL adapter owns context state, sharegroup lifetime, FBO readback and destruction");
}
