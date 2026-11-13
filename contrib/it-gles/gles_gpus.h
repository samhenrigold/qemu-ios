/*
 * What each GPU the boards have reports, as its stock driver builds it: the GL_EXTENSIONS list and glGet's
 * implementation limits, one column per GPU and API, and which of those extensions the bridge implements. Both front
 * ends include it after the mbxshim core: contrib/gles-public/opengles.c (2.x on) and gles1x.c (1.x), each with its
 * own fe_gpu(gc), the column a context answers from.
 *
 * The source is the drivers' own code, decoded from 12 firmwares: iPhone OS 1.0 and 1.1.4's OpenGLES
 * (BuildExtensionString's table and mask, _DoGet), 3.1.3's MBXGLEngine (_GLESPublicExtensions, _DoGet), the SGX 535's
 * GLEngine and IMGSGX535GLDriver on 3.2.2, 4.0, 4.2.1, 4.3.5, 5.0.1, 5.1.1, 6.1.6 and 7.1.2 (glGetString_Exec's
 * switch over the driver's capability mask, then the {name, apis, bit} table it filters; _gleGetState over
 * glrSetConfigData's constants), and the SGX 543MP3's on 10.3.4 (IMGSGX543RC2GLDriver). Not covered: MBX 2.x and
 * 4.x, the SGX 535 on 3.0 and 3.1, the 543 on 6.x to 9.x.
 */

/* The columns: an MBX Lite's on 2.x to 4.x (ES 1.1 only), the SGX 535's (iPhone 3GS, iPod touch 3G and 4G, iPhone 4,
 * iPad) and the SGX 543MP3's (iPhone 5) in an ES 1.1 and an ES 2.0 context, and 1.x's MBX, whose OpenGLES is the
 * driver. FE_MBX, FE_SGX1 and FE_SGX2 keep the values the front end's per-column caches were indexed by. */
enum { FE_MBX, FE_SGX1, FE_SGX2, FE_543_1, FE_543_2, FE_MBX1, FE_NGPU };
#define FE_ES2(gpu) ((gpu) == FE_SGX2 || (gpu) == FE_543_2)

#define FE_C_MBX  (1u << FE_MBX)
#define FE_C_SGX1 (1u << FE_SGX1)
#define FE_C_SGX2 (1u << FE_SGX2)
#define FE_C_5431 (1u << FE_543_1)
#define FE_C_5432 (1u << FE_543_2)
#define FE_C_MBX1 (1u << FE_MBX1)
#define FE_C_ES1  (FE_C_SGX1 | FE_C_5431)
#define FE_C_ES2  (FE_C_SGX2 | FE_C_5432)
#define FE_C_SGX  (FE_C_ES1 | FE_C_ES2)
/* Already on the MBX in 2.2.1, whose engine is the OpenGLES the front end replaces: with no engine to read, only
 * these are given. */
#define FE_C_OLD  0x80u

/*
 * Every extension any of the drivers lists. `real`: the columns whose driver lists it in some firmware. Within a
 * column the front end gives a name only where this firmware's engine names it (MBXGLEngine, GLEngine), which picks
 * the version: checked against the decoded lists of 3.1.3 to 10.3.4, that gives each firmware's list exactly, except
 * IMG_texture_format_BGRA8888, which the SGX driver gave instead of APPLE_texture_format_BGRA8888 by a context flag
 * (3.2.2 to 4.2.1), so the SGX's column keeps the APPLE name alone. 1.x's column is its one list (1.0 and 1.1.4 are
 * the same). MBX 4.x's additions (APPLE_texture_format_BGRA8888, APPLE_texture_max_level, EXT_discard_framebuffer,
 * EXT_read_format_bgra) are Apple's platform guide's, as no MBX 4.x firmware was decoded.
 *
 * `impl`: the columns where the bridge implements it, and `need`: the host capabilities (GLES_CAP_*) it takes too.
 * A real extension the bridge does not implement keeps its row with no `impl` columns, so the gap stays in sight:
 * an app given a name uses it, and goes wrong further away. Not implemented: APPLE_copy_texture_levels,
 * APPLE_framebuffer_multisample, APPLE_rgb_422, the 543's packed float and sRGB formats, EXT_debug_label and
 * EXT_debug_marker (refused on the host), EXT_draw_instanced and EXT_instanced_arrays, EXT_separate_shader_objects
 * (no pipelines), EXT_shader_texture_lod (mapped to ARB in the GLSL, unchecked), EXT_shadow_samplers,
 * EXT_color_buffer_half_float (unchecked), OES_element_index_uint, and OES_vertex_array_object in ES 1.1 (the
 * bridge's vertex arrays hold ES 2.0's attributes, not the client arrays). OES_matrix_palette, which the Mac's GL
 * lacks, the bridge skins itself (gles_skin). Beyond the drivers' lists: APPLE_texture_rectangle and
 * APPLE_core_surface_texture on the MBX, the 3.1.3 engine's private pair (pnames 0x1F04, 0x8A1E), which 2.x
 * CoreAnimation needs before it composites with GL.
 */
