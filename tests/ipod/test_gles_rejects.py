#!/usr/bin/env python3
"""The GL bridge's refusal counters (host and shim sides), and gles-debug's magenta paint.

The A008 popover shadow drew as a black box for days because refusals were log lines. Now every
refusal is a named counter (gles_host_rejects), logged once, the shim's own reports ride the log
channel as "[gles-reject] NAME N" with N a floor, and gles-debug=on paints a refused texture magenta.
"""
from gles_harness import root, src, function, PRELUDE, build_and_run

shim = (root / 'contrib/it-gles/mbxshim.c').read_text()
reporting = shim[shim.index('static char *put_dec('):shim.index('__attribute__((visibility("hidden"))) int gles_unimpl')]
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
''' + reporting + r'''
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
    refused("unimpl:slot", "", 807);
    assert(!strcmp(shimlog, "[gles-reject] shim:unimpl:glFogxv 1\n[gles-reject] shim:unimpl:glFogxv 2\n"
                            "[gles-reject] shim:unimpl:glFogxv 4\n[gles-reject] shim:unimpl:glFogxv 8\n"
                            "[gles-reject] shim:unimpl:slot:807 1\n"));
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
    puts("PASS: refusal counters, shim report lines, fourcc, format tables, magenta paint");
    return 0;
}
'''
build_and_run(code, 'it-gles-rejects-')
