#!/usr/bin/env python3
"""The GL bridge's refusal counters (host and shim sides), and gles-debug's magenta paint.

The A008 popover shadow drew as a black box for days because refusals were log lines. Now every
refusal is a named counter (gles_host_rejects), logged once, the shim's own reports ride the log
channel as "[gles-reject] NAME N" with N a floor, and gles-debug=on paints a refused texture magenta.
"""
from gles_harness import root, src, function, PRELUDE, build_and_run

shim = (root / 'contrib/it-gles/mbxshim.c').read_text()
reporting = shim[shim.index('static char *put_dec('):shim.index('static int guest_fault_read(')]
# The runtime dispatch's two stubs, lifted whole: a row the shim cannot forward, and a firmware
# dispatch field no gles-names.h row names. Both must report through refused().
dispatch = (root / 'contrib/it-gles/gles_dispatch.c').read_text()
assert '#include "gles_dispatch.c"' in shim
stubs = function('gles_unknown_slot(unsigned slot)\n{', dispatch) + function('gles_stub(const char *name)\n{', dispatch)
formats = src[src.index('/* The ES half-float type'):src.index('/* GL_UNPACK_ALIGNMENT in force')]


def definition(head):
    """The whole definition that starts with `head` (a forward declaration would match function())."""
    start = src.index(head + '\n{')
    return src[start:src.index('\n}', start) + 2]