static const struct { const char *name; unsigned char real, impl, need; } fe_ext_table[] = {
    { "GL_OES_mapbuffer",                   FE_C_MBX | FE_C_SGX,  FE_C_MBX | FE_C_SGX, GLES_CAP_MAP_BUFFER },
    { "GL_EXT_map_buffer_range",            FE_C_SGX,             FE_C_SGX, GLES_CAP_MAP_BUFFER },
    { "GL_APPLE_client_storage",            FE_C_MBX1,            FE_C_MBX1, 0 },   /* a hint the host ignores */
    { "GL_APPLE_clip_distance",             FE_C_5432,            FE_C_5432, 0 },
    { "GL_APPLE_color_buffer_packed_float", FE_C_5432,            0, 0 },
    { "GL_APPLE_copy_texture_levels",       FE_C_SGX,             0, 0 },
    { "GL_APPLE_core_surface_texture",      FE_C_MBX | FE_C_OLD,  FE_C_MBX, 0 },
    { "GL_APPLE_framebuffer_multisample",   FE_C_SGX,             0, 0 },
    { "GL_APPLE_rgb_422",                   FE_C_ES2,             0, 0 },
    { "GL_APPLE_sync",                      FE_C_ES2,             FE_C_ES2, 0 },
    { "GL_APPLE_texture_2D_limited_npot",   FE_C_ES1,             FE_C_ES1, 0 },
    { "GL_APPLE_texture_format_BGRA8888",   FE_C_MBX | FE_C_SGX,  FE_C_MBX | FE_C_SGX, 0 },
    { "GL_APPLE_texture_max_level",         FE_C_MBX | FE_C_SGX,  FE_C_MBX | FE_C_SGX, 0 },
    { "GL_APPLE_texture_packed_float",      FE_C_5432,            0, 0 },
    { "GL_APPLE_texture_rectangle",         FE_C_MBX | FE_C_MBX1 | FE_C_OLD, FE_C_MBX | FE_C_MBX1, 0 },
    { "GL_ARB_texture_env_combine",         FE_C_MBX1,            FE_C_MBX1, 0 },
    { "GL_ARB_texture_env_dot3",            FE_C_MBX1,            FE_C_MBX1, 0 },
    { "GL_EXT_blend_minmax",                FE_C_SGX,             FE_C_SGX, 0 },
    { "GL_EXT_color_buffer_half_float",     FE_C_5432,            0, 0 },
    { "GL_EXT_debug_label",                 FE_C_SGX,             0, 0 },
    { "GL_EXT_debug_marker",                FE_C_SGX,             0, 0 },
    { "GL_EXT_discard_framebuffer",         FE_C_MBX | FE_C_SGX,  FE_C_MBX | FE_C_SGX, 0 },
    { "GL_EXT_draw_instanced",              FE_C_ES2,             0, 0 },
    { "GL_EXT_instanced_arrays",            FE_C_ES2,             0, 0 },
    { "GL_EXT_occlusion_query_boolean",     FE_C_5432,            FE_C_5432, 0 },
    { "GL_EXT_pvrtc_sRGB",                  FE_C_5432,            0, 0 },
    { "GL_EXT_read_format_bgra",            FE_C_MBX | FE_C_SGX,  FE_C_MBX | FE_C_SGX, 0 },
    { "GL_EXT_sRGB",                        FE_C_5432,            0, 0 },
    { "GL_EXT_separate_shader_objects",     FE_C_ES2,             0, 0 },
    { "GL_EXT_shader_framebuffer_fetch",    FE_C_ES2,             FE_C_ES2, GLES_CAP_FRAMEBUFFER_FETCH },
    { "GL_EXT_shader_texture_lod",          FE_C_ES2,             0, 0 },
    { "GL_EXT_shadow_samplers",             FE_C_5432,            0, 0 },
    { "GL_EXT_texture_filter_anisotropic",  FE_C_MBX | FE_C_SGX | FE_C_OLD, FE_C_MBX | FE_C_SGX, 0 },
    { "GL_EXT_texture_lod_bias",            FE_C_MBX | FE_C_ES1 | FE_C_OLD, FE_C_MBX | FE_C_ES1, 0 },
    { "GL_EXT_texture_rg",                  FE_C_5432,            FE_C_5432, 0 },
    { "GL_EXT_texture_storage",             FE_C_SGX,             FE_C_SGX, 0 },
    { "GL_IMG_read_format",                 FE_C_MBX | FE_C_SGX | FE_C_OLD, FE_C_MBX | FE_C_SGX, 0 },
    { "GL_IMG_texture_compression_pvrtc",   FE_C_MBX | FE_C_SGX | FE_C_OLD, FE_C_MBX | FE_C_SGX, 0 },
    { "GL_IMG_texture_format_BGRA8888",     FE_C_MBX | FE_C_MBX1 | FE_C_OLD, FE_C_MBX | FE_C_MBX1, 0 },
    { "GL_OES_blend_equation_separate",     FE_C_ES1,             FE_C_ES1, 0 },
    { "GL_OES_blend_func_separate",         FE_C_ES1,             FE_C_ES1, 0 },
    { "GL_OES_blend_subtract",              FE_C_MBX | FE_C_ES1 | FE_C_OLD, FE_C_MBX | FE_C_ES1, 0 },
    { "GL_OES_byte_coordinates",            FE_C_MBX1,            FE_C_MBX1, 0 },
    { "GL_OES_compressed_paletted_texture", FE_C_MBX | FE_C_ES1 | FE_C_MBX1 | FE_C_OLD,
                                            FE_C_MBX | FE_C_ES1 | FE_C_MBX1, 0 },
    { "GL_OES_depth24",                     FE_C_MBX | FE_C_SGX | FE_C_OLD, FE_C_MBX | FE_C_SGX, 0 },
    { "GL_OES_depth_texture",               FE_C_ES2,             FE_C_ES2, 0 },
    { "GL_OES_draw_texture", FE_C_MBX | FE_C_ES1 | FE_C_MBX1 | FE_C_OLD, FE_C_MBX | FE_C_ES1 | FE_C_MBX1, 0 },
    { "GL_OES_element_index_uint",          FE_C_SGX,             0, 0 },
    { "GL_OES_fbo_render_mipmap",           FE_C_SGX,             FE_C_SGX, 0 },
    { "GL_OES_fixed_point",                 FE_C_MBX1,            FE_C_MBX1, 0 },
    { "GL_OES_framebuffer_object",          FE_C_MBX | FE_C_ES1 | FE_C_OLD, FE_C_MBX | FE_C_ES1, 0 },
    { "GL_OES_matrix_palette",              FE_C_MBX | FE_C_ES1,  FE_C_MBX | FE_C_ES1, 0 },   /* gles_skin */
    { "GL_OES_packed_depth_stencil",        FE_C_SGX,             FE_C_SGX, 0 },
    { "GL_OES_point_size_array", FE_C_MBX | FE_C_ES1 | FE_C_MBX1 | FE_C_OLD, FE_C_MBX | FE_C_ES1 | FE_C_MBX1, 0 },
    { "GL_OES_point_sprite", FE_C_MBX | FE_C_ES1 | FE_C_MBX1 | FE_C_OLD, FE_C_MBX | FE_C_ES1 | FE_C_MBX1, 0 },
    { "GL_OES_read_format", FE_C_MBX | FE_C_ES1 | FE_C_MBX1 | FE_C_OLD, FE_C_MBX | FE_C_ES1 | FE_C_MBX1, 0 },
    { "GL_OES_rgb8_rgba8",                  FE_C_MBX | FE_C_SGX | FE_C_OLD, FE_C_MBX | FE_C_SGX, 0 },
    { "GL_OES_single_precision",            FE_C_MBX1,            FE_C_MBX1, 0 },
    { "GL_OES_standard_derivatives",        FE_C_ES2,             FE_C_ES2, 0 },
    { "GL_OES_stencil8",                    FE_C_ES1,             FE_C_ES1, 0 },
    { "GL_OES_stencil_wrap",                FE_C_ES1,             FE_C_ES1, 0 },
    { "GL_OES_texture_float",               FE_C_ES2,             FE_C_ES2, 0 },
    { "GL_OES_texture_half_float",          FE_C_ES2,             FE_C_ES2, 0 },
    { "GL_OES_texture_half_float_linear",   FE_C_5432,            FE_C_5432, 0 },
    { "GL_OES_texture_mirrored_repeat",     FE_C_MBX | FE_C_ES1 | FE_C_OLD, FE_C_MBX | FE_C_ES1, 0 },
    { "GL_OES_vertex_array_object",         FE_C_SGX,             FE_C_ES2, 0 },
};

