/* Host-side self-check of glishim's table fill and context lifetime:
 *   cc -w test_glishim.c -o /tmp/t && /tmp/t
 * Run from contrib/ipad1-gles after build.sh. The guest-services mcr is compiled out, so every qc() returns 0. */
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

static int slot_of(const char *name)
{
    for (int i = 0; i < GLI_N_SLOTS; i++)
        if (!strcmp(gli_slot_names[i], name)) return i;
    return -1;
}

int main(void)
{
    void *front[GLI_N_SLOTS], *back[GLI_N_SLOTS], *root, *ctx, *ctx1;
    GLIPixelFormat *pf;

    assert(gliGetVersion(0, 0, 0) == 1);
    assert(gliChoosePixelFormat(&pf, 0) == 0 && pf->flags == 0);
    assert(gliCreateContext(&root, pf, 0, front, 0, 8) == 0);
    assert(gliCreateContext(&ctx, pf, root, front, back, 8) == 0);
    assert(((GuestGC *)ctx)->api == 2 && ((GuestGC *)ctx)->sg == ((GuestGC *)root)->sg);
    assert(!((GuestGC *)ctx)->owns_sg && ((GuestGC *)root)->owns_sg);

    /* mbxshim's hand thunks, at this layout's slots for their 3.1.3 ones
     * (3.2: +3 from 761 on; 4.2.1: +16) */
    assert(front[GLI_SLOT_glOrthof] == (void *)s_orthof && front[GLI_SLOT_glAlphaFuncx] == (void *)s_alphaFuncx);
    assert(front[GLI_SLOT_glClearDepthf] == (void *)s_clearDepthf && front[301] == (void *)s_texImage2D);
    assert(gli_slot313[GLI_SLOT_glOrthof] == 791 && gli_slot313[GLI_SLOT_glAlphaFuncx] == 761);
    /* generated forwarders and overrides (identical numbering below 761) */
    assert(front[600] == (void *)g600 && front[117] == (void *)gli_getString);
    assert(front[595] == (void *)gli_shaderSource && back[595] == front[595] && GLI_SLOT_glShaderSource == 595);
    /* new since 3.1.3 / not ES: log-once stubs */
    int fbp = slot_of("glFramebufferParameteriAPPLE");
    assert(front[761] == gli_fwd_table[761] && gli_slot313[761] == -1 && front[0] == (void *)g0);
    assert(fbp > 0 && gli_slot313[fbp] == -1 && front[fbp] == gli_fwd_table[fbp]);
    for (int i = 0; i < GLI_N_SLOTS; i++) assert(front[i] && front[i] == back[i]);
    /* iPod main-line ES1 fills reach the matching slots */
    assert(front[GLI_SLOT_glDrawTexfOES] == (void *)s_drawTexf && gli_slot313[GLI_SLOT_glDrawTexfOES] == 817);
    /* a stub reports by name and returns 0 */
    assert(((int (*)(void *))front[fbp])(0) == 0);

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
