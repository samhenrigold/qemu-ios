/* Query results come from visible/depth-rejected GPU draws, never forced true.
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c fn gles_reject gles_es2_write gles_es2_fetch
 * SLICE hw/arm/gles-host.c define GLES_MAX_STRING
 * SLICE hw/arm/gles-queries.h file
 * SLICE:snapbase hw/arm/gles-host-snapshot.c.inc range typedef struct { GByteArray | /* ---- per-context GL state
 * SLICE:qsnap hw/arm/gles-host-snapshot.c.inc fn snap_put_query snap_put_queries snap_get_queries snap_restore_queries
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
static uint8_t memory[4096];
int gles_guest_rw(CPUState *cpu, vaddr addr, void *p, size_t n, bool write)
{
    assert(addr >= 0x1000 && addr + n <= 0x2000);
    if (write)
        memcpy(memory + (addr - 0x1000), p, n);
    else
        memcpy(p, memory + (addr - 0x1000), n);
    return 0;
}
#include "slice.h"
#include "snapbase.h"
#include "qsnap.h"
static GLuint value(void)
{
    GLuint x;
    memcpy(&x, memory + 128, 4);
    return x;
}
int main(void)
{
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, 0};
    CGLPixelFormatObj pf;
    GLint count;
    CGLContextObj ctx;
    assert(!CGLChoosePixelFormat(attrs, &pf, &count));
    assert(!CGLCreateContext(pf, NULL, &ctx));
    CGLDestroyPixelFormat(pf);
    assert(!CGLSetCurrentContext(ctx));
    GLuint f, c, d;
    glGenFramebuffersEXT(1, &f);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, f);
    glGenRenderbuffersEXT(1, &c);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, c);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 32, 32);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT, c);
    glGenRenderbuffersEXT(1, &d);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, d);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT16, 32, 32);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, d);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) ==
           GL_FRAMEBUFFER_COMPLETE_EXT);
    uint32_t a[3] = {2, 0x1000};
    assert(!gles_query_call(NULL, 634, a));
    GLuint ids[2];
    memcpy(ids, memory, 8);
    assert(ids[0] && ids[1] && ids[0] != ids[1]);
    a[0] = ids[0];
    assert(!gles_query_call(NULL, 636, a));
    glViewport(0, 0, 32, 32);
    glClearDepth(1);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    for (int visible = 0; visible < 2; visible++) {
        glDepthFunc(visible ? GL_ALWAYS : GL_NEVER);
        a[0] = 0x8c2f;
        a[1] = ids[visible];
        assert(!gles_query_call(NULL, 637, a));
        a[1] = ids[1 - visible];
        assert(gles_query_call(NULL, 637, a) == -1);
        gh.error = 0;
        glBegin(GL_TRIANGLES);
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glVertex2f(0, 1);
        glEnd();
        a[0] = 0x8c2f;
        assert(!gles_query_call(NULL, 638, a));
        a[0] = ids[visible];
        a[1] = GL_QUERY_RESULT;
        a[2] = 0x1080;
        assert(!gles_query_call(NULL, 641, a));
        printf("visibility %d result %u\n", visible, value());
        assert(value() == visible);
        a[1] = GL_QUERY_RESULT_AVAILABLE;
        assert(!gles_query_call(NULL, 641, a));
        assert(value() == 1);
    }
    a[0] = ids[0];
    assert(gles_query_call(NULL, 636, a) == 1);
    a[0] = 2;
    a[1] = 0x1000;
    assert(!gles_query_call(NULL, 635, a));
    assert(!g_hash_table_size(gh.queries));
    a[0] = 1;
    a[1] = 0x1000;
    assert(!gles_query_call(NULL, 634, a));
    GLuint active;
    memcpy(&active, memory, 4);
    a[0] = 0x8c2f;
    a[1] = active;
    assert(!gles_query_call(NULL, 637, a));
    glDepthFunc(GL_ALWAYS);
    glBegin(GL_TRIANGLES);
    glVertex2f(-1, -1);
    glVertex2f(1, -1);
    glVertex2f(0, 1);
    glEnd();
    a[0] = 1;
    a[1] = 0x1000;
    assert(!gles_query_call(NULL, 634, a));
    assert(!gles_query_call(NULL, 635, a));
    SnapW w = {g_byte_array_new()};
    snap_put_queries(&w, &gh);
    gles_queries_free(&gh);
    memset(&gh, 0, sizeof(gh));
    SnapR r = {w.b->data, w.b->len, 0, false};
    snap_get_queries(&r, &gh);
    assert(!r.bad && r.off == r.n);
    snap_restore_queries(&gh);
    a[0] = 0x8c2f;
    assert(!gles_query_call(NULL, 638, a));
    a[0] = active;
    a[1] = GL_QUERY_RESULT;
    a[2] = 0x1080;
    assert(!gles_query_call(NULL, 641, a));
    assert(value() == 1);
    g_byte_array_free(w.b, true);
    /* Deleting an active name keeps its underlying query active until End. */
    a[0] = 0x8c2f;
    a[1] = active;
    assert(!gles_query_call(NULL, 637, a));
    memcpy(memory, &active, 4);
    a[0] = 1;
    a[1] = 0x1000;
    assert(!gles_query_call(NULL, 635, a));
    assert(gh.deleted_query && gh.active_query == active);
    w = (SnapW){g_byte_array_new()};
    snap_put_queries(&w, &gh);
    gles_queries_free(&gh);
    memset(&gh, 0, sizeof(gh));
    r = (SnapR){w.b->data, w.b->len, 0, false};
    snap_get_queries(&r, &gh);
    assert(!r.bad && gh.deleted_query);
    snap_restore_queries(&gh);
    g_byte_array_free(w.b, true);
    a[0] = active;
    assert(!gles_query_call(NULL, 636, a));
    a[0] = 0x8c2f;
    assert(!gles_query_call(NULL, 638, a));
    assert(!gh.deleted_query && !gh.active_query);
    assert(!glGetError());
    gles_queries_free(&gh);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    puts("GPU queries PASS");
    return 0;
}