/* Where the `n` bytes at `tag` first appear in [base, base + size), or 0. */
static const char *fe_find(const char *base, unsigned long size, const char *tag, unsigned long n)
{
    unsigned long off;
    for (off = 0; base && off + n <= size; off++) {
        unsigned long t = 0;
        while (t < n && base[off + t] == tag[t]) t++;
        if (t == n) return base + off;
    }
    return 0;
}

/* Whether `text` names extension `name` whole: the engines keep each as a string, MBXGLEngine's with a trailing
 * space, so it starts a string and ends at a NUL or a space. */
static int fe_names(const char *text, unsigned long size, const char *name)
{
    unsigned long n = slen(name), off = 0;
    const char *p;
    while (off < size && (p = fe_find(text + off, size - off, name, n))) {
        unsigned long end = (unsigned long)(p - text) + n;
        if ((p == text || p[-1] == 0) && (end == size || text[end] == 0 || text[end] == ' ')) return 1;
        off = (unsigned long)(p - text) + 1;
    }
    return 0;
}

/* GL_EXTENSIONS for `gpu` into `out` (`cap` bytes): every extension real in its column that the bridge implements
 * there with the host's capabilities `caps`, and (but on 1.x) that its engine's __TEXT (`text`, 0 if none was found)
 * names. */
