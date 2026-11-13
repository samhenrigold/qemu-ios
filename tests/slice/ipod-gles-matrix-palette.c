/* OES_matrix_palette, which the Mac's GL lacks, against real CGL: the bridge keeps the palette, applies the matrix
 * calls of MATRIX_PALETTE_OES mode to the current palette matrix, and skins a draw on the CPU.
 *
 * As SAMURAI BLOODSHOW (iOS 5.1.1 iPad) draws its units: matrix mode left at GL_TEXTURE, the palette enabled and
 * made the matrix mode, a matrix per bone, weight and matrix index arrays from a buffer object, then the draw. The
 * modelview, which the palette replaces, moves everything off screen, so an unskinned draw leaves the target black.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c define GLES_FIXED
 * SLICE hw/arm/gles-host.c fn gles_f gles_x gles_reject gles_buffer_destroy gles_buffer_bind gles_type_size gles_needs_widen gles_array_name gles_pointer_ok gles_array_layout_ok gles_set_pointer
 * SLICE hw/arm/gles-host.c fn gles_array_bytes gles_bind_array gles_elem gles_skin gles_bind_all_arrays gles_unbind_arrays
 * SLICE hw/arm/gles-host.c fn gles_palette_reset gles_palette_op gles_palette_call gles_compressed_count gles_query_count gles_unpack gles_fetch_params gles_trace_matrix_op gles_matrix_stack_check
 * SLICE:route hw/arm/gles-host.c range     if (gh.palette_mode && gles_palette_op(slot)) { |     switch (slot) {
 * SLICE:cases hw/arm/gles-host.c range     case GLES_SLOT_ENABLE: |     case GLES_SLOT_DRAW_ELEMENTS: {
 * SLICE:cases hw/arm/gles-host.c range     case GLES_SLOT_GET_INTEGERV: |     case GLES_SLOT_SCALEX:
 * SLICE:cases hw/arm/gles-host.c range     case GLES_SLOT_IS_ENABLED: |     case GLES_SLOT_IS_TEXTURE:
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
static int gles_strict;
static uint64_t gles_t(void) { return 0; }
static const GLint gles_compressed_formats[] = { 0 };
static uint8_t guest[0x1000];
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write)
{
    if (addr < 0x1000 || addr - 0x1000 + n > sizeof(guest)) return -1;
    if (write) memcpy(guest + (addr - 0x1000), data, n);
    else memcpy(data, guest + (addr - 0x1000), n);
    return 0;
}
static bool gles_host_init(void) { return true; }
static int64_t gles_es2_draw(CPUState *cpu, bool elements, const uint32_t *a) { abort(); }
static bool gles_refresh_surfaces(CPUState *cpu) { return true; }
static void gles_draw_sized_points(CPUState *cpu, uint32_t first, uint32_t count) { abort(); }
static void gles_check_draw(const char *what, GLenum mode, uint32_t count) {}
static void gles_trace_draw(const char *what, GLenum mode, uint32_t count) {}
static void gles_note_primitive(GLenum mode) {}
static int64_t gles_host_call_1(CPUState *cpu, uint32_t slot, uint32_t ctx, uint32_t argc, const uint32_t *a);
#include "slice.h"
static int64_t gles_host_call_1(CPUState *cpu, uint32_t slot, uint32_t ctx, uint32_t argc, const uint32_t *a)
{
#include "route.h"
    switch (slot) {
#include "cases.h"
    default: abort();
    }
}
static int64_t call(uint32_t slot, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    const uint32_t a[4] = { a0, a1, a2, a3 };
    return gles_host_call_1(NULL, slot, 0, 4, a);
}
static uint32_t F(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static GLint get(GLenum pname)
{
    GLint v = -1;
    assert(!call(GLES_SLOT_GET_INTEGERV, pname, 0x1000, 0, 0));
    memcpy(&v, guest, 4);
    return v;
}
static void pixel(int x, int y, int lo, int hi)
{
    uint8_t p[4];
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    printf("%d,%d: %d\n", x, y, p[0]);
    assert(p[0] >= lo && p[0] <= hi);
}

/* One bone-weighted square: its centre, two matrix indices and their weights; the vertices as the game's buffer
 * holds them (position, weights, indices). */
