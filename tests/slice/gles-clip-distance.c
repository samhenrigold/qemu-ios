/* GPU clip distances, including disabled planes and a vertex outside a plane.
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c define GLES_MAX_STRING
 * SLICE:prefix hw/arm/gles-framebuffer-fetch.h fn gles_extension_prefix_end
 * SLICE:clip hw/arm/gles-clip-distance.h file
 * SLICE hw/arm/gles-host.c fn gles_es2_glsl
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include "prefix.h"
#include "slice.h"
#include "clip.h"
int gles_guest_rw(CPUState *cpu, vaddr addr, void *p, size_t n, bool w)
{
    abort();
}
static GLuint shader(GLenum type, const char *text)
{
    g_autofree char *desktop = gles_es2_glsl(text);
    g_autofree char *clip = type == GL_VERTEX_SHADER
                                ? gles_clip_vertex_glsl(desktop)
                                : g_strdup(desktop);
    const char *p = clip;
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &p, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n%s\n", log, p);
    }
    assert(ok);
    return s;
}
static void pixel(int x, int y, int red)
{
    uint8_t p[4];
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    printf("%d,%d: %d,%d,%d\n", x, y, p[0], p[1], p[2]);
    assert(p[0] == red && p[1] == 0 && p[2] == 0);
}
int main(void)
{
    CGLPixelFormatAttribute attr[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    assert(!CGLChoosePixelFormat(attr, &pf, &n));
    assert(!CGLCreateContext(pf, NULL, &c));
    CGLDestroyPixelFormat(pf);
    assert(!CGLSetCurrentContext(c));
    GLuint p = glCreateProgram();
    glAttachShader(
        p,
        shader(
            GL_VERTEX_SHADER,
            "#extension GL_APPLE_clip_distance : require\n#if !GL_APPLE_clip_distance\n#error clip capability lost\n#endif\nvarying highp float gl_ClipDistance[gl_MaxClipDistances];\n"
            "attribute vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);gl_ClipDistance[0]=position.x;gl_ClipDistance[1]=position.y;gl_ClipDistance[5]=-1.0;}"));
    glAttachShader(
        p,
        shader(
            GL_FRAGMENT_SHADER,
            "precision mediump float;void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}"));
    glBindAttribLocation(p, 0, "position");
    gles_clip_link(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
    }
    assert(ok);
    glUseProgram(p);
    GLuint f, t;
    glGenFramebuffersEXT(1, &f);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, f);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, t, 0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) ==
           GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 32, 32);
    glClearColor(0, 0, 0, 1);
    float v[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, v);
    glEnableVertexAttribArray(0);
    GLint e0 = glGetUniformLocation(p, "qemu_clip_enabled0"),
          e1 = glGetUniformLocation(p, "qemu_clip_enabled1");
    assert(e0 >= 0 && e1 >= 0);
    glUniform4f(e0, 1, 1, 0, 0);
    glUniform4f(e1, 0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pixel(24, 24, 255);
    pixel(8, 24, 0);
    pixel(24, 8, 0);
    glUniform4f(e0, 0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pixel(8, 8, 255);
    glUniform4f(e1, 0, 1, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pixel(24, 24, 0);
    GLuint query, samples;
    glGenQueries(1, &query);
    glBeginQuery(GL_SAMPLES_PASSED, query);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_SAMPLES_PASSED);
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);
    assert(samples == 0);
    glUniform4f(e1, 0, 0, 0, 0);
    glBeginQuery(GL_SAMPLES_PASSED, query);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEndQuery(GL_SAMPLES_PASSED);
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);
    assert(samples > 0);
    glDeleteQueries(1, &query);
    assert(!glGetError());
    puts("clip distances PASS");
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(c);
    return 0;
}
