/* The front end reports the GPU the device has, not the Mac's: its strings, its extensions and its limits.
 *
 * glGetIntegerv answered the host's limits (MAX_TEXTURE_SIZE 16384, 8 texture units on the MBX, ES 2.0's 1024 uniform
 * vectors), an SGX's ES 1.1 context called itself "PowerVR MBX", and the extension lists were a handful of names, so
 * an app could take a path no device takes. The strings come from the firmware's own engine (MBXGLEngine; the SGX
 * driver and GLEngine). The extensions and limits are gles_gpus.h's, one column per GPU and API as each stock driver
 * builds them: an extension is listed where it is real in the column, the bridge implements it there and the engine
 * names it; each limit is clamped to the GPU's. The excerpts below are as the 3.1.3 MBXGLEngine, the 5.1.1 and 7.1.2
 * SGX drivers and the 4.2.1, 6.1.6 and 10.3.4 GLEngines keep them.
 *
 * SLICE:fe contrib/it-gles/gles_gpus.h range enum { FE_MBX | /* glGetIntegerv, glGetFloatv
 * SLICE:fe contrib/gles-public/opengles.c fn fe_cstring fe_version
 */
#include <assert.h>
#define GLES_CAP_FRAMEBUFFER_FETCH 1u   /* include/hw/arm/guest-services/gles.h */
#define GLES_CAP_MAP_BUFFER 2u
#include <stdio.h>
#include <string.h>
static unsigned slen(const char *s) { return (unsigned)strlen(s); }
#include "fe.h"
static unsigned fe_caps;

