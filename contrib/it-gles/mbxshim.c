/*
 * mbxshim -- a drop-in replacement for
 * /System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle
 *
 * The stock bundle drives the PowerVR MBX through IOKit. This one forwards
 * every GL call to QEMU instead, where a real host OpenGL context executes it.
 * No IOKit, no AppleMBX, no MBX register contract.
 *
 * WHAT THE FRAMEWORK ACTUALLY REQUIRES OF US
 *
 * All of it was read out of the device's own MBXGLEngine (md5 92ddd55f...,
 * byte-identical to the 3.1.3 SDK copy), not assumed:
 *
 *   - The bundle exports exactly ONE symbol, _GLESGetEGLInterface. Everything
 *     else reaches us through the table it returns. `nm -gU` on the real binary
 *     shows 43 exports and the only GLES* one is that.
 *
 *   - GLESGetEGLInterface is three instructions: return &table. The table is
 *     nine function pointers, then pairs of {const char *extension, u32 bit}.
 *     The framework never indexes past the ninth pointer.
 *
 *   - GLESCreateGC(sharegroup, X+0x10, X+0xCE8, X+0xC) is called from
 *     -[EAGLContext initWithAPI:properties:] on a 6592-byte calloc'd block X.
 *     The real one callocs a 5440-byte GC, stores its two pointer arguments
 *     inside that GC, writes the GC through its FOURTH argument (`str r5,[r11]`
 *     at 0x9b40), and RETURNS 1 ON SUCCESS, 0 on failure.
 *
 *     That return value is easy to get backwards, and getting it backwards
 *     costs a whole debug cycle: -[EAGLContext initWithAPI:] simply hands back
 *     nil, after our bundle has been loaded and all three of our entry points
 *     called, so every log line looks healthy. The success path is not the
 *     function's fall-through epilogue -- that one is `mov r0,#0` and is the
 *     failure exit. Success leaves through `cmp r0,#1; beq` at 0x9de8, where
 *     the comparison itself has already put 1 in r0 and the branch just runs
 *     into the pop. Reading only the epilogue makes it look like every path
 *     returns 0.
 *
 *   - X+0x10 is the dispatch table the framework's gl* trampolines index. Each
 *     trampoline is six instructions: read the context from TSD key 30, load
 *     the function from ctx+(0x10+slot*4), load the GC from ctx+0xC, tail-call.
 *     So arg0 of every entry point is the GC, and we own every table entry.
 *
 * Build with contrib/armv6-toolchain.
 */

#include "gles_stubs.h"

/* ------------------------------------------------------------ QEMU_CALL --- */

#define QC_GLES 0x140

#define QC_GLES_INLINE_ARGS 4

/* Mirrors qemu_call_t: call_number(4) + args(32) + retval(8) + error(8) = 52,
 * packed. Frozen -- see include/hw/arm/guest-services/general.h. */
typedef struct __attribute__((packed)) {
    unsigned int call_number;
    struct __attribute__((packed)) {
        unsigned int slot;
        unsigned int ctx;
        unsigned int argc;
        unsigned int spill;
        unsigned int args[QC_GLES_INLINE_ARGS];
    } gles;
    long long retval;
    long long error;
} qemu_call_t;

#define GLES_OP_PRESENT         0x1000
#define GLES_OP_PRESENT_SURFACE 0x1001

extern long write(int, const void *, unsigned long);

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(2, s, slen(s)); }
static void wd(unsigned v)
{
    char b[12], *p = b + 11;
    *p = 0;
    if (!v) *--p = '0';
    while (v) { *--p = '0' + (v % 10); v /= 10; }
    w(p);
}

static long long qc(unsigned slot, void *gc, unsigned argc, const unsigned *args)
{
    volatile qemu_call_t q;
    unsigned i;
    static unsigned spill[16];

    for (i = 0; i < QC_GLES_INLINE_ARGS; i++) q.gles.args[i] = 0;

    q.call_number = QC_GLES;
    q.gles.slot = slot;
    q.gles.ctx = (unsigned)(unsigned long)gc;
    q.gles.argc = argc;
    q.gles.spill = 0;
    q.retval = 0;
    q.error = 0;

    if (argc <= QC_GLES_INLINE_ARGS) {
        for (i = 0; i < argc; i++) q.gles.args[i] = args[i];
    } else {
        for (i = 0; i < argc; i++) spill[i] = args[i];
        q.gles.spill = (unsigned)(unsigned long)spill;
    }

    __asm__ __volatile__("mcr p15, 3, %0, c15, c15, 0" : : "r"(&q) : "memory");
    return q.retval;
}

