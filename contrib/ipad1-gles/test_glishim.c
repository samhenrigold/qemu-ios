/* Host-side self-check of glishim's runtime dispatch discovery, table fill and context lifetime:
 *   cc -w test_glishim.c -o /tmp/t && /tmp/t
 * Run from contrib/ipad1-gles after build.sh (gles_stubs.h). The guest-services mcr is compiled out, so
 * every qc() returns 0; the @encode is one this test writes, in 3.2's shape (3.1.3's order with three
 * fields inserted at 761 and framebuffer_parameteri_APPLE last) plus a field no table knows. */
#define __volatile__(...)
#define __asm__
#include "glishim.c"
#include <assert.h>
#include <string.h>
#include <stdio.h>

/* Signatures as OpenGLES 3.2.2 calls them (userland-gl-display.md 3.1); a
 * mismatch is a compile error. The six unused entries are stubs taking (void). */
static void (*const c_init)(const void *, unsigned, unsigned, void *, void *, void *, void *) = gliInitializeLibrary;
static void (*const c_term)(void) = gliTerminateLibrary;
static int (*const c_choose)(GLIPixelFormat **, const int *) = gliChoosePixelFormat;
static void (*const c_dpf)(GLIPixelFormat *) = gliDestroyPixelFormat;
static int (*const c_create)(void **, GLIPixelFormat *, void *, void **, void **, unsigned) = gliCreateContext;
static int (*const c_create_sh)(void **, GLIPixelFormat *, void *, void **, void **, unsigned) = gliCreateContextWithShared;
static int (*const c_destroy)(void *) = gliDestroyContext;
static int (*const c_seti)(void *, unsigned, const int *) = gliSetInteger;
static int (*const c_geti)(void *, unsigned, int *) = gliGetInteger;
static int (*const c_ver)(int *, int *, int *) = gliGetVersion;
static int (*const c_bind)(void *, void *, unsigned char, int, int) = gliBindViewES;
static unsigned char (*const c_present)(void *) = gliPresentViewES;
static int (*const c_stubs[])(void) = { gliQueryRendererInfo, gliDestroyRendererInfo,
    gliAttachDrawable, gliAttachDrawableWithOptions, gliSwapBuffers,
    gliGetAttribute, gliSetAttribute, gliCopyAttributes };

static const gles_fn_t *row_of_id(unsigned id)
{
    for (unsigned i = 0; i < GLES_N_FNS; i++)
        if (gles_fns[i].id == id) return &gles_fns[i];
    return 0;
}

/* 3.2.2's layout: ids 0..760 in order, then the three 3.2 insertions, 761..821, the APPLE tail; plus
 * a field of our own at the end that no table knows. 827 slots. */
static char encode[64 * 1024];

static void make_encode(void)
{
    char *p = encode;
    p += sprintf(p, "{__GLIFunctionDispatchRec=");
    for (unsigned id = 0; id <= 821; id++) {
        if (id == 761)
            for (const char **f = (const char *[]){ "vertex_attrib_divisor", "draw_arrays_instanced", "draw_elements_instanced", 0 }; *f; f++)
                p += sprintf(p, "\"%s\"^?", *f);
        p += sprintf(p, "\"%s\"^?", row_of_id(id)->field);
    }
    p += sprintf(p, "\"framebuffer_parameteri_APPLE\"^?\"made_up_field\"^?}");
}

