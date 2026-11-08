/* APPLE_shader_framebuffer_fetch: a fragment shader reads what the framebuffer held when its draw started.
 *
 * 6.x's WebKit tiles compile a shader with it whatever GL_EXTENSIONS says. Neither host GLSL knows gl_LastFragData, so
 * the compile failed, the program stayed the one in use before, and Safari's text drew as solid boxes (LightTouchMac
 * issue 126). Each draw here halves the red it finds, so a second draw reads the first one's result, not the first
 * copy; a shader without the extension is left as it came.
 *
 * SLICE hw/arm/gles-framebuffer-fetch.h fn gles_extension_prefix_end
 * SLICE hw/arm/gles-host.c fn gles_es2_glsl gles_es2_fetch_glsl gles_es2_fetch_refresh
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
    char *fetched = gles_es2_fetch_glsl(es);
    char *src = gles_es2_glsl(fetched);
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
    g_free(fetched);
    g_free(src);
    return s;
}

int main(void) {
    char *kept = gles_es2_fetch_glsl("void main() { gl_FragColor = vec4(1.0); }\n");
    assert(!strcmp(kept, "void main() { gl_FragColor = vec4(1.0); }\n"));
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
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER,
                              "attribute highp vec4 Vertex;\n"
                              "void main() { gl_Position = Vertex; }\n"));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER,
                              "#extension GL_APPLE_shader_framebuffer_fetch : require\n#extension GL_EXT_shader_texture_lod : enable\n"
                              "precision mediump float;\n"
                              "void main() { gl_FragColor = gl_LastFragData[0] * vec4(0.5, 1.0, 1.0, 1.0); }\n"));
    glBindAttribLocation(p, 0, "Vertex");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(ok);
    glUseProgram(p);
    GLint sampler = glGetUniformLocation(p, "gles_fetch"), size = glGetUniformLocation(p, "gles_fetch_size");
    assert(sampler >= 0 && size >= 0);
    static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);

    GLuint tex = 0;
    GLint tex_size[2] = {0, 0};
    GLint unit = 0;
    glActiveTexture(GL_TEXTURE3);
    uint8_t px[4];
    for (int draw = 0, want = 128; draw < 2; draw++, want = 64) {
        gles_es2_fetch_refresh(sampler, size, &tex, tex_size);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("draw %d: %d,%d,%d\n", draw, px[0], px[1], px[2]);
        assert(abs(px[0] - want) <= 1 && px[1] == 0 && px[2] == 0);
    }
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    assert(unit == GL_TEXTURE3);            /* the guest's unit is left as it was */
    assert(glGetError() == GL_NO_ERROR);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    puts("PASS: framebuffer fetch reads what the framebuffer holds");
}
