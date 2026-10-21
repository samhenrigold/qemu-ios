/* The GL bridge built for ANGLE, against real ANGLE Metal contexts: a sampled surface refreshes from guest memory
 * in an ES 2.0 context as in an ES1 one, and the bridge's own checks of a draw raise no GL error in either.
 * Run by tests/ipad1/angle-slice.sh.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c fn gles_reject
 * SLICE hw/arm/gles-host.c range #define GLES_PRIVATE_NAME | static bool gles_is_drawable(
 * SLICE hw/arm/gles-host.c range static GLESPVRTC *gles_pvrtc_texture( | static int64_t gles_generate_mipmap(
 * SLICE:helpers hw/arm/gles-host.c fn gles_image_bytes gles_read_pixels
 * SLICE:helpers hw/arm/gles-host.c range static bool gles_surface_range( | /* ------------------------------------------------------------ ES 2.0
 * SLICE:draw hw/arm/gles-host.c fn gles_is_drawable gles_host_fbo gles_texcoord_mask gles_check_texcoords
 * SLICE:draw hw/arm/gles-host.c fn gles_check_fb_complete gles_check_draw
 * CFLAGS -DGLES_HOST_ANGLE=1 -I$ANGLE/include -L$ANGLE/out/ltm-metal -Wl,-rpath,$ANGLE/out/ltm-metal -lEGL -lGLESv2
 * CFLAGS -I$ROOT/include -I$ROOT/hw/arm -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include "slice.h"
static uint8_t ram[0x10000];
int gles_guest_rw(CPUState *cpu, vaddr a, void *p, size_t n, bool write)
{
    if (a < 0x10000000 || a + n > 0x10000000 + sizeof(ram)) return -1;
    if (write) memcpy(ram + (a - 0x10000000), p, n);
    else memcpy(p, ram + (a - 0x10000000), n);
    return 0;
}
/* As tests/slice/ipod-gles-surface.c: guest pages map 1:1 onto ram, and a page is dirty while its bytes differ
 * from what was last cleared. */
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1u << TARGET_PAGE_BITS)
#define TARGET_PAGE_MASK (~(vaddr)(TARGET_PAGE_SIZE - 1))
#define DIRTY_MEMORY_VGA 0
#define WITH_RCU_READ_LOCK_GUARD()
typedef struct { int unused; } MemTxAttrs;
typedef struct { int unused; } MemoryRegion;
typedef struct AddressSpace AddressSpace;
static MemoryRegion fake_ram;
static uint8_t seen[sizeof(ram)];
static hwaddr cpu_get_phys_page_attrs_debug(CPUState *c, vaddr va, MemTxAttrs *attrs)
{ return va < 0x10000000 || va >= 0x10000000 + sizeof(ram) ? (hwaddr)-1 : va; }
static int cpu_asidx_from_attrs(CPUState *c, MemTxAttrs attrs) { return 0; }
typedef int MMUAccessType;
enum { MMU_DATA_LOAD, MMU_DATA_STORE };
#define TLB_INVALID_MASK 1
static void *cpu_env(CPUState *c) { return NULL; }
static int cpu_mmu_index(CPUState *c, bool ifetch) { return 0; }
static int probe_access_flags(void *env, vaddr va, int size, MMUAccessType type, int idx, bool nonfault,
                              void **host, uintptr_t ra) { return 0; }