int main(void)
{
    void *front[1024], *back[1024], *root, *ctx, *ctx1;
    GLIPixelFormat *pf;

    make_encode();
    gles_encode_override = encode;
    assert(gliGetVersion(0, 0, 0) == 1);
    assert(gliChoosePixelFormat(&pf, 0) == 0 && pf->flags == 0);
    assert(gliCreateContext(&root, pf, 0, front, 0, 8) == 0);
    assert(gliCreateContext(&ctx, pf, root, front, back, 8) == 0);
    assert(((GuestGC *)ctx)->api == 2 && ((GuestGC *)ctx)->sg == ((GuestGC *)root)->sg);
    assert(!((GuestGC *)ctx)->owns_sg && ((GuestGC *)root)->owns_sg);

    /* the layout came from the @encode: 827 slots, one unknown */
    assert(gli.n == 827 && !strcmp(gli.how, "encode") && gli.fn[826] < 0 && !strcmp(gli.field[826], "made_up_field"));
    /* mbxshim's hand thunks, at this layout's slots for their ids (+3 from 761 on) */
    assert(gles_slot_of(GLES_ID_glOrthof) == 794 && front[794] == (void *)s_orthof);
    assert(gles_slot_of(GLES_ID_glAlphaFuncx) == 764 && front[764] == (void *)s_alphaFuncx);
    assert(gles_slot_of(GLES_ID_glClearDepthf) == 766 && front[766] == (void *)s_clearDepthf);
    assert(gles_slot_of(GLES_ID_glTexImage2D) == 301 && front[301] == (void *)s_texImage2D);
    assert(front[GLES_ID_glDrawTexfOES + 3] == (void *)s_drawTexf);
    /* generated forwarders and glishim's overrides (identical numbering below 761) */
    assert(front[600] == (void *)fn_glUseProgram && front[117] == (void *)gli_getString);
    assert(front[595] == (void *)gli_shaderSource && back[595] == front[595] && GLES_ID_glShaderSource == 595);
    /* the 3.2 insertions forward under their own ids; the APPLE tail too */
    assert(front[761] == (void *)fn_glVertexAttribDivisor && GLES_ID_glVertexAttribDivisor == 829);
    assert(front[825] == (void *)fn_glFramebufferParameteriAPPLE && gles_slot_of(GLES_ID_glFramebufferParameteriAPPLE) == 825);
    /* a field no table knows: the by-slot stub, which reports and returns 0 */
    assert(front[826] == gles_unknown_table[826] && ((int (*)(void *))front[826])(0) == 0);
    /* a row with no argc: the by-name stub */
    assert(gles_fn_ptr[gles_row_of_name("glDrawElementsBaseVertex")] == (void *)fn_glDrawElementsBaseVertex);
    assert(fn_glDrawElementsBaseVertex(0) == 0);
    for (int i = 0; i < 827; i++) assert(front[i] && front[i] == back[i]);
    /* an id this firmware has no slot for */
    assert(gles_slot_of(GLES_ID_glBindVertexArrayOES) == -1);
    /* batching by id: state setters yes, draws and queries no */
    assert(gles_batchable[GLES_ID_glEnable] && gles_batchable[GLES_ID_glUseProgram]);
    assert(!gles_batchable[GLES_ID_glDrawArrays] && !gles_batchable[GLES_ID_glGetError] && !gles_batchable[GLES_ID_glTexImage2D]);

    /* the exported-trampoline decoder on the two trampoline shapes glitsv.py reads */
    {
        /* armv6 3.x: mov lr, pc; ldr pc, [ip, #0x54]  (slot 17); then a pop */
        static const unsigned arm[] = { 0xE1A0E00F, 0xE59CF054, 0xE8BD8000 };
        /* armv7 Thumb-2: ldr.w r12, [r3, #0xa18]; bx r12  (slot 642, glBindBuffer) */
        static const unsigned short thumb[] = { 0xF8D3, 0xCA18, 0x4760 };
        assert(gles_trampoline_slot(arm, 0, 822) == 17);
        assert(gles_trampoline_slot(thumb, 1, 822) == 642);
        /* a GC load (0xc) and the TSD load (0xc0) are not table slots */
        static const unsigned gc[] = { 0xE593000C, 0xE59330C0, 0xE12FFF13 };
        assert(gles_trampoline_slot(gc, 0, 822) == -1);
        /* 5.x Thumb-2 glClear: ldr r0, [r2, #0x10] (the GC one word later); ldr r2, [r2, #0x3c]; blx r2 */
        static const unsigned short five[] = { 0x6910, 0x6BD2, 0x4790, 0xBD80 };
        assert(gles_trampoline_slot(five, 1, 905) == 10);
        /* 5.x glClearColor: ldr.w lr, [r0, #0x78]; ldr.w r0, [lr, #0x10]; ldr.w lr, [lr, #0x44]; blx lr */
        static const unsigned short five_f[] = { 0xF8D0, 0xE078, 0xF8DE, 0x0010, 0xF8DE, 0xE044, 0x47F0, 0xBD80 };
        assert(gles_trampoline_slot(five_f, 1, 905) == 12);
    }

    /* 5.x attaches of format-less IOSurfaces: the layout from the attach's GL format/type */
    {
        static const int la[8] = { 1, 0xde1, 0x190a, 8, 8, 0x190a, 0x1401, 0 };
        static const int bgra[8] = { 1, 0xde1, 0x1908, 8, 8, 0x80e1, 0x1401, 0 };
        static const int half[8] = { 1, 0xde1, 0x1908, 8, 8, 0x1908, 0x8d61, 0 };
        assert(gli_gl_fourcc(la) == 0x32433038 && gli_gl_fourcc(bgra) == 0x42475241 && !gli_gl_fourcc(half));
        assert(gfx_generation() == 0);          /* no libGFXShared in this process */
    }

    assert(!strcmp(gli_getString(ctx, 0x1F02), "OpenGL ES 2.0"));
    assert(gliCreateContext(&ctx1, pf, root, front, back, 4) == 0);
    assert(!strcmp(gli_getString(ctx1, 0x1F02), "OpenGL ES-CM 1.1"));

    /* 4.x: contexts of one EAGL sharegroup (keyed by its pixel format) share
     * a host sharegroup, released with the last of them */
    {
        GLIPixelFormat g1, g2;
        void *a1, *a2, *b1;
        assert(gliCreateContextWithShared(&a1, &g1, 0, front, back, 8) == 0);
        assert(gliCreateContextWithShared(&a2, &g1, 0, front, back, 0) == 0);
        assert(gliCreateContextWithShared(&b1, &g2, 0, front, back, 4) == 0);
        assert(((GuestGC *)a1)->sg == ((GuestGC *)a2)->sg && ((GuestGC *)a2)->api == 2);
        assert(((GuestGC *)b1)->sg != ((GuestGC *)a1)->sg && ((GuestGC *)b1)->api == 1);
        assert(gliDestroyContext(a1) == 0 && gli_groups[0].refs == 1);
        assert(gliDestroyContext(a2) == 0 && gli_groups[0].refs == 0);
        assert(gliDestroyContext(b1) == 0);
    }
    assert(gliSetInteger(ctx, 0x3E3, (int[]){1}) == 0);
    gliBindViewES(ctx, 0, 0, 0, 0);
    assert(gliDestroyContext(ctx) == 0 && gliDestroyContext(ctx1) == 0);
    assert(gliDestroyContext(root) == 0);
    gliDestroyPixelFormat(pf);
    puts("glishim self-check OK");
    return 0;
}
