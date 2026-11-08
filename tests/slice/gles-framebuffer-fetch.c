/* Synthetic accelerated ES2 fetch shaders: coherent overlapping primitives,
 * strip winding, float storage, hidden uniforms and preserved host bindings.
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c range enum { GLES_CHARGE_TEXTURE | /* Gives back every image
 * SLICE hw/arm/gles-host.c define GLES_PRIVATE_NAME
 * SLICE hw/arm/gles-host.c define GLES_MAX_STRING
 * SLICE hw/arm/gles-host.c fn gles_private_name gles_es2_glsl gles_reject gles_texture_begin
 * SLICE:fetch hw/arm/gles-framebuffer-fetch.h file
 * SLICE:clip hw/arm/gles-clip-distance.h file
 * SLICE:link hw/arm/gles-host.c fn gles_link_program
 * CFLAGS -I$ROOT/include -I$ROOT/hw/arm -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 * VARIANT -DTEST_FETCH_BARRIERS=0
 * VARIANT -DTEST_FETCH_BARRIERS=1
 */
#include "ipod-gles.h"
#include "slice.h"
static uint16_t lduw_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | (unsigned)b[1] << 8;
}
static unsigned copy_allocations, copy_updates;
static void counted_copy_image(GLenum target, GLint level, GLenum format,
                               GLint x, GLint y, GLsizei width, GLsizei height,
                               GLint border)
{
    copy_allocations++;
    glCopyTexImage2D(target, level, format, x, y, width, height, border);
}
static void counted_copy_update(GLenum target, GLint level, GLint xoff,
                                GLint yoff, GLint x, GLint y, GLsizei width,
                                GLsizei height)
{
    copy_updates++;
    glCopyTexSubImage2D(target, level, xoff, yoff, x, y, width, height);
}
#define glCopyTexImage2D counted_copy_image
#define glCopyTexSubImage2D counted_copy_update
/* Pixel tests call the native executor directly, without a QEMU BQL. */
bool gles_native_begin(void)
{
    return false;
}
void gles_native_end(bool unlocked)
{
    assert(!unlocked);
}
#include "fetch.h"
#undef glCopyTexImage2D
#undef glCopyTexSubImage2D
#include "clip.h"
#include "link.h"
static bool test_fetch_draw(GLESFetchProgram *p, GLenum mode, uint32_t count,
                            const uint8_t *indices, unsigned bytes)
{
    return gles_fetch_draw_impl(p, mode, count, indices, bytes,
                                TEST_FETCH_BARRIERS);
}
#define gles_fetch_draw test_fetch_draw