static void fe_extensions(char *out, unsigned cap, int gpu, unsigned caps, const char *text, unsigned long size)
{
    unsigned i, k, n = 0, col = 1u << gpu;
    for (i = 0; i < sizeof fe_ext_table / sizeof fe_ext_table[0]; i++) {
        const char *e = fe_ext_table[i].name;
        if (!(fe_ext_table[i].real & fe_ext_table[i].impl & col)) continue;
        if ((fe_ext_table[i].need & caps) != fe_ext_table[i].need) continue;
        if (gpu != FE_MBX1 && (text ? !fe_names(text, size, e) : !(fe_ext_table[i].real & FE_C_OLD))) continue;
        if (n + slen(e) + 1 >= cap) break;
        if (n) out[n++] = ' ';
        for (k = 0; e[k]; k++) out[n++] = e[k];
    }
    out[n] = 0;
}

/*
 * glGet's implementation limits, where the host's (the Mac's: 16384-texel textures and viewports, 16 texture units,
 * 1024 uniform vectors) are larger: an app sizes its atlases and picks its paths from these, and CoreAnimation tiles
 * any layer larger than MAX_TEXTURE_SIZE, as on the device. The host's answer is lowered to the GPU's, never raised:
 * the Mac's points stop at 64 pixels whatever the SGX's 511 says. A pair is a range (low, high) except VIEWPORT_DIMS;
 * -1: not this column's, or no value found (1.x's viewport is the driver's runtime one).
 *
 * MBX: MBXGLEngine's _DoGet (3.1.3); 1.x: OpenGLES's _DoGet (1.0, the same code in 1.1.4). SGX 535: _gleGetState over
 * glrSetConfigData's constants (3.2.2 and 4.2.1 traced, 5.1.1 in part; Apple's platform guide agrees). SGX 543MP3:
 * 10.3.4's getter, and where the getter was not traced (renderbuffer and cube map sizes, points, uniform and varying
 * vectors, texture image units) the 543 driver's stored constants at the fields the 535's getter reads.
 * MAX_SAMPLES_APPLE is 4 from 4.0 on the SGX (0 on 3.2.2): the host answers 0, as APPLE_framebuffer_multisample is
 * not implemented.
 */
