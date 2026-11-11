/* VectorKit needs context-local VAOs and direct updates of noncurrent programs.
 * Exercise production container lifetime and the direct-uniform dispatcher on
 * real accelerated GL, including the resulting fragment color.
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c fn gles_reject gles_buffer_destroy gles_buffer_bind gles_vao_array_copy gles_vao_free gles_vao_table gles_vao_bind gles_vao_call gles_buffer_forget gles_es2_current_attrib gles_es2_attrib_value gles_es2_emit_vertices
 * SLICE:uniform hw/arm/gles-host.c fn gles_program_uniform
 * SLICE:ordinary hw/arm/gles-host.c range case 602: glUniform1f | case 625: case 632:
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include "slice.h"
static unsigned char memory[4096];
int gles_guest_rw(CPUState *cpu, vaddr addr, void *bytes, size_t count,
                  bool write)
{
    (void)cpu;
    if (addr > sizeof(memory) || count > sizeof(memory) - addr)
        return -1;
    if (write)
        memcpy(memory + addr, bytes, count);
    else
        memcpy(bytes, memory + addr, count);
    return 0;
}
static GLuint programs[2];
static GLuint gles_glsl_host(uint32_t guest)
{
    return guest == 100 ? programs[0] : guest == 200 ? programs[1] : 0;
}
static float gles_f(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, 4);
    return value;
}
static void *gles_es2_fetch(CPUState *cpu, uint32_t ptr, uint64_t n)
{
    if (!n || n > 65536)
        return NULL;
    void *v = g_malloc(n * 4);
    if (gles_guest_rw(cpu, ptr, v, n * 4, false)) {
        g_free(v);
        return NULL;
    }
    return v;
}
/* Ordinary uniform dispatch cases are also cut from production. The guest
 * test location 123 resolves in the target program, not the current one. */