static AddressSpace *cpu_get_address_space(CPUState *c, int i) { return NULL; }
static MemoryRegion *address_space_translate(AddressSpace *as, hwaddr pa, hwaddr *xlat, hwaddr *len,
                                             bool w, MemTxAttrs attrs)
{ *xlat = pa - 0x10000000; return &fake_ram; }
static bool memory_region_is_ram(MemoryRegion *mr) { return mr == &fake_ram; }
static void memory_region_set_log(MemoryRegion *mr, bool on, unsigned client) {}
static ram_addr_t memory_region_get_ram_addr(MemoryRegion *mr) { return 0; }
static void *qemu_map_ram_ptr(void *block, ram_addr_t at) { return ram + at; }
static bool physical_memory_get_dirty_flag(ram_addr_t at, unsigned client)
{ return memcmp(ram + at, seen + at, TARGET_PAGE_SIZE) != 0; }
#define DIRTY_CLIENTS_ALL 0xff
static void physical_memory_set_dirty_range(ram_addr_t at, ram_addr_t len, uint8_t mask) {}
static bool physical_memory_test_and_clear_dirty(ram_addr_t at, ram_addr_t len, unsigned client,
                                                 unsigned long *bmap)
{
    bool d = memcmp(ram + at, seen + at, len) != 0;
    memcpy(seen + at, ram + at, len);
    return d;
}
#include "helpers.h"
static int gles_strict = 1, gles_prof;
static uint64_t gles_t(void) { return 0; }
#include "draw.h"

/* Texel (0, 0) of the texture bound at GL_TEXTURE_2D, through a scratch FBO (ES has no glGetTexImage). */
static void texel(uint8_t out[4])
{
    GLint tex = 0, old = 0;
    GLuint fb;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &old);
    glGenFramebuffersOES(1, &fb);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, fb);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, tex, 0);
    assert(glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, old);
    glDeleteFramebuffersOES(1, &fb);
}

/* A 4x2 'BGRA' surface bound at unit 1, then guest DMA into it: a draw's refresh reloads it. */
static void refresh(bool es2)
{
    GLESAngleContextRef c;
    GLuint tex;
    uint8_t got[4];
    uint32_t a[] = { GL_TEXTURE_2D, 0x10000000, 16, 4, 2, GLES_SURFACE_BGRA32, 0, 0 };

    memset(&gh, 0, sizeof(gh));
    assert(gles_angle_create(NULL, es2, &c) && !CGLSetCurrentContext(c));
    gh.cgl = c;
    gh.surfaces = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    memset(ram, 0, sizeof(ram));
    for (int i = 0; i < 8; i++) memcpy(ram + i * 4, "\x10\x20\x30\xff", 4);
    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    assert(!gles_bind_surface(NULL, a, 0));
    texel(got);
    assert(!memcmp(got, "\x30\x20\x10\xff", 4));
    if (es2) gh.program = 1;
    else glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 8; i++) memcpy(ram + i * 4, "\x40\x50\x60\xff", 4);
    assert(gles_refresh_surfaces(NULL));
    glActiveTexture(GL_TEXTURE1);
    texel(got);
    assert(!memcmp(got, "\x60\x50\x40\xff", 4));
    assert(glGetError() == GL_NO_ERROR && !gles_rejects);
    g_hash_table_destroy(gh.surfaces);
    CGLSetCurrentContext(NULL);
    CGLReleaseContext(c);
}

/* The first draw's checks (offscreen, so all of them), as gles_check_draw runs them after any draw. */
static void check_draw(bool es2)
{
    GLESAngleContextRef c;
    memset(&gh, 0, sizeof(gh));
    assert(gles_angle_create(NULL, es2, &c) && !CGLSetCurrentContext(c));
    gh.cgl = c;
    gh.fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);
    gh.bound_framebuffer = 7;
    if (!es2) glEnable(GL_TEXTURE_2D);
    gles_check_draw("glDrawArrays", GL_TRIANGLES, 3);
    assert(gh.draws_offscreen == 1 && !gles_rejects && glGetError() == GL_NO_ERROR);
    g_hash_table_destroy(gh.fbo_drawable);
    CGLSetCurrentContext(NULL);
    CGLReleaseContext(c);
}

int main(void)
{
    refresh(false);
    refresh(true);
    check_draw(true);           /* first: gles_check_texcoords warns once */
    check_draw(false);
    return 0;
}