static const struct { unsigned short pname, n; short v[FE_NGPU][2]; } fe_limit_table[] = {
    /* pname, values, { MBX, SGX ES 1.1, SGX ES 2.0, 543 ES 1.1, 543 ES 2.0, 1.x MBX } */
    { 0x0D33, 1, { { 1024 }, { 2048 }, { 2048 }, { 4096 }, { 4096 }, { 2048 } } }, /* MAX_TEXTURE_SIZE */
    { 0x84E8, 1, { { 1024 }, { 2048 }, { 2048 }, { 4096 }, { 4096 }, { -1 } } }, /* MAX_RENDERBUFFER_SIZE */
    { 0x851C, 1, { { -1 }, { 2048 }, { 2048 }, { 4096 }, { 4096 }, { -1 } } }, /* MAX_CUBE_MAP_TEXTURE_SIZE */
    /* MAX_VIEWPORT_DIMS */
    { 0x0D3A, 2, { { 1024, 1024 }, { 2048, 2048 }, { 2048, 2048 }, { 4096, 4096 }, { 4096, 4096 }, { -1 } } },
    /* ALIASED_POINT_SIZE_RANGE */
    { 0x846D, 2, { { 1, 64 }, { 1, 511 }, { 1, 511 }, { 1, 511 }, { 1, 511 }, { 1, 32 } } },
    { 0x846E, 2, { { 1, 64 }, { 1, 16 }, { 1, 16 }, { 1, 16 }, { 1, 16 }, { 1, 32 } } }, /* ALIASED_LINE_WIDTH_RANGE */
    { 0x0B12, 2, { { 1, 64 }, { 1, 511 }, { -1 }, { 1, 511 }, { -1 }, { 1, 1 } } }, /* SMOOTH_POINT_SIZE_RANGE */
    { 0x0B22, 2, { { 1, 1 }, { 1, 16 }, { -1 }, { 1, 16 }, { -1 }, { 1, 1 } } }, /* SMOOTH_LINE_WIDTH_RANGE */
    { 0x0D50, 1, { { 4 }, { 4 }, { 4 }, { -1 }, { -1 }, { -1 } } }, /* SUBPIXEL_BITS */
    { 0x84E2, 1, { { 2 }, { 8 }, { -1 }, { 8 }, { -1 }, { 2 } } }, /* MAX_TEXTURE_UNITS */
    { 0x0D32, 1, { { 1 }, { 6 }, { -1 }, { -1 }, { -1 }, { 1 } } }, /* MAX_CLIP_PLANES */
    { 0x0D31, 1, { { 8 }, { 8 }, { -1 }, { 8 }, { -1 }, { 8 } } }, /* MAX_LIGHTS */
    { 0x0D36, 1, { { 16 }, { 16 }, { -1 }, { 16 }, { -1 }, { 16 } } }, /* MAX_MODELVIEW_STACK_DEPTH */
    { 0x0D38, 1, { { 2 }, { 2 }, { -1 }, { 2 }, { -1 }, { -1 } } }, /* MAX_PROJECTION_STACK_DEPTH */
    { 0x0D39, 1, { { 4 }, { 4 }, { -1 }, { 4 }, { -1 }, { 4 } } }, /* MAX_TEXTURE_STACK_DEPTH */
    { 0x8842, 1, { { 9 }, { 11 }, { -1 }, { 11 }, { -1 }, { -1 } } }, /* MAX_PALETTE_MATRICES_OES */
    { 0x86A4, 1, { { 3 }, { 4 }, { -1 }, { 4 }, { -1 }, { -1 } } }, /* MAX_VERTEX_UNITS_OES */
    { 0x84FD, 1, { { 2 }, { 4 }, { 4 }, { 4 }, { 4 }, { -1 } } }, /* MAX_TEXTURE_LOD_BIAS_EXT */
    { 0x84FF, 1, { { 2 }, { 2 }, { 2 }, { 2 }, { 2 }, { -1 } } }, /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */
    { 0x8D57, 1, { { -1 }, { 4 }, { 4 }, { 4 }, { 4 }, { -1 } } }, /* MAX_SAMPLES_APPLE */
    { 0x86A2, 1, { { 14 }, { 14 }, { 4 }, { 14 }, { 4 }, { 14 } } }, /* NUM_COMPRESSED_TEXTURE_FORMATS */
    { 0x8869, 1, { { -1 }, { -1 }, { 16 }, { -1 }, { 16 }, { -1 } } }, /* MAX_VERTEX_ATTRIBS */
    { 0x8DFB, 1, { { -1 }, { -1 }, { 128 }, { -1 }, { 128 }, { -1 } } }, /* MAX_VERTEX_UNIFORM_VECTORS */
    { 0x8DFC, 1, { { -1 }, { -1 }, { 8 }, { -1 }, { 8 }, { -1 } } }, /* MAX_VARYING_VECTORS */
    { 0x8DFD, 1, { { -1 }, { -1 }, { 64 }, { -1 }, { 64 }, { -1 } } }, /* MAX_FRAGMENT_UNIFORM_VECTORS */
    { 0x8872, 1, { { -1 }, { -1 }, { 8 }, { -1 }, { 8 }, { -1 } } }, /* MAX_TEXTURE_IMAGE_UNITS */
    { 0x8B4C, 1, { { -1 }, { -1 }, { 0 }, { -1 }, { -1 }, { -1 } } }, /* MAX_VERTEX_TEXTURE_IMAGE_UNITS */
    { 0x8B4D, 1, { { -1 }, { -1 }, { 8 }, { -1 }, { -1 }, { -1 } } }, /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
    { 0x8824, 1, { { -1 }, { -1 }, { 1 }, { -1 }, { 1 }, { -1 } } }, /* MAX_DRAW_BUFFERS */
};