static bool gles_es2_call(CPUState *cpu, uint32_t slot, uint32_t argc,
                          const uint32_t *args, int64_t *r)
{
    static const unsigned vec_n[] = {1, 2, 3, 4};
    uint32_t a[5] = {0};
    memcpy(a, args, argc * 4);
    if (a[0] == 123)
        a[0] = glGetUniformLocation(gles_glsl_host(gh.program), "color");
    g_autofree void *v = NULL;
    *r = 0;
    switch (slot) {
#include "ordinary.h"
    default:
        assert(0);
    }
    return true;
}
#include "uniform.h"
static GLuint shader(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    GLint ok;
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    assert(ok);
    return s;
}
static GLuint program(bool tint)
{
    GLuint p = glCreateProgram();
    GLint ok;
    glAttachShader(
        p,
        shader(
            GL_VERTEX_SHADER,
            tint
                ? "attribute vec2 pos; attribute vec4 tint; varying vec4 t; void main(){gl_Position=vec4(pos,0.0,1.0);t=tint;}"
                : "attribute vec2 pos; void main(){gl_Position=vec4(pos,0.0,1.0);}"));
    glAttachShader(
        p,
        shader(
            GL_FRAGMENT_SHADER,
            tint
                ? "uniform vec4 color; varying vec4 t; void main(){gl_FragColor=color*t;}"
                : "uniform vec4 color; void main(){gl_FragColor=color;}"));
    glBindAttribLocation(p, 0, "tint");
    glBindAttribLocation(p, 1, "pos");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(ok);
    return p;
}
int main(void)
{
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj fmt;
    CGLContextObj ctx;
    GLint count;
    assert(!CGLChoosePixelFormat(attrs, &fmt, &count));
    assert(!CGLCreateContext(fmt, NULL, &ctx));
    CGLDestroyPixelFormat(fmt);
    assert(!CGLSetCurrentContext(ctx));
    programs[0] = program(false);
    programs[1] = program(false);
    glUseProgram(programs[0]);
    gh.program = 100;
    const float color[] = {.2f, .6f, .8f, 1};
    memcpy(memory + 256, color, sizeof(color));
    uint32_t update[] = {200, 123, 1, 256};
    int64_t result;
    assert(gles_program_uniform(NULL, 907, 4, update, &result) && !result);
    GLint current;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    assert(current == programs[0] && gh.program == 100);
    float actual[4];
    glGetUniformfv(programs[1], glGetUniformLocation(programs[1], "color"),
                   actual);
    assert(!memcmp(actual, color, sizeof(color)));
    glGetUniformfv(programs[0], glGetUniformLocation(programs[0], "color"),
                   actual);
    assert(actual[0] == 0 && actual[3] == 0);
    update[0] = 999;
    assert(gles_program_uniform(NULL, 907, 4, update, &result) && result == -1);
    assert(gh.program == 100);
    gh.error = 0;
    update[0] = 200;
    update[3] = sizeof(memory);
    assert(gles_program_uniform(NULL, 907, 4, update, &result) && result == -1);
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    assert(current == programs[0] && gh.program == 100);

    uint32_t args[] = {2, 64};
    assert(!gles_vao_call(NULL, 839, args));
    uint32_t names[2];
    memcpy(names, memory + 64, sizeof(names));
    assert(names[0] && names[1] && names[0] != names[1]);
    args[0] = names[0];
    assert(!gles_vao_call(NULL, 840, args));
    assert(!gles_vao_call(NULL, 837, args));
    assert(gles_vao_call(NULL, 840, args));
    GLESBuffer *buffer = g_new0(GLESBuffer, 1);
    buffer->refs = 1;
    buffer->name = 77;
    buffer->size = 6 * sizeof(float);
    gles_buffer_bytes = buffer->size;
    const float triangle[] = {-1, -1, 3, -1, -1, 3};
    buffer->data = g_memdup2(triangle, sizeof(triangle));
    gles_buffer_bind(&gh.array_buffer, buffer);
    gh.attr[0].enabled = 1;
    gh.attr[0].size = 2;
    gh.attr[0].type = GL_FLOAT;
    gles_buffer_bind(&gh.attr[0].vbo, buffer);
    gles_buffer_bind(&gh.element_buffer, buffer);
    gh.attr_normalized[0] = true;
    assert(!gles_vao_bind(names[1]));
    assert(!gh.attr[0].enabled && !gh.attr[0].vbo && !gh.element_buffer);
    assert(gh.array_buffer == buffer); /* not VAO state */
    assert(!gles_vao_bind(names[0]));
    assert(gh.attr[0].enabled && gh.attr[0].vbo == buffer &&
           gh.element_buffer == buffer);
    assert(gh.attr_normalized[0]);
    assert(gles_vao_bind(0x12345678) == -1 && gh.vao_binding == names[0]);
    gh.error = 0;

    GLuint fb, rb;
    glGenFramebuffersEXT(1, &fb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 8, 8);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, rb);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) ==
           GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 8, 8);
    glUseProgram(programs[1]);
    glVertexAttribPointer(1, gh.attr[0].size, gh.attr[0].type,
                          gh.attr_normalized[0], 0, gh.attr[0].vbo->data);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    uint8_t pixel[4];
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("Position in attribute 1, without emission adapter: %u,%u,%u,%u\n",
           pixel[0], pixel[1], pixel[2], pixel[3]);
    GLfloat initial[4];
    gles_es2_current_attrib(0, initial);
    assert(initial[0] == 0 && initial[1] == 0 && initial[2] == 0 &&
           initial[3] == 1);
    assert(gles_es2_emit_vertices(3));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(0);
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(abs(pixel[0] - 51) <= 1 && abs(pixel[1] - 153) <= 1 &&
           abs(pixel[2] - 204) <= 1 && pixel[3] == 255);
    GLuint tinted = program(true);
    glUseProgram(tinted);
    glUniform4f(glGetUniformLocation(tinted, "color"), 1, 1, 1, 1);
    const GLfloat constant[4] = {.25f, .5f, .75f, 1};
    gles_es2_attrib_value(0, constant);
    assert(gles_es2_emit_vertices(3));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(0);
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(abs(pixel[0] - 64) <= 1 && abs(pixel[1] - 128) <= 1 &&
           abs(pixel[2] - 191) <= 1 && pixel[3] == 255);
    gles_es2_current_attrib(0, initial);
    assert(!memcmp(initial, constant, sizeof(initial)));
    g_free(gh.attr[0].fbuf);
    gh.attr[0].fbuf = NULL;
    gles_buffer_forget(buffer);
    assert(!gh.attr[0].vbo && !gh.element_buffer);
    assert(!gles_vao_bind(names[1]));
    assert(!gles_vao_bind(names[0]));
    assert(!gh.attr[0].vbo && !gh.element_buffer);
    args[0] = 2;
    args[1] = 64;
    assert(!gles_vao_call(NULL, 838, args));
    assert(!gh.vao_binding);
    g_hash_table_destroy(gh.vaos);
    gles_buffer_destroy(buffer);
    assert(!gles_buffer_bytes);
    assert(glGetError() == GL_NO_ERROR);
    puts(
        "PASS: VectorKit direct uniforms, restored current program, VAO captures/deletes and accelerated pixel output");
    return 0;
}
