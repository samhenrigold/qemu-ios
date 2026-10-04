#!/usr/bin/env python3
"""The OES_fixed_point setters/getters and APPLE_fence slots the bridge now forwards, against real CGL.

Every engine binary of these firmwares fills them (glFogx, glLightxv, glTexEnvx...); the host used
to answer UNHANDLED. Enum-valued parameters travel as they are, numbers are 16.16 both ways.
"""
from gles_harness import root, src, function, PRELUDE, build_and_run

cases = src[src.index('    /*\n     * OES_fixed_point, the rest of it'):src.index('    /* ---- lighting and fog ---- */')]
helpers = ''.join(function(n) for n in ('gles_x(uint32_t value)\n{', 'gles_f(uint32_t bits)\n{', 'gles_reject(',
                                        'gles_fetch_params(', 'gles_light_nparams(', 'gles_material_nparams(',
                                        'gles_texenv_nparams(', 'gles_texparam_nparams(', 'gles_pname_is_enum(',
                                        'gles_xparam(', 'gles_fetch_xparams(', 'gles_write_xparams('))
code = PRELUDE + helpers + r'''
static uint32_t guest[16];
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write) {
    if (addr != 0x1000 || n > sizeof(guest)) return -1;
    if (write) memcpy(guest, data, n); else memcpy(data, guest, n);
    return 0;
}
static int64_t dispatch(unsigned slot, const uint32_t *a) { CPUState *cpu = NULL; switch (slot) {
''' + cases + r'''
default: assert(0); return -1; } }
#define X(v) ((uint32_t)(int32_t)((v) * 65536.0f))
int main(void) {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj format; GLint count; CGLContextObj ctx;
    assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(format); assert(CGLSetCurrentContext(ctx) == kCGLNoError);
    float f[4]; GLint i;
    /* a number in 16.16, an enum as itself */
    assert(!dispatch(770, (uint32_t[]){ GL_FOG_DENSITY, X(0.25) }));
    glGetFloatv(GL_FOG_DENSITY, f); assert(f[0] == 0.25f);
    assert(!dispatch(770, (uint32_t[]){ GL_FOG_MODE, GL_LINEAR }));
    glGetIntegerv(GL_FOG_MODE, &i); assert(i == GL_LINEAR);
    guest[0] = X(0.5); guest[1] = X(0.25); guest[2] = X(1); guest[3] = X(0.125);
    assert(!dispatch(771, (uint32_t[]){ GL_FOG_COLOR, 0x1000 }));
    glGetFloatv(GL_FOG_COLOR, f); assert(f[0] == 0.5f && f[1] == 0.25f && f[2] == 1 && f[3] == 0.125f);
    assert(!dispatch(784, (uint32_t[]){ GL_LIGHT1, GL_DIFFUSE, 0x1000 }));
    glGetLightfv(GL_LIGHT1, GL_DIFFUSE, f); assert(f[0] == 0.5f && f[3] == 0.125f);
    assert(!dispatch(783, (uint32_t[]){ GL_LIGHT1, GL_SPOT_CUTOFF, X(45) }));
    glGetLightfv(GL_LIGHT1, GL_SPOT_CUTOFF, f); assert(f[0] == 45);
    assert(!dispatch(787, (uint32_t[]){ GL_FRONT, GL_SHININESS, X(12) }));
    glGetMaterialfv(GL_FRONT, GL_SHININESS, f); assert(f[0] == 12);
    assert(!dispatch(797, (uint32_t[]){ GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL }));
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &i); assert(i == GL_DECAL);
    assert(!dispatch(797, (uint32_t[]){ GL_TEXTURE_ENV, GL_RGB_SCALE, X(2) }));
    glGetTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE, f); assert(f[0] == 2);
    assert(!dispatch(781, (uint32_t[]){ GL_LIGHT_MODEL_TWO_SIDE, 1 }));
    glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, &i); assert(i == 1);
    assert(!dispatch(804, (uint32_t[]){ GL_POINT_SIZE_MIN, X(3) }));
    glGetFloatv(GL_POINT_SIZE_MIN, f); assert(f[0] == 3);
    guest[0] = X(1); guest[1] = X(0); guest[2] = X(0); guest[3] = X(-2);
    assert(!dispatch(766, (uint32_t[]){ GL_CLIP_PLANE0, 0x1000 }));
    /* the getters convert back: a 16.16 number, an enum as itself, a clip plane both ways */
    memset(guest, 0, sizeof(guest));
    assert(!dispatch(777, (uint32_t[]){ GL_LIGHT1, GL_DIFFUSE, 0x1000 }));
    assert(guest[0] == X(0.5) && guest[3] == X(0.125));
    assert(!dispatch(779, (uint32_t[]){ GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x1000 }));
    assert(guest[0] == GL_DECAL);
    assert(!dispatch(780, (uint32_t[]){ GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 0x1000 }));
    assert(guest[0] == GL_NEAREST_MIPMAP_LINEAR);
    assert(!dispatch(775, (uint32_t[]){ GL_CLIP_PLANE0, 0x1000 }));
    assert(guest[0] == X(1) && guest[3] == X(-2));
    assert(!dispatch(774, (uint32_t[]){ GL_CLIP_PLANE0, 0x1000 }));
    memcpy(f, guest, 16); assert(f[0] == 1 && f[3] == -2);
    /* a pname no *f takes is refused and counted, not passed on */
    assert(dispatch(784, (uint32_t[]){ GL_LIGHT1, 0xffff, 0x1000 }) == -1 && gh.error == GL_INVALID_ENUM);
    /* APPLE_fence passes through */
    assert(!dispatch(463, (uint32_t[]){ 2, 0x1000 }));
    assert(guest[0] && guest[1] && guest[0] != guest[1]);
    assert(!dispatch(465, (uint32_t[]){ guest[0] }));
    assert(dispatch(466, (uint32_t[]){ guest[0] }) == 1);
    assert(!dispatch(468, (uint32_t[]){ guest[0] }));
    assert(dispatch(467, (uint32_t[]){ guest[0] }) == 1);
    assert(!dispatch(464, (uint32_t[]){ 2, 0x1000 }));
    assert(dispatch(466, (uint32_t[]){ guest[0] }) == 0);
    assert(glGetError() == GL_NO_ERROR);
    char *rejects = gles_host_rejects();
    assert(!strcmp(rejects, "light:0x4001/0xffff\t1\n"));
    free(rejects);
    puts("PASS: OES_fixed_point setters and getters convert both ways, enums pass as themselves, fences pass through");
    return 0;
}
'''
build_and_run(code, 'it-gles-fixed-')