typedef struct { float pos[3], w[2]; uint8_t idx[2], pad[2]; } Vertex;
static void square(Vertex *v, float x, uint8_t i0, uint8_t i1, float w0, float w1)
{
    static const float corner[6][2] = { {-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1} };
    for (unsigned k = 0; k < 6; k++) {
        v[k] = (Vertex){ { x + corner[k][0] * 0.15f, corner[k][1] * 0.15f, 0 }, { w0, w1 }, { i0, i1 } };
    }
}

int main(void)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, 0 };
    CGLPixelFormatObj pf;
    GLint n;
    CGLContextObj c;
    assert(!CGLChoosePixelFormat(attrs, &pf, &n));
    assert(!CGLCreateContext(pf, NULL, &c));
    CGLDestroyPixelFormat(pf);
    assert(!CGLSetCurrentContext(c));
    GLuint fbo, tex;
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, 32, 32);
    gh.vertex.client_state = GL_VERTEX_ARRAY;
    gh.normal.client_state = GL_NORMAL_ARRAY;
    gh.color.client_state = GL_COLOR_ARRAY;
    for (unsigned u = 0; u < GLES_MAX_TEXUNITS; u++) gh.texcoord[u].client_state = GL_TEXTURE_COORD_ARRAY;
    gles_palette_reset(&gh);

    /* the extension's state as a context starts it, and its limits */
    assert(get(GL_MAX_PALETTE_MATRICES_OES) == 11 && get(GL_MAX_VERTEX_UNITS_OES) == 4);
    assert(get(GL_MATRIX_INDEX_ARRAY_TYPE_OES) == GL_UNSIGNED_BYTE && get(GL_WEIGHT_ARRAY_TYPE_OES) == GL_FLOAT);
    assert(!call(GLES_SLOT_IS_ENABLED, GL_MATRIX_PALETTE_OES, 0, 0, 0));

    /* the modelview the palette replaces: everything 5 to the right, off the target */
    assert(!call(GLES_SLOT_MATRIX_MODE, GL_MODELVIEW, 0, 0, 0));
    assert(!call(GLES_SLOT_TRANSLATEF, F(5), 0, 0, 0));
    /* the game's order: matrix mode left at the texture matrix, then the palette */
    assert(!call(GLES_SLOT_MATRIX_MODE, GL_TEXTURE, 0, 0, 0));
    assert(!call(GLES_SLOT_ENABLE, GL_MATRIX_PALETTE_OES, 0, 0, 0));
    assert(!call(GLES_SLOT_MATRIX_MODE, GL_MATRIX_PALETTE_OES, 0, 0, 0));
    assert(get(GL_MATRIX_MODE) == GL_MATRIX_PALETTE_OES && call(GLES_SLOT_IS_ENABLED, GL_MATRIX_PALETTE_OES, 0, 0, 0));
    /* bone 0 loaded, bone 1 built by the matrix calls, bone 2 a half turn about y */
    float m0[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.5f, 0, 0, 1 };
    memcpy(guest, m0, sizeof(m0));
    assert(!call(GLES_ID_glCurrentPaletteMatrixOES, 0, 0, 0, 0));
    assert(!call(GLES_SLOT_LOAD_MATRIXF, 0x1000, 0, 0, 0));
    assert(!call(GLES_ID_glCurrentPaletteMatrixOES, 1, 0, 0, 0));
    assert(!call(GLES_SLOT_LOAD_IDENTITY, 0, 0, 0, 0));
    assert(!call(GLES_SLOT_TRANSLATEF, F(0.5f), 0, 0, 0));
    assert(!call(GLES_ID_glCurrentPaletteMatrixOES, 2, 0, 0, 0));
    assert(!call(GLES_SLOT_ROTATEF, F(180), 0, F(1), 0));
    assert(get(GL_CURRENT_PALETTE_MATRIX_OES) == 2);
    /* out of range, and the palette has no stack */
    assert(call(GLES_ID_glCurrentPaletteMatrixOES, 11, 0, 0, 0) == -1 && gh.error == GL_INVALID_VALUE);
    gh.error = 0;
    assert(call(GLES_SLOT_PUSH_MATRIX, 0, 0, 0, 0) == -1 && gh.error == GL_STACK_OVERFLOW);
    gh.error = 0;
    /* none of it reached the host's matrices */
    GLint mode;
    float m[16];
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetFloatv(GL_TEXTURE_MATRIX, m);
    assert(mode == GL_TEXTURE && m[12] == 0 && m[0] == 1);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    assert(m[12] == 5);
    assert(fabsf(gh.palette[1][12] - 0.5f) < 1e-6f && fabsf(gh.palette[2][0] + 1) < 1e-6f);

    /* three squares from a buffer object: on bone 0, on bone 1, and half on each, which lands between them */
    static Vertex v[18];
    square(v, 0, 0, 0, 1, 0);
    square(v + 6, 0, 1, 0, 1, 0);
    square(v + 12, 0, 0, 1, 0.5f, 0.5f);
    GLESBuffer vbo = { .data = (uint8_t *)v, .size = sizeof(v), .name = 7, .refs = 2 };
    gh.array_buffer = &vbo;
    assert(!call(GLES_SLOT_ENABLE_CLIENT_STATE, GL_VERTEX_ARRAY, 0, 0, 0));
    assert(!call(GLES_SLOT_ENABLE_CLIENT_STATE, GL_WEIGHT_ARRAY_OES, 0, 0, 0));
    assert(!call(GLES_SLOT_ENABLE_CLIENT_STATE, GL_MATRIX_INDEX_ARRAY_OES, 0, 0, 0));
    assert(!call(GLES_SLOT_VERTEX_POINTER, 3, GL_FLOAT, sizeof(Vertex), 0));
    assert(!call(GLES_ID_glWeightPointerOES, 2, GL_FLOAT, sizeof(Vertex), offsetof(Vertex, w)));
    assert(!call(GLES_ID_glMatrixIndexPointerOES, 2, GL_UNSIGNED_BYTE, sizeof(Vertex), offsetof(Vertex, idx)));
    assert(call(GLES_ID_glMatrixIndexPointerOES, 5, GL_UNSIGNED_BYTE, 0, 0) == -1 && gh.error == GL_INVALID_VALUE);
    gh.error = 0;
    assert(call(GLES_ID_glWeightPointerOES, 1, GL_UNSIGNED_BYTE, 0, 0) == -1 && gh.error == GL_INVALID_ENUM);
    gh.error = 0;
    gh.array_buffer = NULL;
    assert(get(GL_WEIGHT_ARRAY_BUFFER_BINDING_OES) == 7 && get(GL_MATRIX_INDEX_ARRAY_SIZE_OES) == 2 &&
           get(GL_WEIGHT_ARRAY_STRIDE_OES) == sizeof(Vertex) && get(GL_MATRIX_INDEX_ARRAY_OES) == 1);
    assert(!call(GLES_SLOT_GET_POINTERV, GL_WEIGHT_ARRAY_POINTER_OES, 0x1000, 0, 0));
    assert(!memcmp(guest, &(uint32_t){ offsetof(Vertex, w) }, 4));

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor4f(1, 0, 0, 1);
    assert(!call(GLES_SLOT_DRAW_ARRAYS, GL_TRIANGLES, 0, 18, 0));
    pixel(8, 16, 255, 255);     /* bone 0: x -0.5 */
    pixel(24, 16, 255, 255);    /* bone 1: x 0.5 */
    pixel(16, 16, 255, 255);    /* half each: x 0 */
    pixel(16, 28, 0, 0);
    pixel(2, 16, 0, 0);
    /* the draw put the host's matrices and mode back */
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    assert(mode == GL_TEXTURE && m[12] == 5);

    /* an ordinary draw after the skinned one: leaving palette mode for MODELVIEW selects the host's modelview, which
     * the next call moves, and the square lands where that puts it */
    assert(!call(GLES_SLOT_DISABLE, GL_MATRIX_PALETTE_OES, 0, 0, 0));
    assert(!call(GLES_SLOT_MATRIX_MODE, GL_MODELVIEW, 0, 0, 0));
    assert(!call(GLES_SLOT_TRANSLATEF, F(-5), F(0.5f), 0, 0));
    glClear(GL_COLOR_BUFFER_BIT);
    assert(!call(GLES_SLOT_DRAW_ARRAYS, GL_TRIANGLES, 0, 6, 0));
    pixel(16, 24, 255, 255);    /* x 0, y 0.5 */
    pixel(8, 16, 0, 0);
    pixel(16, 16, 0, 0);
    glGetFloatv(GL_TEXTURE_MATRIX, m);
    assert(m[12] == 0 && m[0] == 1);
    assert(!call(GLES_SLOT_TRANSLATEF, F(5), F(-0.5f), 0, 0));
    assert(!call(GLES_SLOT_MATRIX_MODE, GL_MATRIX_PALETTE_OES, 0, 0, 0));
    assert(!call(GLES_SLOT_ENABLE, GL_MATRIX_PALETTE_OES, 0, 0, 0));

    /* lit: a square facing away (normal -z), on bone 2's half turn, faces the default light (+z) once its normal
     * is skinned too */
    static struct { float pos[3], n[3], w; uint8_t idx, pad[3]; } lv[6];
    for (unsigned k = 0; k < 6; k++) {
        lv[k].pos[0] = v[k].pos[0]; lv[k].pos[1] = v[k].pos[1];
        lv[k].n[2] = -1; lv[k].w = 1; lv[k].idx = 2;
    }
    GLESBuffer lit = { .data = (uint8_t *)lv, .size = sizeof(lv), .name = 8, .refs = 2 };
    gh.array_buffer = &lit;
    assert(!call(GLES_SLOT_ENABLE_CLIENT_STATE, GL_NORMAL_ARRAY, 0, 0, 0));
    assert(!call(GLES_SLOT_VERTEX_POINTER, 3, GL_FLOAT, sizeof(lv[0]), 0));
    assert(!call(GLES_SLOT_NORMAL_POINTER, GL_FLOAT, sizeof(lv[0]), 12, 0));
    assert(!call(GLES_ID_glWeightPointerOES, 1, GL_FLOAT, sizeof(lv[0]), 24));
    assert(!call(GLES_ID_glMatrixIndexPointerOES, 1, GL_UNSIGNED_BYTE, sizeof(lv[0]), 28));
    gh.array_buffer = NULL;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glClear(GL_COLOR_BUFFER_BIT);
    assert(!call(GLES_SLOT_DRAW_ARRAYS, GL_TRIANGLES, 0, 6, 0));
    pixel(16, 16, 200, 255);    /* diffuse 0.8 plus ambient, from the front */
    glDisable(GL_LIGHTING);

    /* disabled, the palette leaves the draw to the modelview: off the target */
    assert(!call(GLES_SLOT_DISABLE, GL_MATRIX_PALETTE_OES, 0, 0, 0));
    glClear(GL_COLOR_BUFFER_BIT);
    assert(!call(GLES_SLOT_DRAW_ARRAYS, GL_TRIANGLES, 0, 6, 0));
    pixel(16, 16, 0, 0);
    /* LoadPaletteFromModelViewMatrixOES copies the modelview, in any mode; leaving palette mode restores the
     * host's */
    assert(!call(GLES_ID_glLoadPaletteFromModelViewMatrixOES, 0, 0, 0, 0));
    assert(gh.palette[2][12] == 5);
    assert(!call(GLES_SLOT_MATRIX_MODE, GL_MODELVIEW, 0, 0, 0));
    assert(get(GL_MATRIX_MODE) == GL_MODELVIEW);
    assert(glGetError() == GL_NO_ERROR && !gh.error);
    char *rejects = gles_host_rejects();
    assert(!*rejects);
    free(rejects);
    puts("PASS: OES_matrix_palette: palette matrices kept off the host's, skinned positions and normals, state restored");
    return 0;
}