/* How many values `pname` has if it is one of `gpu`'s limits, else 0; then clamp the host's answer `v` to it. */
static unsigned fe_limit(int gpu, unsigned pname, float v[2], int clamp)
{
    unsigned i;
    for (i = 0; i < sizeof fe_limit_table / sizeof fe_limit_table[0]; i++) {
        const short *hw = fe_limit_table[i].v[gpu];
        unsigned n = fe_limit_table[i].n;
        if (fe_limit_table[i].pname != pname || hw[0] < 0) continue;
        if (!clamp) return n;
        if (n == 2 && pname != 0x0D3A) {                    /* a range: raise its low end, lower its high */
            if (v[0] < hw[0]) v[0] = hw[0];
            if (v[1] > hw[1]) v[1] = hw[1];
        } else {
            if (v[0] > hw[0]) v[0] = hw[0];
            if (n == 2 && v[1] > hw[1]) v[1] = hw[1];
        }
        return n;
    }
    return 0;
}

/* glGetIntegerv, glGetFloatv and glGetBooleanv: the host's answer, clamped to the GPU's limit. */
static int fe_getIntegerv(void *gc, unsigned pname, unsigned params)
{
    int *p = (int *)(unsigned long)params, r = (int)qc(GLES_ID_glGetIntegerv, gc, 2, A(pname, params));
    unsigned n = fe_limit(fe_gpu(gc), pname, 0, 0);
    float v[2];
    if (r || !p || !n) return r;
    v[0] = p[0]; v[1] = n == 2 ? p[1] : 0;
    fe_limit(fe_gpu(gc), pname, v, 1);
    p[0] = (int)v[0];
    if (n == 2) p[1] = (int)v[1];
    return r;
}
static int fe_getFloatv(void *gc, unsigned pname, unsigned params)
{
    float *p = (float *)(unsigned long)params;
    int r = (int)qc(GLES_ID_glGetFloatv, gc, 2, A(pname, params));
    unsigned n = fe_limit(fe_gpu(gc), pname, 0, 0);
    if (r || !p || !n) return r;
    if (n == 1) { float v[2] = { p[0], 0 }; fe_limit(fe_gpu(gc), pname, v, 1); p[0] = v[0]; }
    else fe_limit(fe_gpu(gc), pname, p, 1);
    return r;
}
static int fe_getBooleanv(void *gc, unsigned pname, unsigned params)
{
    unsigned char *p = (unsigned char *)(unsigned long)params;
    unsigned n = fe_limit(fe_gpu(gc), pname, 0, 0);
    int v[2], r;
    if (!n) return (int)qc(GLES_ID_glGetBooleanv, gc, 2, A(pname, params));
    if ((r = fe_getIntegerv(gc, pname, (unsigned)(unsigned long)v)) || !p) return r;
    p[0] = v[0] != 0;
    if (n == 2) p[1] = v[1] != 0;
    return r;
}