code = '#define GLES_TEST_REAL_DEBUG 1\n' + PRELUDE + formats + function('gles_texture_object(') \
    + definition('static void gles_debug_texture(GLenum target)') + function('gles_surface_bpp(') + r'''
int gles_guest_rw(CPUState *cpu, vaddr a, void *p, size_t n, bool write) { return -1; }
static char shimlog[4096];
static void w(const char *s) { strlcat(shimlog, s, sizeof(shimlog)); }
static void wd(unsigned v) { char b[12]; snprintf(b, sizeof(b), "%u", v); w(b); }
#define GLES_MAX_SLOTS 848
static struct { const char *field[GLES_MAX_SLOTS]; } gli;
static const struct { unsigned id; } gles_fns[] = { { GLES_ID_glFogx } };
static int gles_row_of_name(const char *name) { return strcmp(name, "glFogx") ? -1 : 0; }
''' + reporting + stubs + r'''
int main(void)
{
    char *out;
    /* host side: counted per call, one log line per name, the same name from two sites shares a counter */
    assert(gles_refuse("teximage:0x%x/0x%x", 0x1906, 0x8033));
    assert(!gles_refuse("teximage:0x%x/0x%x", 0x1906, 0x8033));
    assert(!gles_host_refuse("teximage:0x%x/0x%x", 0x1906, 0x8033));
    assert(gles_refuse("slot:%u", 807));
    out = gles_host_rejects();
    assert(!strcmp(out, "slot:807\t1\nteximage:0x1906/0x8033\t3\n"));
    free(out);
    /* the shim's line: a floor the host keeps the largest of; anything else on the log channel is not one */
    assert(gles_shim_reject_line("[gles-reject] shim:unimpl:glFogxv 4\n"));
    assert(gles_shim_reject_line("[gles-reject] shim:unimpl:glFogxv 2\n"));
    assert(gles_shim_reject_line("[gles-reject] shim:surface:A008\n"));
    assert(!gles_shim_reject_line("[mbxshim] GLESCreateGC\n"));
    out = gles_host_rejects();
    assert(strstr(out, "shim:unimpl:glFogxv\t4\n") && strstr(out, "shim:surface:A008\t1\n"));
    free(out);
    /* four printable bytes, or hex */
    char f[12];
    assert(!strcmp(gles_fourcc(0x41303038, f), "A008"));
    assert(!strcmp(gles_fourcc(0x00000010, f), "0x00000010"));
    assert(gles_surface_bpp(0x41303038) == 1 && gles_surface_bpp(0x4c303038) == 1 && gles_surface_bpp(0x34343434) == 2 &&
           gles_surface_bpp(0x31353535) == 2 && gles_surface_bpp(0x41524742) == 4 && gles_surface_bpp(0x41424752) == 4 &&
           !gles_surface_bpp(0x34323076));
    /* the texture formats the firmwares' drivers accept, and their sizes */
    assert(gles_texel_bytes(GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV) == 2 && gles_texel_bytes(GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV) == 2);
    assert(gles_texel_bytes(GL_RGBA, GL_UNSIGNED_INT_8_8_8_8) == 4 && gles_texel_bytes(GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV) == 4);
    assert(gles_texel_bytes(GL_RGBA, GL_FLOAT) == 16 && gles_texel_bytes(GL_LUMINANCE_ALPHA, GLES_HALF_FLOAT_OES) == 4);
    assert(gles_texel_bytes(GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT) == 2 && gles_texel_bytes(GL_DEPTH_COMPONENT, GL_UNSIGNED_INT) == 4);
    assert(!gles_texel_bytes(GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE) && !gles_texel_bytes(GL_RGB, GL_UNSIGNED_SHORT_4_4_4_4) &&
           !gles_texel_bytes(GL_RGBA, GL_UNSIGNED_SHORT_5_6_5) && !gles_texel_bytes(0x8A1F, GL_UNSIGNED_BYTE));
    assert(gles_host_type(GLES_HALF_FLOAT_OES) == GL_HALF_FLOAT_ARB && gles_host_type(GL_FLOAT) == GL_FLOAT);
    /* shim side: reported at 1, 2, 4, 8 calls, once per name */
    for (unsigned i = 0; i < 9; i++) refused("unimpl:", "glFogxv", ~0u);
    refused("drawable:", "A008", 807);
    assert(!strcmp(shimlog, "[gles-reject] shim:unimpl:glFogxv 1\n[gles-reject] shim:unimpl:glFogxv 2\n"
                            "[gles-reject] shim:unimpl:glFogxv 4\n[gles-reject] shim:unimpl:glFogxv 8\n"
                            "[gles-reject] shim:drawable:A008:807 1\n"));
    /* the runtime dispatch's stubs: a row it cannot forward is counted by its gl name (and said once),
     * a firmware field no row names by that field, and the inert hint not at all */
    shimlog[0] = 0;
    gles_stub("glFogx"); gles_stub("glFogx");
    assert(strstr(shimlog, "[gles-reject] shim:unimpl:glFogx 1\n") && strstr(shimlog, "[gles-reject] shim:unimpl:glFogx 2\n"));
    {
        const char *said = strstr(shimlog, "unimplemented GL entry point glFogx");
        assert(said && !strstr(said + 1, "unimplemented GL entry point glFogx"));
    }
    shimlog[0] = 0;
    assert(!gles_stub("glDiscardFramebufferEXT") && !shimlog[0]);
    gli.field[830] = "fancy_new_field_APPLE";
    gles_unknown_slot(830); gles_unknown_slot(830);
    assert(strstr(shimlog, "(field fancy_new_field_APPLE, dispatch slot 830)") &&
           strstr(shimlog, "[gles-reject] shim:unimpl:field:fancy_new_field_APPLE 1\n") &&
           strstr(shimlog, "[gles-reject] shim:unimpl:field:fancy_new_field_APPLE 2\n"));
    assert(gles_shim_reject_line("[gles-reject] shim:unimpl:field:fancy_new_field_APPLE 2\n"));
    char t[5];
    assert(!strcmp(fourcc_text(0x41303038, t), "A008") && !strcmp(fourcc_text(0x00000010, t), "????"));
    assert(inert_stub("glDiscardFramebufferEXT") && !inert_stub("glDiscardFramebufferEX") && !inert_stub("glFogx"));
    /* gles-debug: a refused texture samples magenta; off, it is left alone */
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj format; GLint count; CGLContextObj ctx;
    assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(format); assert(CGLSetCurrentContext(ctx) == kCGLNoError);
    GLuint tex; uint8_t px[4] = { 1, 2, 3, 4 }, got[4];
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gles_debug_texture(GL_TEXTURE_2D);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, got);
    assert(!memcmp(got, px, 4));
    gles_host_set_debug(true);
    gles_debug_texture(GL_TEXTURE_2D);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, got);
    assert(!memcmp(got, "\xff\0\xff\xff", 4) && glGetError() == GL_NO_ERROR);
    /* and every newly accepted texture type is a real desktop upload: one texel each, read back as RGBA8 */
    {
        struct { GLenum fmt, type; uint8_t in[16]; uint8_t want[4]; } up[] = {
            { GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, {0x0f, 0xf0}, {0, 0, 0xff, 0xff} },   /* 0xf00f, REV: low nibble is B=f, top is A=f */
            { GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, {0x1f, 0x80}, {0, 0, 0xff, 0xff} },   /* 0x801f, REV: low 5 bits B=1f, bit 15 A=1 */
            { GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, {0x11, 0x22, 0x33, 0x44}, {0x44, 0x33, 0x22, 0x11} },
            { GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, {0x11, 0x22, 0x33, 0x44}, {0x33, 0x22, 0x11, 0x44} },
            { GL_LUMINANCE_ALPHA, GLES_HALF_FLOAT_OES, {0x00, 0x3c, 0x00, 0x38}, {0xff, 0xff, 0xff, 0x80} },  /* 1.0h, 0.5h */
            { GL_RGB, GL_FLOAT, {0,0,0x80,0x3f, 0,0,0,0x3f, 0,0,0,0}, {0xff, 0x80, 0, 0xff} },            /* 1.0f, 0.5f, 0 */
        };
        for (unsigned u = 0; u < sizeof(up) / sizeof(up[0]); u++) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, up[u].fmt, gles_host_type(up[u].type), up[u].in);
            assert(glGetError() == GL_NO_ERROR);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, got);
            for (unsigned c = 0; c < 4; c++) assert(abs((int)got[c] - up[u].want[c]) <= 1);
        }
        uint16_t depth = 0x8000; uint8_t d8[4];
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, &depth);
        assert(glGetError() == GL_NO_ERROR);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, d8);
        assert(d8[0] == 0x80);
    }
    puts("PASS: refusal counters, shim report lines, runtime-dispatch stubs, fourcc, format tables, magenta paint");
    return 0;
}
'''
build_and_run(code, 'it-gles-rejects-')
