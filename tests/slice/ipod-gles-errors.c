/* The guest's glGetError sees what the GL did with its calls, as on the device.
 *
 * Strict mode drained the host's glGetError around every forwarded call and only logged what it found, and a refused
 * entry point (the host's unhandled slot, the shim's by-name stub) returned 0 with no error, so an app could not tell
 * a call the host refused from one that worked. Now the host's error is the context's error (the first one, until
 * glGetError reads it), in ES terms, and a refusal sets INVALID_OPERATION; the log line and the gles-rejects count
 * stay as they were.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c fn gles_reject gles_es_error gles_take_error gles_host_error
 * SLICE:cases hw/arm/gles-host.c range case GLES_SLOT_GET_ERROR: | case GLES_SLOT_LOAD_MATRIXX:
 * SLICE:cases hw/arm/gles-host.c range case GLES_OP_SET_ERROR: | case GLES_OP_DRAWABLE_STORAGE:
 * SLICE:unhandled hw/arm/gles-host.c range         if (gles_refuse("slot:%u", slot)) { |         if (slot == GLES_ID_glDrawRangeElements)
 * SLICE:shim contrib/it-gles/gles_dispatch.c fn gles_unsupported gles_unknown_slot gles_stub
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
int gles_guest_rw(CPUState *cpu, vaddr a, void *p, size_t n, bool write) { return -1; }
#include "slice.h"

static int64_t call(uint32_t slot, uint32_t argc, const uint32_t *a)
{
    switch (slot) {
#include "cases.h"
    default:
#include "unhandled.h"
        return 0;
    }
}

/* the shim's side: what it sends the host */
#define GLES_MAX_SLOTS 848
static struct { const char *field[GLES_MAX_SLOTS]; } gli;
static const struct { unsigned id; } gles_fns[] = { { GLES_ID_glFogx } };
static int gles_row_of_name(const char *name) { return strcmp(name, "glFogx") ? -1 : 0; }
static void w(const char *s) { (void)s; }
static void wd(unsigned v) { (void)v; }
static void refused(const char *what, const char *name, unsigned num) { (void)what; (void)name; (void)num; }
static int inert_stub(const char *name) { return !strcmp(name, "glDiscardFramebufferEXT"); }
#define A(...) (const unsigned[]){ __VA_ARGS__ }
static unsigned sent_slot, sent_argc, sent_arg, sent;
static long long qc(unsigned slot, void *gc, unsigned argc, const unsigned *args)
{
    assert(gc);
    sent++; sent_slot = slot; sent_argc = argc; sent_arg = argc ? args[0] : 0;
    return 0;
}
#include "shim.h"

int main(void)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj format; GLint count; CGLContextObj ctx;
    assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(format); assert(CGLSetCurrentContext(ctx) == kCGLNoError);
    uint32_t a[3] = { 0x8840 };

    /* the ES set: shared tokens pass, ES 2.0 has no stack errors, desktop-only ones are an unsupported operation */
    gh.api_version = 1;
    assert(gles_es_error(GL_INVALID_ENUM) == GL_INVALID_ENUM && gles_es_error(GL_OUT_OF_MEMORY) == GL_OUT_OF_MEMORY);
    assert(gles_es_error(GL_STACK_OVERFLOW) == GL_STACK_OVERFLOW && gles_es_error(0x0506) == 0x0506);
    assert(gles_es_error(0x8031) == GL_INVALID_OPERATION);                 /* TABLE_TOO_LARGE */
    gh.api_version = 2;
    assert(gles_es_error(GL_STACK_UNDERFLOW) == GL_INVALID_OPERATION && gles_es_error(GL_NO_ERROR) == GL_NO_ERROR);
    gh.api_version = 1;

    /* a forwarded call the host refuses (glEnable(GL_MATRIX_PALETTE_OES): INVALID_ENUM on the Mac) reaches the
     * guest, counted and logged as before; the first error stays until glGetError reads it */
    glEnable(0x8840);
    gles_host_error(GLES_ID_glEnable, 1, a);
    glLineWidth(-1);                                                       /* INVALID_VALUE, after it */
    gles_host_error(GLES_ID_glLineWidth, 1, a);
    char *out = gles_host_rejects();
    assert(strstr(out, "glerror:") && strstr(out, ":0x500\t1\n") && strstr(out, ":0x501\t1\n"));
    free(out);
    assert(call(GLES_SLOT_GET_ERROR, 0, a) == GL_INVALID_ENUM);
    assert(call(GLES_SLOT_GET_ERROR, 0, a) == GL_NO_ERROR && glGetError() == GL_NO_ERROR);
    /* an engine operation's host error is counted, not the app's */
    glEnable(0x8840);
    gles_host_error(GLES_OP_PRESENT, 0, a);
    assert(call(GLES_SLOT_GET_ERROR, 0, a) == GL_NO_ERROR);
    /* without strict mode the host's own flag answers, mapped */
    glPushMatrix();
    for (unsigned i = 0; i < 64; i++) glPushMatrix();
    gh.api_version = 2;
    assert(call(GLES_SLOT_GET_ERROR, 0, a) == GL_INVALID_OPERATION);
    gh.api_version = 1;
    while (glGetError() != GL_NO_ERROR) {
    }

    /* an entry point the host has no case for, and the shim's own refusal (GLES_OP_SET_ERROR) */
    assert(call(9999, 0, a) == 0 && call(GLES_SLOT_GET_ERROR, 0, a) == GL_INVALID_OPERATION);
    out = gles_host_rejects();
    assert(strstr(out, "slot:9999\t1\n"));
    free(out);
    uint32_t e[1] = { GL_INVALID_OPERATION };
    assert(call(GLES_OP_SET_ERROR, 1, e) == 0 && call(GLES_OP_SET_ERROR, 0, e) == -1);
    assert(call(GLES_SLOT_GET_ERROR, 0, a) == GL_INVALID_OPERATION && call(GLES_SLOT_GET_ERROR, 0, a) == GL_NO_ERROR);

    /* the shim sends it for a by-name or by-slot stub, never for the inert hint or without a context */
    int gc;
    assert(gles_stub(&gc, "glFogx") == 0 && sent == 1 && sent_slot == GLES_OP_SET_ERROR && sent_argc == 1 &&
           sent_arg == GL_INVALID_OPERATION);
    gli.field[830] = "fancy_new_field_APPLE";
    assert(gles_unknown_slot(&gc, 830) == 0 && sent == 2 && sent_slot == GLES_OP_SET_ERROR);
    assert(gles_stub(&gc, "glDiscardFramebufferEXT") == 0 && gles_stub(0, "glFogx") == 0 && sent == 2);
    puts("PASS: host errors and refusals reach the guest's glGetError, first error kept, in ES terms");
    return 0;
}