/*
 * Reported once per slot, then silent. An app that touches an unimplemented
 * entry point does so thousands of times a second, and a log line per call
 * would both drown the log and slow the guest enough to change what it does.
 */
static unsigned char unimpl_seen[GLES_N_SLOTS];

__attribute__((visibility("hidden"))) int gles_unimpl(unsigned slot)
{
    if (slot < GLES_N_SLOTS && !unimpl_seen[slot]) {
        unimpl_seen[slot] = 1;
        w("[mbxshim] unimplemented slot "); wd(slot); w("\n");
    }
    return 0;
}

/* ------------------------------------------------------- implemented slots --
 *
 * Arguments are declared `unsigned` even where GL says `float`. iOS armv6 uses
 * the soft-float variant of AAPCS, so float arguments arrive in the core
 * registers as raw bit patterns -- which is exactly what the host wants, since
 * it reinterprets them itself. Declaring them float here would round-trip them
 * through a VFP register for no reason and invite an ABI mismatch.
 */

#define A(...) (const unsigned[]){ __VA_ARGS__ }

static int s_clear(void *gc, unsigned mask)
    { return (int)qc(10, gc, 1, A(mask)); }
static int s_clearColor(void *gc, unsigned r, unsigned g, unsigned b, unsigned a)
    { return (int)qc(12, gc, 4, A(r, g, b, a)); }
static int s_color4f(void *gc, unsigned r, unsigned g, unsigned b, unsigned a)
    { return (int)qc(37, gc, 4, A(r, g, b, a)); }
static int s_disable(void *gc, unsigned cap)
    { return (int)qc(63, gc, 1, A(cap)); }
static int s_disableClientState(void *gc, unsigned arr)
    { return (int)qc(64, gc, 1, A(arr)); }
static int s_drawArrays(void *gc, unsigned mode, unsigned first, unsigned count)
    { return (int)qc(65, gc, 3, A(mode, first, count)); }
static int s_enable(void *gc, unsigned cap)
    { return (int)qc(72, gc, 1, A(cap)); }
static int s_enableClientState(void *gc, unsigned arr)
    { return (int)qc(73, gc, 1, A(arr)); }
static int s_finish(void *gc)
    { return (int)qc(89, gc, 0, A(0)); }
static int s_flush(void *gc)
    { return (int)qc(90, gc, 0, A(0)); }
static int s_genTextures(void *gc, unsigned n, unsigned ids)
    { return (int)qc(98, gc, 2, A(n, ids)); }
static int s_getError(void *gc)
    { return (int)qc(102, gc, 0, A(0)); }
static int s_loadIdentity(void *gc)
    { return (int)qc(157, gc, 0, A(0)); }
static int s_matrixMode(void *gc, unsigned m)
    { return (int)qc(174, gc, 1, A(m)); }
static int s_texCoordPointer(void *gc, unsigned size, unsigned type,
                             unsigned stride, unsigned ptr)
    { return (int)qc(289, gc, 4, A(size, type, stride, ptr)); }
static int s_texImage2D(void *gc, unsigned target, unsigned level, unsigned ifmt,
                        unsigned wd_, unsigned ht, unsigned border,
                        unsigned fmt, unsigned type, unsigned pixels)
    { return (int)qc(301, gc, 9,
                     A(target, level, ifmt, wd_, ht, border, fmt, type, pixels)); }
static int s_texParameteri(void *gc, unsigned target, unsigned pname, unsigned p)
    { return (int)qc(304, gc, 3, A(target, pname, p)); }
static int s_vertexPointer(void *gc, unsigned size, unsigned type,
                           unsigned stride, unsigned ptr)
    { return (int)qc(334, gc, 4, A(size, type, stride, ptr)); }
static int s_viewport(void *gc, unsigned x, unsigned y, unsigned wv, unsigned h)
    { return (int)qc(335, gc, 4, A(x, y, wv, h)); }
static int s_orthof(void *gc, unsigned l, unsigned r, unsigned b,
                    unsigned t, unsigned n, unsigned f)
    { return (int)qc(791, gc, 6, A(l, r, b, t, n, f)); }
static int s_bindTexture(void *gc, unsigned target, unsigned tex)
    { return (int)qc(5, gc, 2, A(target, tex)); }

/* ------------------------------------------------------------ EGL interface */

/* One GC per context. The framework only ever hands this back to us as arg0,
 * so its contents are ours; the host keys off the same pointer. */
static unsigned gles_gc_storage[16];
static unsigned gles_sharegroup_storage[8];

static void *GLESCreateSharegroup(void *a, void *b, void *c, void *d)
{
    (void)a; (void)b; (void)c; (void)d;
    w("[mbxshim] GLESCreateSharegroup\n");
    /* Must be non-NULL: the framework treats a null sharegroup as failure and
     * -[EAGLContext initWithAPI:] returns nil without ever calling us again. */
    return gles_sharegroup_storage;
}

