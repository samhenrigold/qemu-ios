/* The front end reports the GPU the device has, not the Mac's: its strings, its extensions and its limits.
 *
 * glGetIntegerv answered the host's limits (MAX_TEXTURE_SIZE 16384, 8 texture units on the MBX, ES 2.0's 1024 uniform
 * vectors), an SGX's ES 1.1 context called itself "PowerVR MBX", and the extension lists were a handful of names, so
 * an app could take a path no device takes. The strings come from the firmware's own engine (MBXGLEngine; the SGX
 * driver and GLEngine), the extensions are the hardware's that the bridge implements and the engine names, and each
 * limit is clamped to the GPU's. The excerpts below are as the 3.1.3 MBXGLEngine, the 5.1.1 and 7.1.2 SGX drivers
 * and the 4.2.1 and 6.1.6 GLEngines keep them.
 *
 * SLICE:fe contrib/gles-public/opengles.c range enum { FE_MBX | static int fe_gpu
 * SLICE:fe contrib/gles-public/opengles.c fn fe_find fe_cstring fe_names
 * SLICE:fe contrib/gles-public/opengles.c range #define FE_X_MBX | /* The list for `gpu`
 * SLICE:fe contrib/gles-public/opengles.c fn fe_extensions fe_version
 * SLICE:fe contrib/gles-public/opengles.c range static const struct { unsigned short pname | /* How many values
 * SLICE:fe contrib/gles-public/opengles.c fn fe_limit
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned slen(const char *s) { return (unsigned)strlen(s); }
#include "fe.h"

static const char *ext(int gpu, const char *text, unsigned long size)
{
    static char out[1024];
    fe_extensions(out, sizeof out, gpu, text, size);
    return out;
}

static const char *ver(int gpu, const char *text, unsigned long size)
{
    static char out[64];
    return fe_version(out, sizeof out, gpu, text, size);
}

int main(void)
{
    /* 3.1.3 MBXGLEngine: its names end in a space; a longer name (OES_framebuffer_object_x) is not the extension. */
    static const char mbx[] = "Imagination Technologies\0PowerVR MBXLite with VGPLite\0OpenGL ES-CM 1.1 (48)\0"
                              "GL_OES_framebuffer_object_x \0GL_OES_framebuffer_object \0GL_OES_matrix_palette \0"
                              "GL_IMG_texture_compression_pvrtc \0GL_APPLE_texture_rectangle \0";
    assert(!strcmp(fe_cstring(mbx, sizeof mbx, "PowerVR "), "PowerVR MBXLite with VGPLite"));
    assert(!strcmp(ver(FE_MBX, mbx, sizeof mbx), "OpenGL ES-CM 1.1 (48)"));
    /* OES_matrix_palette is the MBX's but has no host implementation */
    assert(!strcmp(ext(FE_MBX, mbx, sizeof mbx),
                   "GL_APPLE_texture_rectangle GL_IMG_texture_compression_pvrtc GL_OES_framebuffer_object"));
    /* 2.x: the engine is the OpenGLES this replaces, so the 2.2.1 list the bridge implements, no 4.x additions */
    assert(strstr(ext(FE_MBX, 0, 0), "GL_OES_draw_texture") && !strstr(ext(FE_MBX, 0, 0), "discard"));
    assert(!strcmp(ver(FE_MBX, 0, 0), "OpenGL ES-CM 1.1"));

    /* SGX drivers: 5.1.1 keeps the whole version strings, 7.1.2 only the build that GLEngine appends. */
    static const char sgx511[] = "Imagination Technologies\0PowerVR SGX 535\0OpenGL ES 2.0 IMGSGX535-63.24\0"
                                 "OpenGL ES-CM 1.1 IMGSGX535-63.24\0";
    static const char sgx712[] = "Imagination Technologies\0PowerVR SGX 535\0IMGSGX535-97.7\0IMGSGX535GLDriver\0";
    assert(!strcmp(fe_cstring(sgx712, sizeof sgx712, "PowerVR "), "PowerVR SGX 535"));
    assert(!strcmp(ver(FE_SGX2, sgx511, sizeof sgx511), "OpenGL ES 2.0 IMGSGX535-63.24"));
    assert(!strcmp(ver(FE_SGX1, sgx511, sizeof sgx511), "OpenGL ES-CM 1.1 IMGSGX535-63.24"));
    assert(!strcmp(ver(FE_SGX1, sgx712, sizeof sgx712), "OpenGL ES-CM 1.1 IMGSGX535-97.7"));
    assert(!strcmp(ver(FE_SGX2, 0, 0), "OpenGL ES 2.0"));

    /* GLEngine: ES 1.1 and 2.0 take different lists from the same names; 4.2.1 has no APPLE_sync, 6.1.6 has it. */
    static const char ge421[] = "GL_ARB_shadow\0GL_OES_depth_texture\0GL_OES_packed_depth_stencil\0"
                                "GL_IMG_texture_compression_pvrtc\0GL_OES_draw_texture\0GL_OES_vertex_array_object\0"
                                "OpenGL ES GLSL ES 1.0\0";
    static const char ge616[] = "GL_APPLE_sync\0GL_OES_texture_float\0GL_OES_draw_texture\0";
    assert(!strcmp(ext(FE_SGX2, ge421, sizeof ge421),
                   "GL_IMG_texture_compression_pvrtc GL_OES_depth_texture GL_OES_packed_depth_stencil"));
    assert(!strcmp(ext(FE_SGX1, ge421, sizeof ge421),
                   "GL_IMG_texture_compression_pvrtc GL_OES_draw_texture GL_OES_packed_depth_stencil"));
    assert(!strcmp(ext(FE_SGX2, ge616, sizeof ge616), "GL_APPLE_sync GL_OES_texture_float"));
    assert(!strcmp(fe_cstring(ge421, sizeof ge421, "OpenGL ES GLSL ES 1.0"), "OpenGL ES GLSL ES 1.0"));

    /* Limits: the Mac's answers clamped to each GPU's. */
    float v[2];
    v[0] = 16384; assert(fe_limit(FE_MBX, 0x0D33, v, 1) == 1 && v[0] == 1024);       /* MAX_TEXTURE_SIZE */
    v[0] = 16384; assert(fe_limit(FE_SGX1, 0x0D33, v, 1) == 1 && v[0] == 2048);
    v[0] = 8; assert(fe_limit(FE_MBX, 0x84E2, v, 1) == 1 && v[0] == 2);               /* MAX_TEXTURE_UNITS */
    v[0] = 8; assert(fe_limit(FE_SGX1, 0x84E2, v, 1) == 1 && v[0] == 8);
    assert(fe_limit(FE_SGX2, 0x84E2, v, 0) == 0);                                     /* not an ES 2.0 limit */
    v[0] = 16384; v[1] = 16384; assert(fe_limit(FE_SGX2, 0x0D3A, v, 1) == 2 && v[0] == 2048 && v[1] == 2048);
    v[0] = 0.5f; v[1] = 64; assert(fe_limit(FE_SGX2, 0x846D, v, 1) == 2 && v[0] == 1 && v[1] == 64);  /* a range */
    v[0] = 1; v[1] = 2047; assert(fe_limit(FE_SGX2, 0x846D, v, 1) == 2 && v[1] == 511);
    v[0] = 1024; assert(fe_limit(FE_SGX2, 0x8DFB, v, 1) == 1 && v[0] == 128);          /* MAX_VERTEX_UNIFORM_VECTORS */
    v[0] = 1024; assert(fe_limit(FE_SGX2, 0x8DFD, v, 1) == 1 && v[0] == 64);
    v[0] = 31; assert(fe_limit(FE_SGX2, 0x8DFC, v, 1) == 1 && v[0] == 8);              /* MAX_VARYING_VECTORS */
    v[0] = 16; assert(fe_limit(FE_SGX2, 0x8872, v, 1) == 1 && v[0] == 8);              /* MAX_TEXTURE_IMAGE_UNITS */
    v[0] = 16; assert(fe_limit(FE_SGX2, 0x8B4C, v, 1) == 1 && v[0] == 0);              /* no vertex texture fetch */
    v[0] = 512; assert(fe_limit(FE_MBX, 0x0D33, v, 1) == 1 && v[0] == 512);            /* a smaller host stays */
    assert(fe_limit(FE_SGX2, 0x0B50, v, 0) == 0);                                     /* not a limit */
    puts("PASS: the front end reports the device's GPU: strings, extensions and limits");
}
