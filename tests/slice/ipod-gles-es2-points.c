/* An ES 2.0 program's points are gl_PointSize wide and see gl_PointCoord, as on the device.
 *
 * The host is a legacy desktop GL 2.1 context: until VERTEX_PROGRAM_POINT_SIZE and POINT_SPRITE are on, it ignores a
 * vertex shader's gl_PointSize (every point one pixel) and leaves gl_PointCoord undefined, so an ES 2.0 particle
 * system drew single dots. The vertex shader below asks for 16-pixel points; the fragment shader paints
 * gl_PointCoord.
 *
 * SLICE hw/arm/gles-host.c fn gles_es2_glsl gles_es2_use_program
 * CFLAGS -Wno-deprecated-declarations -framework OpenGL
 * PKG glib-2.0
 */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slice.h"

static GLuint compile(GLenum type, const char *es) {
    char *src = gles_es2_glsl(es);
    const char *p = src;
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &p, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        printf("%s\n", log);
    }
    assert(ok);
    g_free(src);
    return s;
}

int main(void) {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj format;
    GLint count;
    CGLContextObj ctx;
    assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(ctx) == kCGLNoError);
    GLuint fb, rb;
    glGenFramebuffersEXT(1, &fb); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 64, 64);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER,
                              "attribute vec4 pos;\nvoid main() { gl_PointSize = 16.0; gl_Position = pos; }\n"));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER,
                              "precision mediump float;\n"
                              "void main() { gl_FragColor = vec4(gl_PointCoord, 1.0, 1.0); }\n"));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(ok);
    gles_es2_use_program(p);
    static const float point[2] = {0, 0};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, point);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_POINTS, 0, 1);

    /* the point covers window pixels 24..39; gl_PointCoord runs left to right and top to bottom across it */
    uint8_t px[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const uint8_t *upper_left = px + (38 * 64 + 25) * 4, *lower_right = px + (25 * 64 + 38) * 4;
    const uint8_t *outside = px + (20 * 64 + 20) * 4;
    printf("upper left %d,%d,%d lower right %d,%d,%d outside %d,%d,%d\n", upper_left[0], upper_left[1], upper_left[2],
           lower_right[0], lower_right[1], lower_right[2], outside[0], outside[1], outside[2]);
    assert(upper_left[2] == 255 && upper_left[0] < 64 && upper_left[1] < 64);
    assert(lower_right[2] == 255 && lower_right[0] > 192 && lower_right[1] > 192);
    assert(outside[2] == 0);
    assert(glGetError() == GL_NO_ERROR);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    puts("PASS: ES 2.0 points take gl_PointSize and gl_PointCoord");
}