static int GLESDestroySharegroup(void *sg) { (void)sg; return 0; }

/*
 * The one that matters. See the header comment for how the contract was read
 * out of the real binary.
 */
static int GLESCreateGC(void *sharegroup, void **table, void *x_ce8,
                        void **gc_out)
{
    unsigned i;

    (void)sharegroup; (void)x_ce8;
    w("[mbxshim] GLESCreateGC\n");

    if (table) {
        /* Fill every entry. The trampolines never null-check. */
        for (i = 0; i < GLES_N_SLOTS; i++) {
            table[i] = gles_default_table[i];
        }
        table[5]   = (void *)s_bindTexture;
        table[10]  = (void *)s_clear;
        table[12]  = (void *)s_clearColor;
        table[37]  = (void *)s_color4f;
        table[63]  = (void *)s_disable;
        table[64]  = (void *)s_disableClientState;
        table[65]  = (void *)s_drawArrays;
        table[72]  = (void *)s_enable;
        table[73]  = (void *)s_enableClientState;
        table[89]  = (void *)s_finish;
        table[90]  = (void *)s_flush;
        table[98]  = (void *)s_genTextures;
        table[102] = (void *)s_getError;
        table[157] = (void *)s_loadIdentity;
        table[174] = (void *)s_matrixMode;
        table[289] = (void *)s_texCoordPointer;
        table[301] = (void *)s_texImage2D;
        table[304] = (void *)s_texParameteri;
        table[334] = (void *)s_vertexPointer;
        table[335] = (void *)s_viewport;
        table[791] = (void *)s_orthof;
    }

    if (gc_out) {
        *gc_out = gles_gc_storage;
    }
    return 1;   /* 1 = success. See the header comment; 0 here yields a nil
                 * EAGLContext with no other symptom. */
}

static int GLESDestroyGC(void *gc) { (void)gc; return 0; }

/*
 * The surface CoreAnimation gave us, if any.
 *
 * The stock engine imports ten IOSurface functions and every one is read-side
 * (IOSurfaceGetBaseAddress/BytesPerRow/Width/Height/PixelFormat/AllocSize/ID/
 * PlaneCount, IOSurfaceLock, IOSurfaceUnlock). There is no IOSurfaceCreate: CA
 * owns the allocation and the engine only renders into it. So all we keep is
 * what the host needs to write pixels.
 */
static struct {
    void *ref;
    unsigned base, stride, width, height, format;
} surf_state;

static void *iosurf;    /* IOSurface.framework handle */
static void *(*p_IOSurfaceGetBaseAddress)(void *);
static unsigned (*p_IOSurfaceGetBytesPerRow)(void *);
static unsigned (*p_IOSurfaceGetWidth)(void *);
static unsigned (*p_IOSurfaceGetHeight)(void *);
static unsigned (*p_IOSurfaceGetPixelFormat)(void *);
static int (*p_IOSurfaceLock)(void *, unsigned, unsigned *);
static int (*p_IOSurfaceUnlock)(void *, unsigned, unsigned *);

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2

static void iosurface_init(void)
{
    if (iosurf) return;
    iosurf = dlopen("/System/Library/PrivateFrameworks/IOSurface.framework/IOSurface",
                    RTLD_NOW);
    if (!iosurf) { w("[mbxshim] IOSurface.framework not available\n"); return; }
    p_IOSurfaceGetBaseAddress = dlsym(iosurf, "IOSurfaceGetBaseAddress");
    p_IOSurfaceGetBytesPerRow = dlsym(iosurf, "IOSurfaceGetBytesPerRow");
    p_IOSurfaceGetWidth       = dlsym(iosurf, "IOSurfaceGetWidth");
    p_IOSurfaceGetHeight      = dlsym(iosurf, "IOSurfaceGetHeight");
    p_IOSurfaceGetPixelFormat = dlsym(iosurf, "IOSurfaceGetPixelFormat");
    p_IOSurfaceLock           = dlsym(iosurf, "IOSurfaceLock");
    p_IOSurfaceUnlock         = dlsym(iosurf, "IOSurfaceUnlock");
}