static const char *ext(int gpu, const char *text, unsigned long size)
{
    static char out[1024];
    fe_extensions(out, sizeof out, gpu, fe_caps, text, size);
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
    /* OES_matrix_palette is the MBX's, which the bridge skins itself */
    assert(!strcmp(ext(FE_MBX, mbx, sizeof mbx),
                   "GL_APPLE_texture_rectangle GL_IMG_texture_compression_pvrtc GL_OES_framebuffer_object "
                   "GL_OES_matrix_palette"));
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
    /* Original N42 11A465 543RC2 driver's identity (the build omits RC2). */
    static const char sgx543[] = "PowerVR SGX 543\0IMGSGX543GLDriver\0IMGSGX543-97\0";
    assert(!strcmp(fe_cstring(sgx543, sizeof sgx543, "PowerVR "), "PowerVR SGX 543"));
    assert(!strcmp(ver(FE_SGX2, sgx543, sizeof sgx543), "OpenGL ES 2.0 IMGSGX543-97"));
    static const char sgx554[] = "IMGSGX554-115.15\0";
    assert(!strcmp(ver(FE_SGX2, sgx554, sizeof sgx554), "OpenGL ES 2.0 IMGSGX554-115.15"));
    static const char bad[] = "IMGSGX543GLDriver\0IMGSGX543-\0";
    assert(!strcmp(ver(FE_SGX2, bad, sizeof bad), "OpenGL ES 2.0"));

    /* GLEngine: ES 1.1 and 2.0 take different lists from the same names; 4.2.1 has no APPLE_sync, 6.1.6 has it. */
    static const char ge421[] = "GL_ARB_shadow\0GL_OES_depth_texture\0GL_OES_packed_depth_stencil\0"
                                "GL_IMG_texture_compression_pvrtc\0GL_OES_draw_texture\0GL_OES_vertex_array_object\0"
                                "OpenGL ES GLSL ES 1.0\0";
    static const char ge616[] = "GL_APPLE_sync\0GL_OES_texture_float\0GL_OES_draw_texture\0";
    /* OES_vertex_array_object (4.0 on): the bridge's vertex arrays hold ES 2.0's attributes, so ES 2.0 alone */
    assert(!strcmp(ext(FE_SGX2, ge421, sizeof ge421),
                   "GL_IMG_texture_compression_pvrtc GL_OES_depth_texture GL_OES_packed_depth_stencil "
                   "GL_OES_vertex_array_object"));
    assert(!strcmp(ext(FE_SGX1, ge421, sizeof ge421),
                   "GL_IMG_texture_compression_pvrtc GL_OES_draw_texture GL_OES_packed_depth_stencil"));
    /* 5.1.1 (9B206): OES_matrix_palette, which the bridge skins itself, is ES 1.1's only */
    static const char ge511[] = "GL_OES_mapbuffer\0GL_OES_matrix_palette\0GL_OES_packed_depth_stencil\0";
    assert(!strcmp(ext(FE_SGX1, ge511, sizeof ge511), "GL_OES_matrix_palette GL_OES_packed_depth_stencil"));
    assert(!strcmp(ext(FE_SGX2, ge511, sizeof ge511), "GL_OES_packed_depth_stencil"));
    assert(!strcmp(ext(FE_SGX2, ge616, sizeof ge616), "GL_APPLE_sync GL_OES_texture_float"));
    /* Buffer mapping is advertised only where the host reports it (the A6's), whatever the engine names. */
    static const char maps[] = "GL_OES_mapbuffer\0GL_EXT_map_buffer_range\0GL_APPLE_sync\0";
    assert(!strcmp(ext(FE_SGX2, maps, sizeof maps), "GL_APPLE_sync"));
    fe_caps = GLES_CAP_MAP_BUFFER;
    assert(!strcmp(ext(FE_SGX2, maps, sizeof maps), "GL_OES_mapbuffer GL_EXT_map_buffer_range GL_APPLE_sync"));
    fe_caps = 0;
    assert(!strcmp(fe_cstring(ge421, sizeof ge421, "OpenGL ES GLSL ES 1.0"), "OpenGL ES GLSL ES 1.0"));
    /* Real but not implemented: never listed, whatever the engine names. Implemented and real, now listed:
     * OES_stencil8 (ES 1.1: the host takes a stencil-only renderbuffer) and EXT_texture_storage (6.0 on). */
    static const char ge712[] = "GL_APPLE_framebuffer_multisample\0GL_OES_element_index_uint\0"
                                "GL_EXT_debug_marker\0GL_EXT_draw_instanced\0GL_OES_stencil8\0GL_EXT_texture_storage\0"
                                "GL_EXT_shader_framebuffer_fetch\0GL_EXT_texture_rg\0";
    assert(!strcmp(ext(FE_SGX1, ge712, sizeof ge712), "GL_EXT_texture_storage GL_OES_stencil8"));
    assert(!strcmp(ext(FE_SGX2, ge712, sizeof ge712), "GL_EXT_texture_storage"));
    /* the 535's ES 2.0 lists framebuffer fetch (6.x, 7.x) where the host implements it */
    fe_caps = GLES_CAP_FRAMEBUFFER_FETCH;
    assert(!strcmp(ext(FE_SGX2, ge712, sizeof ge712), "GL_EXT_shader_framebuffer_fetch GL_EXT_texture_storage"));
    fe_caps = 0;
    /* 10.3.4's GLEngine on the 543MP3: its own ES 2.0 additions, none of them the 535's */
    static const char ge1034[] = "GL_APPLE_clip_distance\0GL_EXT_sRGB\0GL_EXT_occlusion_query_boolean\0"
                                 "GL_EXT_texture_rg\0GL_OES_texture_half_float_linear\0GL_EXT_shadow_samplers\0"
                                 "GL_EXT_color_buffer_half_float\0GL_APPLE_sync\0GL_OES_stencil8\0";
    assert(!strcmp(ext(FE_543_2, ge1034, sizeof ge1034),
                   "GL_APPLE_clip_distance GL_APPLE_sync GL_EXT_occlusion_query_boolean GL_EXT_texture_rg "
                   "GL_OES_texture_half_float_linear"));
    assert(!strcmp(ext(FE_SGX2, ge1034, sizeof ge1034), "GL_APPLE_sync"));
    assert(!strcmp(ext(FE_543_1, ge1034, sizeof ge1034), "GL_OES_stencil8"));
    /* 1.x: its OpenGLES's own list, no engine to read */
    assert(!strcmp(ext(FE_MBX1, 0, 0),
                   "GL_APPLE_client_storage GL_APPLE_texture_rectangle GL_ARB_texture_env_combine "
                   "GL_ARB_texture_env_dot3 GL_IMG_texture_format_BGRA8888 GL_OES_byte_coordinates "
                   "GL_OES_compressed_paletted_texture GL_OES_draw_texture GL_OES_fixed_point GL_OES_point_size_array "
                   "GL_OES_point_sprite GL_OES_read_format GL_OES_single_precision"));
    /* the table: implemented only where real */
    for (unsigned i = 0; i < sizeof fe_ext_table / sizeof fe_ext_table[0]; i++)
        assert(!(fe_ext_table[i].impl & ~fe_ext_table[i].real));

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
    v[0] = 11; assert(fe_limit(FE_MBX, 0x8842, v, 1) == 1 && v[0] == 9);               /* MAX_PALETTE_MATRICES_OES */
    v[0] = 11; assert(fe_limit(FE_SGX1, 0x8842, v, 1) == 1 && v[0] == 11);
    v[0] = 4; assert(fe_limit(FE_MBX, 0x86A4, v, 1) == 1 && v[0] == 3);                /* MAX_VERTEX_UNITS_OES */
    assert(fe_limit(FE_SGX2, 0x86A4, v, 0) == 0);                                     /* not an ES 2.0 limit */
    v[0] = 16384; assert(fe_limit(FE_543_2, 0x0D33, v, 1) == 1 && v[0] == 4096);     /* the iPhone 5: 4096 */
    v[0] = 16384; assert(fe_limit(FE_543_1, 0x84E8, v, 1) == 1 && v[0] == 4096);     /* MAX_RENDERBUFFER_SIZE */
    v[0] = 16384; v[1] = 16384; assert(fe_limit(FE_543_2, 0x0D3A, v, 1) == 2 && v[0] == 4096 && v[1] == 4096);
    v[0] = 16384; assert(fe_limit(FE_MBX1, 0x0D33, v, 1) == 1 && v[0] == 2048);      /* 1.x */
    v[0] = 8; assert(fe_limit(FE_MBX1, 0x84E2, v, 1) == 1 && v[0] == 2);
    v[0] = 1; v[1] = 64; assert(fe_limit(FE_MBX1, 0x846D, v, 1) == 2 && v[1] == 32);
    v[0] = 16; assert(fe_limit(FE_SGX2, 0x84FF, v, 1) == 1 && v[0] == 2);              /* anisotropy: 2x on the SGX */
    v[0] = 16; assert(fe_limit(FE_543_1, 0x84FF, v, 1) == 1 && v[0] == 2);
    v[0] = 16; assert(fe_limit(FE_SGX2, 0x84FD, v, 1) == 1 && v[0] == 4);              /* LOD bias */
    v[0] = 14; assert(fe_limit(FE_SGX2, 0x86A2, v, 1) == 1 && v[0] == 4);              /* no palettes in ES 2.0 */
    v[0] = 14; assert(fe_limit(FE_SGX1, 0x86A2, v, 1) == 1 && v[0] == 14);
    v[0] = 0; assert(fe_limit(FE_SGX2, 0x8D57, v, 1) == 1 && v[0] == 0);               /* MAX_SAMPLES_APPLE stays 0 */
    v[0] = 512; assert(fe_limit(FE_MBX, 0x0D33, v, 1) == 1 && v[0] == 512);            /* a smaller host stays */
    assert(fe_limit(FE_SGX2, 0x0B50, v, 0) == 0);                                     /* not a limit */
    puts("PASS: the front end reports the device's GPU: strings, extensions and limits");
}
