/* What the guest's GL objects hold on the host, together, stays under GLES_MAX_GPU_BYTES, against CGL.
 *
 * Each texture level and renderbuffer is charged when the host is asked to make it and given back when it is
 * deleted; replacing an image charges only the difference, and an upload the host refuses takes its charge back.
 * Past the cap the call fails with GL_OUT_OF_MEMORY and the host allocates nothing (the per-object size checks
 * alone let a guest make as many 64 MiB textures as it had names).
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c range /* ---------------------------------------------------------- GPU memory | /*\n * Profiling and strictness
 * SLICE hw/arm/gles-host.c define GLES_PRIVATE_NAME
 * SLICE hw/arm/gles-host.c fn gles_type_size gles_x
 * SLICE hw/arm/gles-host.c range /* The ES half-float type | /* ---------------------------------------------------------------- refusals
 * SLICE hw/arm/gles-host.c range static const uint8_t *gles_zeroed( | /*\n * Report a draw the host rejected
 * SLICE hw/arm/gles-host.c fn gles_texture_begin gles_texture_end gles_guest_name_ok gles_guest_names gles_rb_format gles_texture_object
 * SLICE:teximage hw/arm/gles-host.c range case GLES_SLOT_TEX_IMAGE_2D: { | case GLES_SLOT_TEX_SUB_IMAGE_2D: {
 * SLICE:teximage hw/arm/gles-host.c range case GLES_SLOT_DELETE_TEXTURES: { | case GLES_SLOT_ALPHA_FUNC:
 * SLICE:teximage hw/arm/gles-host.c range case GLES_SLOT_RENDERBUFFER_STORAGE: { | case GLES_SLOT_FB_RENDERBUFFER: {
 * SLICE:teximage hw/arm/gles-host.c range case GLES_SLOT_DELETE_RENDERBUFFERS: | case GLES_SLOT_CHECK_FB_STATUS: {
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
static int gles_strict = 1;
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void gles_surface_forget(GLenum target) { (void)target; }
static GLESPVRTC *gles_pvrtc_texture(bool create) { return NULL; }
#include "slice.h"

static uint32_t guest[64];
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write)
{
    if (write || addr < 0x1000 || addr + n > 0x1000 + sizeof(guest)) return -1;
    memcpy(data, (uint8_t *)guest + (addr - 0x1000), n);
    return 0;
}

static int64_t dispatch(uint32_t slot, const uint32_t *a)
{
    CPUState *cpu = NULL;
    switch (slot) {
#include "teximage.h"
    default: assert(0);
    }
    return -1;
}

/* glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL) through the bridge. */
static int64_t teximage(uint32_t target, uint32_t level, uint32_t ifmt, uint32_t w, uint32_t h)
{
    uint32_t a[9] = { target, level, ifmt, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0 };
    return dispatch(GLES_SLOT_TEX_IMAGE_2D, a);
}

static GLint width(GLuint texture, GLint level)
{
    GLint w = -1;
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
    return w;
}

int main(void)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj format;
    GLint count;
    CGLContextObj ctx;
    assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(ctx) == kCGLNoError);
    gh.rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
    gh.unpack_alignment = 4;

    const uint64_t mib = 1 << 20, cap = GLES_MAX_GPU_BYTES;
    GLuint tex[3], rb;
    glGenTextures(3, tex);

    /* A level is charged once, its replacement by the difference, each level and face apart. */
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    assert(!teximage(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512) && gles_held_bytes == mib);
    assert(!teximage(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512) && gles_held_bytes == mib);
    assert(!teximage(GL_TEXTURE_2D, 1, GL_RGBA, 256, 256) && gles_held_bytes == mib + mib / 4);
    assert(!teximage(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256) && gles_held_bytes == mib / 2);

    /* One byte past the cap: GL_OUT_OF_MEMORY, and the host made nothing. At the cap it is made. */
    gles_buffer_bytes = cap - gles_held_bytes - 4 * mib + 1;
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    assert(teximage(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1024) < 0 && gh.error == GL_OUT_OF_MEMORY);
    assert(width(tex[1], 0) == 0 && gles_held_bytes == mib / 2);
    gh.error = 0;
    gles_buffer_bytes--;
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    assert(!teximage(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1024) && width(tex[1], 0) == 1024);
    assert(gles_held_bytes + gles_buffer_bytes == cap);

    /* Full: a renderbuffer and texture 0 (the context's own) are refused as well. */
    glGenRenderbuffersEXT(1, &rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    gh.bound_renderbuffer = rb;
    uint32_t storage[] = { GL_RENDERBUFFER_EXT, GL_RGBA8, 64, 64 };
    assert(dispatch(GLES_SLOT_RENDERBUFFER_STORAGE, storage) < 0 && gh.error == GL_OUT_OF_MEMORY);
    gh.error = 0;
    glBindTexture(GL_TEXTURE_2D, 0);
    assert(teximage(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16) < 0 && gh.error == GL_OUT_OF_MEMORY);
    gh.error = 0;

    /* Deleting gives the bytes back; then the same calls go through. */
    guest[0] = tex[1];
    uint32_t del[] = { 1, 0x1000 };
    assert(!dispatch(GLES_SLOT_DELETE_TEXTURES, del) && gles_held_bytes == mib / 2);
    assert(!dispatch(GLES_SLOT_RENDERBUFFER_STORAGE, storage) && gles_held_bytes == mib / 2 + 64 * 64 * 4);
    assert(!teximage(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16) && gles_held_bytes == mib / 2 + 64 * 64 * 4 + 16 * 16 * 4);
    guest[0] = rb;
    assert(!dispatch(GLES_SLOT_DELETE_RENDERBUFFERS, del) && gles_held_bytes == mib / 2 + 16 * 16 * 4);

    /* A renderbuffer larger than a texture may be is refused before the budget is asked. */
    uint32_t huge[] = { GL_RENDERBUFFER_EXT, GL_RGBA8, 16384, 16384 };
    gles_buffer_bytes = 0;
    assert(dispatch(GLES_SLOT_RENDERBUFFER_STORAGE, huge) < 0 && gh.error == GL_INVALID_VALUE);
    gh.error = 0;

    /* An upload the host refuses (an internal format it does not take) keeps the old charge. */
    glBindTexture(GL_TEXTURE_2D, tex[2]);
    assert(teximage(GL_TEXTURE_2D, 0, 0x1234, 512, 512) < 0 && gh.error != GL_OUT_OF_MEMORY);
    assert(gles_held_bytes == mib / 2 + 16 * 16 * 4);
    gh.error = 0;

    /* A context's and a share group's tables give everything back when they go. */
    gles_charge_free(gh.own_charged);
    gles_charge_free(gh.charged);
    assert(gles_held_bytes == 0);

    glDeleteTextures(3, tex);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    puts("PASS: GL objects' host memory charged, given back and capped at GLES_MAX_GPU_BYTES");
    return 0;
}