/* Read a surface's geometry. Returns 1 if it looks usable. */
static int surface_capture(void *s)
{
    iosurface_init();
    if (!s || !p_IOSurfaceGetBaseAddress) return 0;

    surf_state.ref    = s;
    surf_state.base   = (unsigned)(unsigned long)p_IOSurfaceGetBaseAddress(s);
    surf_state.stride = p_IOSurfaceGetBytesPerRow ? p_IOSurfaceGetBytesPerRow(s) : 0;
    surf_state.width  = p_IOSurfaceGetWidth ? p_IOSurfaceGetWidth(s) : 0;
    surf_state.height = p_IOSurfaceGetHeight ? p_IOSurfaceGetHeight(s) : 0;
    surf_state.format = p_IOSurfaceGetPixelFormat ? p_IOSurfaceGetPixelFormat(s) : 0;

    w("[mbxshim]   surface base="); wd(surf_state.base);
    w(" stride="); wd(surf_state.stride);
    w(" "); wd(surf_state.width); w("x"); wd(surf_state.height);
    w(" fmt="); wd(surf_state.format); w("\n");
    return surf_state.base && surf_state.width && surf_state.height;
}

/*
 * Both bind entry points log their arguments. Which of the two actually carries
 * the render target on 3.1.3 is not settled: in the stock engine
 * GLESBindCoreSurface reaches _DetachTexture, which reads more like the
 * texture-from-surface path than the drawable. Rather than guess from ARM, both
 * record whatever they are given and the log says which one fired with what --
 * the first real CAEAGLLayer client answers it as data.
 */
static int GLESBindCoreSurface(void *gc, void *surf, void *a, void *b)
{
    (void)gc; (void)a; (void)b;
    w("[mbxshim] GLESBindCoreSurface arg1="); wd((unsigned)(unsigned long)surf);
    w("\n");
    surface_capture(surf);
    return 1;
}

static int GLESBindView(void *gc, void *view, void *a, void *b)
{
    (void)gc; (void)a; (void)b;
    w("[mbxshim] GLESBindView arg1="); wd((unsigned)(unsigned long)view);
    w(" arg2="); wd((unsigned)(unsigned long)a);
    w(" arg3="); wd((unsigned)(unsigned long)b); w("\n");
    surface_capture(view);
    return 1;
}

static int GLESFinishTexture(void *gc, void *a) { (void)gc; (void)a; return 0; }

/*
 * Where a frame becomes visible.
 *
 * If CoreAnimation has handed us a surface, render into it and let CA composite
 * -- that is the real path, and the only one that survives CA repainting. The
 * blit straight to the panel is the fallback for clients that never bind one
 * (the direct-trap tests), and it is deliberately second: it bypasses CA
 * entirely, so anything CA draws next overwrites it, which makes a stale frame
 * look like a live one.
 */
static int GLESPresentView(void *gc, void *view)
{
    (void)view;

    if (surf_state.ref && surf_state.base) {
        unsigned lockseed = 0;
        long long r;

        if (p_IOSurfaceLock) p_IOSurfaceLock(surf_state.ref, 0, &lockseed);
        /* Re-read the base each frame: CA is entitled to move or reallocate a
         * surface between frames, and caching it would write into whatever now
         * owns the old address. */
        if (p_IOSurfaceGetBaseAddress) {
            surf_state.base =
                (unsigned)(unsigned long)p_IOSurfaceGetBaseAddress(surf_state.ref);
        }
        r = qc(GLES_OP_PRESENT_SURFACE, gc, 5,
               A(surf_state.base, surf_state.stride, surf_state.width,
                 surf_state.height, surf_state.format));
        if (p_IOSurfaceUnlock) p_IOSurfaceUnlock(surf_state.ref, 0, &lockseed);
        return r == 0;
    }

    qc(GLES_OP_PRESENT, gc, 0, A(0));
    return 1;
}

static int GLESSwapNotification(void *gc, void *a) { (void)gc; (void)a; return 0; }

/*
 * The table GLESGetEGLInterface hands back: nine function pointers, then
 * {extension string, bit} pairs. The framework never indexes past the ninth
 * pointer, so the extension list only has to be well-formed and terminated.
 */
static void *const gles_egl_interface[] = {
    /* +0x00 */ (void *)GLESCreateSharegroup,
    /* +0x04 */ (void *)GLESDestroySharegroup,
    /* +0x08 */ (void *)GLESCreateGC,
    /* +0x0c */ (void *)GLESDestroyGC,
    /* +0x10 */ (void *)GLESBindCoreSurface,
    /* +0x14 */ (void *)GLESBindView,
    /* +0x18 */ (void *)GLESFinishTexture,
    /* +0x1c */ (void *)GLESPresentView,
    /* +0x20 */ (void *)GLESSwapNotification,
    /* +0x24 onward: extension pairs. Empty list, null-terminated. */
    (void *)0, (void *)0,
};

void *GLESGetEGLInterface(void);
void *GLESGetEGLInterface(void)
{
    w("[mbxshim] GLESGetEGLInterface\n");
    return (void *)gles_egl_interface;
}