int gles_guest_rw(CPUState *cpu, vaddr addr, void *p, size_t n, bool w)
{
    abort();
}
static GLuint compile(GLenum type, const char *source)
{
    g_autofree char *desktop = gles_es2_glsl(source);
    g_autofree char *clipped = type == GL_VERTEX_SHADER
                                   ? gles_clip_vertex_glsl(desktop)
                                   : g_strdup(desktop);
    g_autofree char *adapted = gles_fetch_glsl(clipped);
    const char *p = adapted;
    GLuint s = glCreateShader(type);
    GLint ok;
    glShaderSource(s, 1, &p, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n%s\n", log, p);
    }
    assert(ok);
    return s;
}
static GLuint make_program(void)
{
    GLuint p = glCreateProgram();
    GLint ok;
    glAttachShader(
        p,
        compile(
            GL_VERTEX_SHADER,
            "#extension GL_APPLE_clip_distance : require\n"
            "#extension GL_EXT_shader_framebuffer_fetch : require\n"
            "varying highp float gl_ClipDistance[6];\n#if !GL_APPLE_clip_distance\n#error clip capability lost\n#endif\n"
            "#if !GL_EXT_shader_framebuffer_fetch\n#error fetch capability lost\n#endif\n"
            "attribute vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}"));
    glAttachShader(
        p,
        compile(
            GL_FRAGMENT_SHADER,
            "#extension GL_EXT_shader_framebuffer_fetch : require\n"
            "#extension GL_EXT_shader_texture_lod : require\n"
            "precision mediump float;\n"
            "mediump vec4 gl_LastFragData[gl_MaxDrawBuffers];\n"
            "uniform vec4 amount;void main(){gl_FragColor=gl_LastFragData[0]+amount;}"));
    glBindAttribLocation(p, 0, "position");
    gles_link_program(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    assert(ok);
    gles_fetch_link(p);
    assert(gles_fetch_program(p));
    return p;
}
static void expect(float r, float g, float b)
{
    float v[4];
    glReadPixels(24, 24, 1, 1, GL_RGBA, GL_FLOAT, v);
    assert(fabsf(v[0] - r) < .008 && fabsf(v[1] - g) < .008 &&
           fabsf(v[2] - b) < .008 && fabsf(v[3] - 1) < .008);
}
static void batch_regression(void)
{
    GLuint p = glCreateProgram();
    glAttachShader(
        p, compile(GL_VERTEX_SHADER,
                   "attribute vec4 position;varying float shade;"
                   "void main(){gl_Position=position;shade=position.z;}"));
    glAttachShader(
        p,
        compile(
            GL_FRAGMENT_SHADER,
            "#extension GL_EXT_shader_framebuffer_fetch : require\n"
            "varying float shade;void main(){gl_FragColor=gl_LastFragData[0]*0.5+vec4(shade,0.,0.,0.5);}"));
    glBindAttribLocation(p, 0, "position");
    gles_link_program(p);
    GLint linked;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    assert(linked);
    gles_fetch_link(p);
    glUseProgram(p);
    GLuint f, t;
    glGenFramebuffersEXT(1, &f);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, f);
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, t, 0);
    glViewport(0, 0, 256, 256);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    float vertices[64 * 2 * 3 * 4];
    uint16_t index[64 * 2 * 3];
    unsigned n = 0;
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++)
            for (unsigned layer = 0; layer < 2; layer++) {
                float xx = -.95f + x * .25f, yy = -.95f + y * .25f;
                float tri[] = {xx,        yy,        .2f + .2f * layer, 1,
                               xx + .06f, yy,        .2f + .2f * layer, 1,
                               xx,        yy + .06f, .2f + .2f * layer, 1};
                memcpy(vertices + n * 4, tri, sizeof(tri));
                for (unsigned i = 0; i < 3; i++)
                    index[n + i] = n + (i + 1) % 3;
                n += 3;
            }
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, 0, vertices);
    glEnableVertexAttribArray(0);
    GLuint sentinel;
    glGenBuffers(1, &sentinel);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, sentinel);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 128, NULL, GL_STREAM_READ);
    glBindBufferRangeEXT(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 0, sentinel, 16, 32);
    unsigned char baseline[256 * 256 * 4], actual[sizeof(baseline)];
    for (unsigned indexed = 0; indexed < 2; indexed++) {
        const uint8_t *indices = indexed ? (const uint8_t *)index : NULL;
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        assert(gles_fetch_draw_impl(gles_fetch_program(p), GL_TRIANGLES, n,
                                    indices, 2, false));
        glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, baseline);
        glClear(GL_COLOR_BUFFER_BIT);
        GLuint query;
        glGenQueries(1, &query);
        glBeginQuery(GL_SAMPLES_PASSED, query);
        uint64_t before = gles_fetch_batch_draws;
        assert(gles_fetch_draw_impl(gles_fetch_program(p), GL_TRIANGLES, n,
                                    indices, 2, true));
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint samples;
        glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);
        unsigned pixels = 0;
        for (unsigned i = 0; i < sizeof(baseline); i += 4)
            if (baseline[i])
                pixels++;
        assert(samples == pixels * 2);
        glDeleteQueries(1, &query);
        assert(gles_fetch_batch_draws - before == 2);
        glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, actual);
        assert(!memcmp(baseline, actual, sizeof(actual)));
        GLint state;
        glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING_EXT, &state);
        assert(state == sentinel);
        glGetIntegerIndexedvEXT(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING_EXT, 0,
                                &state);
        assert(state == sentinel);
        glGetIntegerIndexedvEXT(GL_TRANSFORM_FEEDBACK_BUFFER_START_EXT, 0,
                                &state);
        assert(state == 16);
        glGetIntegerIndexedvEXT(GL_TRANSFORM_FEEDBACK_BUFFER_SIZE_EXT, 0,
                                &state);
        assert(state == 32);
        assert(!glIsEnabled(GL_RASTERIZER_DISCARD_EXT));
    }
    /* A primitive crossing w=0 gets a full-screen dependency bound. Preserve
     * the original result rather than projecting it into a misleading box. */
    vertices[3] = -.2f;
    glClear(GL_COLOR_BUFFER_BIT);
    assert(gles_fetch_draw_impl(gles_fetch_program(p), GL_TRIANGLES, n, NULL, 0,
                                false));
    glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, baseline);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(gles_fetch_draw_impl(gles_fetch_program(p), GL_TRIANGLES, n, NULL, 0,
                                true));
    glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, actual);
    assert(!memcmp(baseline, actual, sizeof(actual)));
    assert(glGetError() == GL_NO_ERROR);
    puts(
        "PASS: 128 order-dependent triangles reduce to two native draws with identical pixels, arrays and indices");
}
int main(void)
{
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj f;
    CGLContextObj ctx;
    GLint count;
    assert(!CGLChoosePixelFormat(attrs, &f, &count));
    assert(!CGLCreateContext(f, NULL, &ctx));
    CGLDestroyPixelFormat(f);
    assert(!CGLSetCurrentContext(ctx));
    assert(gles_fetch_capabilities() & 1);
    if (TEST_FETCH_BARRIERS)
        assert(gles_fetch_barriers_supported());
    GLuint program = make_program();
    glUseProgram(program);
    GLESFetchProgram *fetch = gles_fetch_program(program);
    assert(fetch->uniforms->len == 1);
    char name[64];
    GLsizei length;
    GLint size;
    GLenum type;
    glGetActiveUniform(program, g_array_index(fetch->uniforms, GLuint, 0),
                       sizeof(name), &length, &size, &type, name);
    assert(!strcmp(name, "amount"));
    GLuint fb, color, sentinel;
    glGenFramebuffersEXT(1, &fb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, color, 0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) ==
           GL_FRAMEBUFFER_COMPLETE_EXT);
    glGenTextures(1, &sentinel);
    glActiveTexture(GL_TEXTURE0 + GLES_MAX_TEXUNITS);
    glBindTexture(GL_TEXTURE_2D, sentinel);
    glActiveTexture(GL_TEXTURE3);
    glViewport(0, 0, 32, 32);
    glEnableVertexAttribArray(0);
    static const float repeated[] = {-1, -1, 3, -1, -1, 3,
                                     -1, -1, 3, -1, -1, 3};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, repeated);
    glClearColor(0, .1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(glGetUniformLocation(program, "amount"), .2, .1, 0, 0);
    assert(gles_fetch_draw(fetch, GL_TRIANGLES, 6, NULL, 0));
    expect(.4, .3, 0);
    assert(copy_allocations == 1 &&
           copy_updates == (TEST_FETCH_BARRIERS ? 0 : 1));
    GLint state;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &state);
    assert(state == GL_TEXTURE3);
    glActiveTexture(GL_TEXTURE0 + GLES_MAX_TEXUNITS);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &state);
    assert(state == sentinel);
    glActiveTexture(GL_TEXTURE3);
    uint16_t indices[] = {0, 1, 2, 0, 1, 2};
    glClear(GL_COLOR_BUFFER_BIT);
    assert(
        gles_fetch_draw(fetch, GL_TRIANGLES, 6, (const uint8_t *)indices, 2));
    expect(.4, .3, 0);
    static const float strip[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, strip);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(gles_fetch_draw(fetch, GL_TRIANGLE_STRIP, 4, NULL, 0));
    expect(.2, .2, 0);
    static const float fan[] = {-1, -1, 1, -1, 1, 1, -1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, fan);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(gles_fetch_draw(fetch, GL_TRIANGLE_FAN, 4, NULL, 0));
    expect(.2, .2, 0);
    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, 32, 32, 0, GL_RGBA, GL_FLOAT,
                 NULL);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, repeated);
    glClearColor(.5, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(glGetUniformLocation(program, "amount"), .75, 0, 0, 0);
    assert(gles_fetch_draw(fetch, GL_TRIANGLES, 6, NULL, 0));
    expect(2, 0, 0);
    assert(glGetError() == GL_NO_ERROR);
    /* Explicit ES LOD and gradients retain the selected mip level after
     * translation to desktop ARB names, alongside framebuffer fetch. */
    GLuint lod = glCreateProgram();
    GLint linked;
    glAttachShader(
        lod,
        compile(
            GL_VERTEX_SHADER,
            "attribute vec2 position;void main(){gl_Position=vec4(position,0.,1.);}"));
    glAttachShader(
        lod,
        compile(
            GL_FRAGMENT_SHADER,
            "#extension GL_EXT_shader_framebuffer_fetch : require\n"
            "#extension GL_EXT_shader_texture_lod : require\n"
            "precision mediump float;uniform sampler2D image;"
            "void main(){gl_FragColor=gl_LastFragData[0]+"
            "0.5*texture2DLodEXT(image,vec2(0.5),1.0)+"
            "0.5*texture2DGradEXT(image,vec2(0.5),vec2(0.5,0.),vec2(0.,0.5));}"));
    glBindAttribLocation(lod, 0, "position");
    glLinkProgram(lod);
    glGetProgramiv(lod, GL_LINK_STATUS, &linked);
    assert(linked);
    gles_fetch_link(lod);
    glUseProgram(lod);
    GLuint source;
    glGenTextures(1, &source);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    uint8_t red[] = {255, 0, 0, 255, 255, 0, 0, 255,
                     255, 0, 0, 255, 255, 0, 0, 255};
    uint8_t green[] = {0, 255, 0, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 red);
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 green);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glUniform1i(glGetUniformLocation(lod, "image"), 0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(gles_fetch_draw(gles_fetch_program(lod), GL_TRIANGLES, 3, NULL, 0));
    expect(.5, .5, 0);
    assert(glGetError() == GL_NO_ERROR);
    assert(copy_allocations == 2 &&
           copy_updates >= (TEST_FETCH_BARRIERS ? 4 : 8));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, 16, 16, 0, GL_RGBA, GL_FLOAT,
                 NULL);
    glActiveTexture(GL_TEXTURE0 + GLES_MAX_TEXUNITS);
    assert(gles_fetch_copy(gles_fetch_program(lod)));
    assert(copy_allocations == 3);
    GLint width;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    assert(width == 16);
    assert(glGetError() == GL_NO_ERROR);
    GLuint mixed = glCreateProgram();
    glAttachShader(
        mixed,
        compile(
            GL_VERTEX_SHADER,
            "#extension GL_APPLE_clip_distance : require\n"
            "varying highp float gl_ClipDistance[8];\nattribute vec2 position;"
            "void main(){gl_Position=vec4(position,0.,1.);gl_ClipDistance[0]=position.x;}"));
    glAttachShader(
        mixed,
        compile(
            GL_FRAGMENT_SHADER,
            "#extension GL_EXT_shader_framebuffer_fetch : require\n"
            "uniform vec4 amount;void main(){gl_FragColor=gl_LastFragData[0]+amount;}"));
    glBindAttribLocation(mixed, 0, "position");
    gles_clip_link(mixed);
    glGetProgramiv(mixed, GL_LINK_STATUS, &linked);
    assert(linked);
    gles_fetch_link(mixed);
    GLESFetchProgram *mixed_fetch = gles_fetch_program(mixed);
    assert(mixed_fetch && mixed_fetch->uniforms->len == 1);
    glUseProgram(mixed);
    glUniform4f(glGetUniformLocation(mixed, "amount"), .2, 0, 0, 0);
    glUniform4f(mixed_fetch->clip[0], 1, 0, 0, 0);
    glUniform4f(mixed_fetch->clip[1], 0, 0, 0, 0);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(gles_fetch_draw(mixed_fetch, GL_TRIANGLES, 6, NULL, 0));
    float mixed_pixel[4];
    glReadPixels(12, 8, 1, 1, GL_RGBA, GL_FLOAT, mixed_pixel);
    assert(fabsf(mixed_pixel[0] - .4f) < .008f);
    glReadPixels(4, 8, 1, 1, GL_RGBA, GL_FLOAT, mixed_pixel);
    assert(fabsf(mixed_pixel[0]) < .008f);
    assert(glGetError() == GL_NO_ERROR);
    glUniform4f(mixed_fetch->clip[0], 0, 0, 0, 0);
    glUniform4f(mixed_fetch->clip[1], 0, 0, 0, 0);
    glUniform4f(glGetUniformLocation(mixed, "amount"), .2, 0, 0, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0, 0, 0, .5);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(gles_fetch_draw(mixed_fetch, GL_TRIANGLES, 6, NULL, 0));
    glReadPixels(12, 8, 1, 1, GL_RGBA, GL_FLOAT, mixed_pixel);
    assert(fabsf(mixed_pixel[0] - .2f) < .008f &&
           fabsf(mixed_pixel[3] - .5f) < .008f);
    glDisable(GL_BLEND);
    assert(glGetError() == GL_NO_ERROR);
    gles_fetch_forget(mixed);
    /* A renderbuffer colour target keeps its depth/stencil, masks, scissor
     * and independently bound read framebuffer during the barrier resolve. */
    GLuint render_fbo, render_color, render_depth;
    glGenFramebuffersEXT(1, &render_fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, render_fbo);
    glGenRenderbuffersEXT(1, &render_color);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, render_color);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 32, 32);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, render_color);
    glGenRenderbuffersEXT(1, &render_depth);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, render_depth);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, 32,
                             32);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, render_depth);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, render_depth);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) ==
           GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 32, 32);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1);
    glClearStencil(0);
    glDepthMask(GL_TRUE);
    glStencilMask(255);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "amount"), .25, .1, 0, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, repeated);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 3, 255);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 16, 16);
    glColorMask(1, 0, 0, 0);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, fb);
    GLuint query, samples;
    glGenQueries(1, &query);
    glBeginQuery(GL_SAMPLES_PASSED, query);
    assert(gles_fetch_draw(fetch, GL_TRIANGLES, 6, NULL, 0));
    glEndQuery(GL_SAMPLES_PASSED);
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);
    assert(samples == 256);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &state);
    assert(state == fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &state);
    assert(state == render_fbo);
    glGetFramebufferAttachmentParameterivEXT(
        GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &state);
    assert(state == GL_RENDERBUFFER_EXT);
    glGetFramebufferAttachmentParameterivEXT(
        GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT, &state);
    assert(state == render_color);
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    assert(mask[0] && !mask[1] && !mask[2] && !mask[3]);
    assert(glIsEnabled(GL_SCISSOR_TEST));
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, render_fbo);
    uint8_t pixel[4], stencil;
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(abs(pixel[0] - 64) <= 1 && pixel[1] == 0 && pixel[2] == 0 &&
           pixel[3] == 255);
    glReadPixels(24, 24, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(pixel[0] == 0 && pixel[3] == 255);
    glReadPixels(8, 8, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, &stencil);
    assert(stencil == 3);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, fb);
    glDepthFunc(GL_NEVER);
    glBeginQuery(GL_SAMPLES_PASSED, query);
    assert(gles_fetch_draw(fetch, GL_TRIANGLES, 6, NULL, 0));
    glEndQuery(GL_SAMPLES_PASSED);
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);
    assert(samples == 0);
    assert(glGetError() == GL_NO_ERROR);
    gles_fetch_forget(lod);
    gles_fetch_forget(program);
    assert(!gles_fetch_program(program));
    batch_regression();
    gles_link_program(0);
    assert(glGetError() != GL_NO_ERROR);
    puts(
        "PASS: coherent GPU framebuffer fetch, arrays/indices, strip/fan winding, float color, hidden uniforms and restored texture state");
    return 0;
}
