/* An ES 2.0 shader sees GL_ES defined as 1, as on the device.
 *
 * The host compiles GLSL 1.20, which has no GL_ES, refuses `#define GL_ES` and fails `#if GL_ES` on the undefined
 * name. N.O.V.A. 3's engine (Glitch) opens its shaders with `#if GL_ES && TEXTURE_TYPE == ...`, so each of them
 * failed to compile and its menu's buttons never drew. The shaders below take that shape; the fragment shader paints
 * green only on the GL_ES side of its #if.
 *
 * A shader that asks for OES_standard_derivatives (which the SGX reports) compiles too: GLSL 1.20 has dFdx, dFdy
 * and fwidth in its core and no OES extension of that name, so a `require` of it failed the compile.
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
    /* Only the macro is renamed: a longer name that starts with GL_ES is left alone. */
    char *kept = gles_es2_glsl("#ifdef GL_ES_ANOTHER\n#endif\n");
    assert(strstr(kept, "#ifdef GL_ES_ANOTHER\n"));
    g_free(kept);

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
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 16, 16);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER,
                              "#define TEXTURE_2D 1\n#define TEXTURE_RECT 4\n#define TEXTURE_TYPE TEXTURE_2D\n"
                              "#if GL_ES\n"
                              "#    if TEXTURE_TYPE == TEXTURE_RECT\n#        undef TEXTURE_TYPE\n#    endif\n"
                              "#endif\n"
                              "attribute highp vec4 Vertex;\n"
                              "void main() { gl_Position = Vertex; }\n"));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER,
                              "#define TEXTURE_2D 0\n#define TEXTURE_3D 1\n#define TEXTURE_TYPE TEXTURE_2D\n"
                              "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
                              "#if GL_ES && TEXTURE_TYPE == TEXTURE_3D && !defined(GL_OES_texture_3D)\n"
                              "#    define PAINT vec4(1.0, 0.0, 0.0, 1.0)\n"
                              "#elif GL_ES\n"
                              "#    define PAINT vec4(0.0, 1.0, 0.0, 1.0)\n"
                              "#else\n"
                              "#    define PAINT vec4(0.0, 0.0, 1.0, 1.0)\n"
                              "#endif\n"
                              "void main() { gl_FragColor = PAINT; }\n"));
    glBindAttribLocation(p, 0, "Vertex");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(ok);
    gles_es2_use_program(p);
    static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    uint8_t px[4];
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("center %d,%d,%d\n", px[0], px[1], px[2]);
    assert(px[0] == 0 && px[1] == 255 && px[2] == 0);
    glDeleteShader(compile(GL_FRAGMENT_SHADER,
                           "#extension GL_OES_standard_derivatives : require\n"
                           "precision mediump float;\n"
                           "void main() { gl_FragColor = vec4(fwidth(gl_FragCoord.x)); }\n"));
    assert(glGetError() == GL_NO_ERROR);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    puts("PASS: ES 2.0 shaders see GL_ES and take OES_standard_derivatives");
}
