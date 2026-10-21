/* Guest sizes, strides and counts never steer a host access outside the buffer they describe.
 *
 * An attribute array takes its layout only from gles_set_pointer, which refuses what GL would (1 to 4 components,
 * a type the bridge sizes, a stride that is a GLsizei): a GL_FIXED size of 0x40000001 made the element size wrap to
 * 4 bytes while the widening loop walked 0x40000001 components per vertex. A layout that reaches the draw anyway (a
 * snapshot is restored through the same check, but the range code must not depend on it) is ranged by
 * gles_array_range, whose sums cannot wrap: a stride and first near 2^32 used to wrap the 64-bit end check and point
 * the host far outside the buffer object. glUniform*v's count times its width is taken in 64 bits, so the host never
 * reads more uniforms than were fetched. A NULL-pixel glTexImage2D with a row-bytes setting is zero-filled and
 * unpacked by one layout, so the host reads no more than the zeros (it used to unpack by alignment rules a staging
 * buffer sized by row bytes).
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c range /* ---------------------------------------------------------- GPU memory | /*\n * Profiling and strictness
 * SLICE hw/arm/gles-host.c define GLES_FIXED
 * SLICE hw/arm/gles-host.c fn gles_type_size gles_x gles_buffer_destroy gles_buffer_bind
 * SLICE hw/arm/gles-host.c range /* The ES half-float type | /* ---------------------------------------------------------------- refusals
 * SLICE hw/arm/gles-host.c range static const uint8_t *gles_zeroed( | /*\n * Report a draw the host rejected
 * SLICE hw/arm/gles-host.c fn gles_array_layout_ok gles_set_pointer gles_es2_fetch gles_es2_bind_attr gles_texture_object
 * SLICE:attr hw/arm/gles-host.c range     case 511: |     case 512: case 513:
 * SLICE:uniform hw/arm/gles-host.c range     case 610 ... 617: { |     case 625: case 632: {
 * SLICE:teximage hw/arm/gles-host.c range case GLES_SLOT_TEX_IMAGE_2D: { | case GLES_SLOT_TEX_SUB_IMAGE_2D: {
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
static int gles_strict = 1;
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void gles_surface_forget(GLenum target) { (void)target; }
static void gles_texture_begin(void) {}
static int64_t gles_texture_end(uint32_t target, uint32_t level, GLESPVRTCLevel image) { return 0; }

/* The host GL calls the paths under test make, recorded instead of run (no context is needed for them). */
static const void *rec_ptr;
static bool rec_enabled;
static GLsizei rec_count;
static GLint unpack_alignment = 4, unpack_row_length;
static size_t teximage_reads;
#define glVertexAttribPointer(i, s, t, n, st, p) (rec_ptr = (const void *)(p))
#define glEnableVertexAttribArray(i) (rec_enabled = true)
#define glGetError() GL_NO_ERROR
#define glUniform1fv(l, c, v) (rec_count = (c))
#define glUniform2fv(l, c, v) (rec_count = (c))
#define glUniform3fv(l, c, v) (rec_count = (c))
#define glUniform4fv(l, c, v) (rec_count = (c))
#define glUniform1iv(l, c, v) (rec_count = (c))
#define glUniform2iv(l, c, v) (rec_count = (c))
#define glUniform3iv(l, c, v) (rec_count = (c))
#define glUniform4iv(l, c, v) (rec_count = (c))
#define glUniformMatrix2fv(l, c, t, v) (rec_count = (c))
#define glUniformMatrix3fv(l, c, t, v) (rec_count = (c))
#define glUniformMatrix4fv(l, c, t, v) (rec_count = (c))
static void pixel_store(GLenum pname, GLint v)
{
    if (pname == GL_UNPACK_ALIGNMENT) unpack_alignment = v;
    if (pname == GL_UNPACK_ROW_LENGTH) unpack_row_length = v;
}
/* What GL's unpack rules read for a w x h image of bpp-byte texels under the recorded state. */
static void tex_image(GLsizei w, GLsizei h, size_t bpp, const void *p)
{
    size_t row = unpack_row_length ? (size_t)unpack_row_length * bpp : (size_t)w * bpp;
    row = (row + unpack_alignment - 1) / unpack_alignment * unpack_alignment;
    teximage_reads = p ? row * (h - 1) + (size_t)w * bpp : 0;
}
#define glPixelStorei(p, v) pixel_store(p, v)
#define glTexImage2D(t, l, i, w, h, b, f, ty, p) tex_image(w, h, gles_texel_bytes(f, ty), p)
#define glGetTexParameteriv(t, p, v) (*(v) = 0)
#define glTexParameteri(t, p, v) ((void)0)
#define glGetIntegerv(p, v) (*(v) = 0)
#include "slice.h"

