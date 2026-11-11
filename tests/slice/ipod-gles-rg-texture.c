/* Camera's image-processing helpers upload RG half-float textures. Verify
 * their upload size, desktop type translation and actual channel readback.
 * SLICE hw/arm/gles-host.c range /* The ES half-float type | /* GL_UNPACK_ALIGNMENT
 * CFLAGS -Wno-deprecated-declarations -framework OpenGL
 */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "slice.h"
int main(void)
{
    const GLenum rg = 0x8227;
    assert(gles_texel_bytes(rg, GLES_HALF_FLOAT_OES) == 4);
    assert(gles_texel_bytes(GL_RED, GLES_HALF_FLOAT_OES) == 2);
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj format;
    CGLContextObj context;
    GLint count;
    assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(context) == kCGLNoError);
    const uint16_t half[] = {0x3c00, 0x3800, 0, 0x3c00};
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    const GLint internal[] = {0x8227 /* RG */, 0x822f /* RG16F */};
    for (unsigned i = 0; i < 2; i++) {
        glTexImage2D(GL_TEXTURE_2D, 0, internal[i], 2, 1, 0, rg,
                     gles_host_type(GLES_HALF_FLOAT_OES), half);
        assert(glGetError() == GL_NO_ERROR);
        float result[8] = {0};
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, result);
        assert(glGetError() == GL_NO_ERROR);
        assert(result[0] == 1 && result[1] == .5f && result[2] == 0 &&
               result[3] == 1);
        assert(result[4] == 0 && result[5] == 1 && result[6] == 0 &&
               result[7] == 1);
    }
    glDeleteTextures(1, &texture);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(context);
    puts("ok");
}