static uint8_t guest[4096];
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write)
{
    if (write || addr < 0x1000 || addr + n > 0x1000 + sizeof(guest)) return -1;
    memcpy(data, guest + (addr - 0x1000), n);
    return 0;
}

static int64_t es2(uint32_t slot, const uint32_t *a)
{
    static const unsigned vec_n[] = { 1, 2, 3, 4 };
    g_autofree void *v = NULL;
    CPUState *cpu = NULL;
    int64_t result = 0, *r = &result;
    switch (slot) {
#include "attr.h"
#include "uniform.h"
    default: assert(0);
    }
    return result;
}

static int64_t teximage(const uint32_t *a)
{
    CPUState *cpu = NULL;
    switch (GLES_SLOT_TEX_IMAGE_2D) {
#include "teximage.h"
    }
    return -1;
}

int main(void)
{
    GLESBuffer vbo = { .data = g_malloc0(64), .size = 64, .refs = 1 };
    GLESArray *at = &gh.attr[0];

    /* GL_FIXED with 0x40000001 components: refused, the attribute untouched, and no draw reads it */
    uint32_t fixed[] = { 0, 0x40000001, 0x140C, 0, 0, 0x1000 };
    assert(es2(511, fixed) == -1 && gh.error == GL_INVALID_VALUE && !at->size && !at->ptr);
    gh.error = 0;
    at->enabled = true;
    assert(!gles_es2_bind_attr(NULL, 0, 0, 4) && !rec_enabled);

    /* a stride past INT32_MAX is no GLsizei: refused */
    uint32_t wide[] = { 0, 4, GL_FLOAT, 0, 0x80000000u, 0x1000 };
    assert(es2(511, wide) == -1 && !at->size);

    /* a good layout draws from guest memory */
    uint32_t good[] = { 0, 2, GL_FLOAT, 0, 8, 0x1000 };
    assert(es2(511, good) == 0 && at->size == 2 && at->stride == 8);
    assert(gles_es2_bind_attr(NULL, 0, 0, 4) && rec_enabled && rec_ptr == at->buf);

    /* a buffer-object range whose 64-bit end wraps (stride and first near 2^32, offset near 2^32) is refused,
     * not resolved to a pointer outside the buffer */
    rec_enabled = false; rec_ptr = NULL;
    gles_buffer_bind(&at->vbo, &vbo);
    at->type = GL_FLOAT; at->size = 1; at->stride = 0xffffffffu; at->ptr = 0xffffffffu;
    assert(!gles_es2_bind_attr(NULL, 0, 0xffffffffu, 2) && !rec_enabled);
    /* and one inside it is taken */
    at->stride = 4; at->ptr = 8;
    assert(gles_es2_bind_attr(NULL, 0, 2, 4) && rec_ptr == vbo.data + 16);
    uint64_t off, len;
    assert(gles_array_range(8, 4, 4, 2, 4, 64, &off, &len) && off == 16 && len == 16);
    assert(!gles_array_range(8, 4, 4, 2, 13, 64, &off, &len));          /* one past the end */
    assert(!gles_array_range(UINT32_MAX, UINT32_MAX, 4, UINT32_MAX, 2, 64, &off, &len));
    assert(gles_range_ok(60, 4, 64) && !gles_range_ok(61, 4, 64) && !gles_range_ok(UINT64_MAX, 2, 64));

    /* glUniform4fv with a count whose count * 4 wraps 32 bits: refused, the host never called */
    uint32_t uni[] = { 0, 0x40000001, 0x1000 };
    rec_count = 0;
    assert(es2(613, uni) == -1 && rec_count == 0);
    uint32_t uni_ok[] = { 0, 3, 0x1000 };
    assert(es2(613, uni_ok) == 0 && rec_count == 3);

    /* NULL-pixel glTexImage2D under UNPACK_ROW_BYTES_APPLE = 6 (2 RGB texels, unaligned): the host reads only the
     * zeros it was given */
    gh.unpack_row_bytes = 6;
    uint32_t tex[9] = { GL_TEXTURE_2D, 0, GL_RGB, 2, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, 0 };
    assert(teximage(tex) == 0 && teximage_reads && teximage_reads <= gh.zerobuf_size);
    /* and with guest pixels, likewise from the staging copy */
    tex[8] = 0x1000;
    assert(teximage(tex) == 0 && teximage_reads <= gh.txbuf_size);

    puts("PASS: guest layouts refused at the setter, wrapped ranges refused, uniform counts and NULL uploads bounded");
    return 0;
}
