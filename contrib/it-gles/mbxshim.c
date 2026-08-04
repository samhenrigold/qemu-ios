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

static void wx(unsigned long v)
{
    char b[11], *p = b + 10;
    *p = 0;
    if (!v) *--p = '0';
    while (v) { *--p = "0123456789abcdef"[v & 15]; v >>= 4; }
    w("0x"); w(p);
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

/* ---- the fixed-function set a real ES 1.1 game needs ----------------------
 *
 * These are exactly Cube Runner's imports: `nm -u` on the binary lists 41 gl*
 * symbols, the 21 above cover part of it and these 20 cover the rest. Slot
 * numbers come from the framework's own trampolines (see gles.h), not from a
 * table of ours.
 */
static int s_blendFunc(void *gc, unsigned s, unsigned d)
    { return (int)qc(7, gc, 2, A(s, d)); }
static int s_colorPointer(void *gc, unsigned size, unsigned type,
                          unsigned stride, unsigned ptr)
    { return (int)qc(51, gc, 4, A(size, type, stride, ptr)); }
static int s_depthMask(void *gc, unsigned flag)
    { return (int)qc(61, gc, 1, A(flag)); }
static int s_drawElements(void *gc, unsigned mode, unsigned count,
                          unsigned type, unsigned indices)
    { return (int)qc(67, gc, 4, A(mode, count, type, indices)); }
static int s_fogf(void *gc, unsigned pname, unsigned param)
    { return (int)qc(91, gc, 2, A(pname, param)); }
static int s_fogfv(void *gc, unsigned pname, unsigned params)
    { return (int)qc(92, gc, 2, A(pname, params)); }
static int s_hint(void *gc, unsigned target, unsigned mode)
    { return (int)qc(128, gc, 2, A(target, mode)); }
static int s_lightfv(void *gc, unsigned light, unsigned pname, unsigned params)
    { return (int)qc(151, gc, 3, A(light, pname, params)); }
static int s_lineWidth(void *gc, unsigned width)
    { return (int)qc(155, gc, 1, A(width)); }
static int s_materialfv(void *gc, unsigned face, unsigned pname, unsigned params)
    { return (int)qc(171, gc, 3, A(face, pname, params)); }
static int s_multMatrixf(void *gc, unsigned m)
    { return (int)qc(176, gc, 1, A(m)); }
static int s_normalPointer(void *gc, unsigned type, unsigned stride,
                           unsigned ptr)
    { return (int)qc(188, gc, 3, A(type, stride, ptr)); }
static int s_popMatrix(void *gc)
    { return (int)qc(205, gc, 0, A(0)); }
static int s_pushMatrix(void *gc)
    { return (int)qc(210, gc, 0, A(0)); }
static int s_rotatef(void *gc, unsigned an, unsigned x, unsigned y, unsigned z)
    { return (int)qc(248, gc, 4, A(an, x, y, z)); }
static int s_scalef(void *gc, unsigned x, unsigned y, unsigned z)
    { return (int)qc(250, gc, 3, A(x, y, z)); }
static int s_shadeModel(void *gc, unsigned mode)
    { return (int)qc(253, gc, 1, A(mode)); }
static int s_translatef(void *gc, unsigned x, unsigned y, unsigned z)
    { return (int)qc(309, gc, 3, A(x, y, z)); }
static int s_clearDepthf(void *gc, unsigned d)
    { return (int)qc(763, gc, 1, A(d)); }
static int s_frustumf(void *gc, unsigned l, unsigned r, unsigned b,
                      unsigned t, unsigned n, unsigned f)
    { return (int)qc(772, gc, 6, A(l, r, b, t, n, f)); }

/* OES framebuffer objects. EAGL calls these itself inside
 * -renderbufferStorage:fromDrawable:, so a CAEAGLLayer client needs them
 * before it can draw anything at all. */
static int s_genRenderbuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(668, gc, 2, A(n, ids)); }
static int s_bindRenderbuffer(void *gc, unsigned target, unsigned rb)
    { return (int)qc(666, gc, 2, A(target, rb)); }
static int s_deleteRenderbuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(667, gc, 2, A(n, ids)); }
static int s_renderbufferStorage(void *gc, unsigned t, unsigned f,
                                 unsigned wv, unsigned h)
    { return (int)qc(669, gc, 4, A(t, f, wv, h)); }
static int s_getRenderbufferParameteriv(void *gc, unsigned t, unsigned p,
                                        unsigned out)
    { return (int)qc(670, gc, 3, A(t, p, out)); }
static int s_genFramebuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(674, gc, 2, A(n, ids)); }
static int s_bindFramebuffer(void *gc, unsigned target, unsigned fb)
    { return (int)qc(672, gc, 2, A(target, fb)); }
static int s_deleteFramebuffers(void *gc, unsigned n, unsigned ids)
    { return (int)qc(673, gc, 2, A(n, ids)); }
static int s_checkFramebufferStatus(void *gc, unsigned target)
    { return (int)qc(675, gc, 1, A(target)); }
static int s_framebufferRenderbuffer(void *gc, unsigned t, unsigned at,
                                     unsigned rbt, unsigned rb)
    { return (int)qc(679, gc, 4, A(t, at, rbt, rb)); }
static int s_framebufferTexture2D(void *gc, unsigned t, unsigned at,
                                  unsigned tt, unsigned tex, unsigned lvl)
    { return (int)qc(677, gc, 5, A(t, at, tt, tex, lvl)); }
static int s_getFramebufferAttachmentParameteriv(void *gc, unsigned t,
                                                 unsigned at, unsigned p,
                                                 unsigned out)
    { return (int)qc(680, gc, 4, A(t, at, p, out)); }

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
        table[666] = (void *)s_bindRenderbuffer;
        table[667] = (void *)s_deleteRenderbuffers;
        table[668] = (void *)s_genRenderbuffers;
        table[669] = (void *)s_renderbufferStorage;
        table[670] = (void *)s_getRenderbufferParameteriv;
        table[672] = (void *)s_bindFramebuffer;
        table[673] = (void *)s_deleteFramebuffers;
        table[674] = (void *)s_genFramebuffers;
        table[675] = (void *)s_checkFramebufferStatus;
        table[677] = (void *)s_framebufferTexture2D;
        table[679] = (void *)s_framebufferRenderbuffer;
        table[680] = (void *)s_getFramebufferAttachmentParameteriv;

        table[7]   = (void *)s_blendFunc;
        table[51]  = (void *)s_colorPointer;
        table[61]  = (void *)s_depthMask;
        table[67]  = (void *)s_drawElements;
        table[91]  = (void *)s_fogf;
        table[92]  = (void *)s_fogfv;
        table[128] = (void *)s_hint;
        table[151] = (void *)s_lightfv;
        table[155] = (void *)s_lineWidth;
        table[171] = (void *)s_materialfv;
        table[176] = (void *)s_multMatrixf;
        table[188] = (void *)s_normalPointer;
        table[205] = (void *)s_popMatrix;
        table[210] = (void *)s_pushMatrix;
        table[248] = (void *)s_rotatef;
        table[250] = (void *)s_scalef;
        table[253] = (void *)s_shadeModel;
        table[309] = (void *)s_translatef;
        table[763] = (void *)s_clearDepthf;
        table[772] = (void *)s_frustumf;
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
/*
 * ONE OF THESE PER GC, not one for the whole process.
 *
 * Every field below used to be a single global, and an app with more than one
 * CAEAGLLayer therefore had all of its views sharing one drawable, one CA
 * block and one surface. Cube Runner has three GCs and binds two views, so its
 * game layer and its menu layer wrote into the same surface and whichever
 * presented last won -- which is why the panel showed a correct camera with no
 * obstacles.
 *
 * The block in particular CANNOT be shared: CA indexes it, keys its own
 * bookkeeping off it, and hands it back as createBuffer's arg0, so it is how we
 * tell one view's surfaces from another's. Sharing it is also the likeliest
 * reason CA refused the second bind outright -- the real engine allocates this
 * per GC at ctx+0x210.
 */
typedef struct {
    void *gc;                 /* identity; 0 means the slot is free */
    void *drawable;
    void *block[8];           /* CA indexes this -- must be per view */
    void *ref;
    unsigned base, stride, width, height, format;
    unsigned char need_buffer;
} ca_view_t;

#define CA_MAX_VIEWS 4
static ca_view_t ca_views[CA_MAX_VIEWS];

/* Look a view up by the GC that owns it, optionally allocating a slot. */
static ca_view_t *ca_view_for_gc(void *gc, int create)
{
    int i, free_slot = -1;

    for (i = 0; i < CA_MAX_VIEWS; i++) {
        if (ca_views[i].gc == gc) return &ca_views[i];
        if (!ca_views[i].gc && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return 0;
    ca_views[free_slot].gc = gc;
    return &ca_views[free_slot];
}

/* CA hands the block back as createBuffer's arg0; that is the view's identity
 * on the callback side. */
static ca_view_t *ca_view_for_block(void *blk)
{
    int i;

    for (i = 0; i < CA_MAX_VIEWS; i++) {
        if (ca_views[i].gc && (void *)ca_views[i].block == blk) {
            return &ca_views[i];
        }
    }
    return 0;
}

static void *iosurf;    /* IOSurface.framework handle */
static void *(*p_IOSurfaceGetBaseAddress)(void *);
static unsigned (*p_IOSurfaceGetBytesPerRow)(void *);
static unsigned (*p_IOSurfaceGetWidth)(void *);
static unsigned (*p_IOSurfaceGetHeight)(void *);
static unsigned (*p_IOSurfaceGetPixelFormat)(void *);
static int (*p_IOSurfaceLock)(void *, unsigned, unsigned *);
static int (*p_IOSurfaceUnlock)(void *, unsigned, unsigned *);
static unsigned long (*p_IOSurfaceGetTypeID)(void);
static unsigned long (*p_CFGetTypeID)(const void *);

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2

/* Heap bounds checks, so the search below can never dereference a word that
 * merely looks like a pointer. malloc_zone_from_ptr answers "is this address
 * owned by a malloc zone" from the zone's own address ranges, without reading
 * anything at the address itself. */
extern void *malloc_zone_from_ptr(const void *);
extern unsigned long malloc_size(const void *);

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
    p_IOSurfaceGetTypeID      = dlsym(iosurf, "IOSurfaceGetTypeID");
    {
        void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/"
                          "CoreFoundation", RTLD_NOW);
        if (cf) p_CFGetTypeID = dlsym(cf, "CFGetTypeID");
    }
}

/*
 * Try to read a surface's geometry, and REJECT it unless the numbers are
 * self-consistent.
 *
 * The validation is not defensive padding, it is load-bearing. Measured on
 * 3.1.3: the object GLESBindView is handed is NOT an IOSurfaceRef. Calling
 * IOSurfaceGetBaseAddress on it does not fail -- it happily reads whatever is
 * at that offset of some other object and returns garbage (observed:
 * base=3468311840, stride=3885969411, 3852415272x3851223040). Nothing about
 * that is distinguishable from a real surface at the call site.
 *
 * Accepting it would point the host's present at an arbitrary guest address
 * with a nonsense stride. So anything that fails a plausibility check is
 * dropped, the view's surface stays empty, and GLESPresentView falls back to the panel
 * blit rather than scribbling somewhere random.
 */
static int surface_capture(ca_view_t *v, void *s)
{
    unsigned base, stride, width, height, format;

    iosurface_init();
    if (!v || !s || !p_IOSurfaceGetBaseAddress) return 0;

    base   = (unsigned)(unsigned long)p_IOSurfaceGetBaseAddress(s);
    stride = p_IOSurfaceGetBytesPerRow ? p_IOSurfaceGetBytesPerRow(s) : 0;
    width  = p_IOSurfaceGetWidth  ? p_IOSurfaceGetWidth(s)  : 0;
    height = p_IOSurfaceGetHeight ? p_IOSurfaceGetHeight(s) : 0;
    format = p_IOSurfaceGetPixelFormat ? p_IOSurfaceGetPixelFormat(s) : 0;

    w("[mbxshim]   candidate surface base="); wd(base);
    w(" stride="); wd(stride);
    w(" "); wd(width); w("x"); wd(height);
    w(" fmt="); wd(format); w("\n");

    /* A real drawable on this device is at most a screenful and its stride has
     * to cover its width. */
    if (!base || width == 0 || height == 0 ||
        width > 2048 || height > 2048 || stride < width * 4 ||
        stride > width * 4 + 4096) {
        w("[mbxshim]   -> REJECTED (not a plausible IOSurface); "
          "keeping panel fallback\n");
        return 0;
    }

    v->ref    = s;
    v->base   = base;
    v->stride = stride;
    v->width  = width;
    v->height = height;
    v->format = format;
    w("[mbxshim]   -> accepted\n");
    return 1;
}

/* ------------------------------------------ the CoreAnimation drawable ----
 *
 * THE DRAWABLE IS A CALLBACK TABLE, NOT A BUFFER.
 *
 * This was read out of the stock engine (device copy, md5 92ddd55f, so the
 * addresses below are literal file addresses in it) and it settles what two
 * rounds of guessing could not. GLESBindView never dereferences its drawable as
 * data; it tail-calls through it:
 *
 *   0xd7ec  str  r5, [r4, #0x20c]     ; remember the drawable in the engine ctx
 *   0xd7f4  str  r3, [r4, #0x214]     ; r3 = &_GLESCreateBuffer   (0xda88)
 *   0xd804  str  r4, [r4, #0x210]     ; the engine's own context
 *   0xd808  str  r3, [r4, #0x218]     ; r3 = &_GLESDestroyBuffer  (0xd8dc)
 *   0xd80c  str  r8, [r4, #0x224]     ; the GC
 *   0xd814  mov  r1, r10              ; a pixel-format FourCC
 *   0xd818  add  r2, r4, #528         ; = ctx+0x210, i.e. the block just built
 *   0xd81c  ldr  pc, [r5, #0x4]       ; drawable->bind(drawable, fourcc, block)
 *
 * So the engine hands CoreAnimation a {context, functions} closure and CA calls
 * back into it. `_GLESCreateBuffer(ctx, surface)` at 0xda88 takes the
 * IOSurfaceRef as its SECOND ARGUMENT -- its first instructions are
 * IOSurfaceLock(r1) and _ValidateCoreSurface(r1). That is where the surface
 * comes from. It was never anywhere inside the drawable, which is why looking
 * for it there found nothing.
 *
 * THE FORMAT IS 'BGRA', and now for a reason rather than by assumption. r10
 * above is a FourCC selected from the internalformat argument:
 *
 *   GL_RGB565_OES (0x8d62)                        -> 'L565' (0x4c353635)
 *   GL_RGB8/RGBA4/RGB5_A1/RGBA8 (0x8051..0x8058)  -> 'BGRA' (0x42475241)
 *   0 (unspecified)                               -> 'BGRA'
 *   anything else                                 -> GL_INVALID_ENUM, no bind
 *
 * It is the format the engine ASKS CA for, so for an RGBA8 renderbuffer -- what
 * every CAEAGLLayer client gets by default -- the surface really is BGRA.
 *
 * THE REST OF THE TABLE, and the one that actually delivers a frame:
 *
 *   +0x08 unbind(drawable)          -- ONLY on the failure path. GLESBindView
 *                                      reaches it at 0xd864 exclusively when
 *                                      _ViewTextureBeginIfNeeded returned 0,
 *                                      and falls straight into SetError after.
 *                                      Calling it on success releases the
 *                                      drawable CA just handed over.
 *   +0x0c nextBuffer(drawable)      -- returns the IOSurfaceRef to render into
 *                                      for THIS frame.
 *   +0x10 present(drawable, 1)      -- the frame in that surface is finished.
 *
 * nextBuffer is the whole answer to "where does the address come from".
 * _ViewTextureBeginIfNeeded (0xd5ec) is the only caller:
 *
 *   0xd5f4  ldr  r3, [r1, #0x21c]   ; nothing to do unless a buffer is wanted
 *   0xd618  ldr  pc, [r3, #0xc]     ; surface = drawable->nextBuffer(drawable)
 *   0xd628  ldr  r2, [r4, #0x230]   ; then look it up in the buffer list that
 *   0xd634  ldr  r3, [r2, #0x18]    ;   _GLESCreateBuffer built
 *   0xd678  str  r0, [r4, #0x220]   ; and arm the present
 *
 * and GLESPresentView re-arms ctx+0x21c on the way out (0xd70c), so it is
 * exactly ONE nextBuffer per frame. That is also the answer to whether CA can
 * move the surface between frames: it does not merely have the right to, the
 * design hands you a different one each frame -- this is double buffering, and
 * caching the address from bind time would render into the buffer being
 * scanned out.
 *
 * bind, present and nextBuffer all report failure as zero (0xd820
 * `subs r5,r0,#0` then `beq` to SetError; 0xd624 likewise), and so does
 * _GLESCreateBuffer (0xdc50 `mov r0,#1` on the success path only).
 */

#define CA_FOURCC_BGRA 0x42475241
#define CA_FOURCC_565L 0x4c353635

#define GL_RGB565_OES  0x8d62

typedef int (*ca_bind_fn)(void *drawable, unsigned fourcc, void **block);
typedef int (*ca_unbind_fn)(void *drawable);
typedef void *(*ca_next_fn)(void *drawable);
typedef int (*ca_present_fn)(void *drawable, unsigned n);


/* These fire every frame; one log line per process is enough to know the path
 * is live, and a line per frame would change the timing it is reporting on. */
static unsigned char present_logged;
static unsigned present_ok, present_fail;
static unsigned char surface_logged;
/* Mirrors the engine's ctx+0x21c: "a buffer is wanted for the next frame". */


static int ca_next_buffer(ca_view_t *v);

/*
 * The block CA is given. Its shape is the engine's ctx+0x210 verbatim, because
 * CA indexes it and we do not get to choose the layout: [0] is the context
 * passed back to us as arg0, [1] create, [2] destroy, [5] the GC.
 */


static int ca_create_buffer(void *ctx, void *surface)
{
    /* ctx is the block we handed CA at bind time, which names the view. */
    ca_view_t *v = ca_view_for_block(ctx);

    w("[mbxshim] CA createBuffer surface="); wx((unsigned long)surface); w("\n");
    /* Still validated: this is the frame's destination address, and a wrong one
     * is a write to an arbitrary guest page. */
    return surface_capture(v, surface) ? 1 : 0;
}

/*
 * Ask CA for this frame's surface. Mirrors _ViewTextureBeginIfNeeded: at most
 * one call per frame, gated on the same flag the engine keeps at ctx+0x21c.
 */
static int ca_next_buffer(ca_view_t *v)
{
    void **vt;
    void *s;

    if (!v || !v->drawable) return 0;
    vt = v->drawable;
    if (!v->need_buffer) return v->ref != 0;

    s = ((ca_next_fn)vt[3])(v->drawable);
    if (!s) {
        w("[mbxshim] drawable->nextBuffer returned nothing\n");
        return 0;
    }
    v->need_buffer = 0;
    if (s == v->ref) {
        return v->base != 0;
    }
    /* A different surface from last frame is the normal case, not an error:
     * CA rotates buffers. Re-read its geometry rather than assuming it matches
     * the one before. */
    if (!surface_logged) {
        surface_logged = 1;
        w("[mbxshim] drawable->nextBuffer -> "); wx((unsigned long)s); w("\n");
    }
    return surface_capture(v, s);
}

static int ca_destroy_buffer(void *ctx, void *surface)
{
    ca_view_t *v = ca_view_for_block(ctx);

    w("[mbxshim] CA destroyBuffer surface="); wx((unsigned long)surface); w("\n");
    /* Only the view that owns it, so one layer's teardown cannot blind
     * another -- the same rule as the bind failure path. */
    if (v && v->ref == surface) {
        /* Whatever replaces it will arrive through createBuffer. Presenting into
         * a destroyed surface writes into freed memory. */
        v->ref = 0;
        v->base = 0;
    }
    return 1;
}

/*
 * GLESBindCoreSurface is the texture-from-surface path, not the drawable one --
 * which is what its reach into _DetachTexture always suggested, and a real
 * CAEAGLLayer client confirmed by never calling it at all. Its first argument
 * IS an IOSurfaceRef (the stock one compares it against known formats and
 * texture-images it), so it is captured directly, with no callback dance.
 */
static int GLESBindCoreSurface(void *gc, void *surf, void *a, void *b)
{
    /* Texture-from-surface, so it belongs to the calling GC like everything
     * else now -- never called by a CAEAGLLayer client, but if one ever does
     * it must not land in another view's slot. */
    ca_view_t *v = ca_view_for_gc(gc, 1);

    (void)a; (void)b;
    w("[mbxshim] GLESBindCoreSurface arg1="); wx((unsigned long)surf); w("\n");
    surface_capture(v, surf);
    return 1;
}

/*
 * GLESBindView is the drawable path -- the one a CAEAGLLayer actually takes.
 * See the contract above: we do not read a surface out of the drawable, we hand
 * CA a closure and CA calls us back with one.
 */
static int GLESBindView(void *gc, void *drawable, void *ifmt, void *flags)
{
    void **vt = drawable;
    ca_view_t *v;
    int had_drawable;
    unsigned f = (unsigned)(unsigned long)ifmt;
    unsigned fourcc = (f == GL_RGB565_OES) ? CA_FOURCC_565L : CA_FOURCC_BGRA;
    int r;

    (void)flags;
    w("[mbxshim] GLESBindView drawable="); wx((unsigned long)drawable);
    w(" internalformat="); wx(f); w("\n");

    iosurface_init();
    if (!drawable) {
        return 0;
    }

    /*
     * DO NOT install this drawable as the active one until CA has accepted it.
     *
     * This assignment used to happen here, before the bind, and the failure
     * path below zeroed that single global -- so a bind CA REFUSED destroyed
     * the pointer to the drawable that was working. Measured on Cube Runner:
     * the first view binds, presents and composites happily for 24 seconds;
     * the moment a game starts the app binds a SECOND view, CA returns 0 for
     * it, and from that instant there was no drawable at all, so GLESPresentView stopped
     * calling CA's present callback entirely. The app carries on rendering at
     * a full 60 fps and the pixels keep landing in CA's surfaces -- nothing
     * anywhere reports an error -- but CA is never told a frame is ready, so
     * the panel keeps showing whatever it last composited. That is the frozen
     * GL world behind the menus, and it is why the frame counter stays honest
     * while the screen is a photograph.
     *
     * A refused bind now costs the caller its own view and nothing else.
     */
    v = ca_view_for_gc(gc, 1);
    if (!v) {
        w("[mbxshim] GLESBindView: no free view slot\n");
        return 0;
    }
    /*
     * Is this GC already holding a working drawable?
     *
     * ca_view_for_gc returns the EXISTING slot when one GC binds twice, which
     * this app does: it binds successfully, and later binds again for a
     * drawable CA refuses. Freeing the slot on that failure threw away the
     * working view -- the same "a failure clears state the caller does not own"
     * bug as the global ca_drawable, reintroduced one layer down and costing
     * 4164 of 5220 presents, which fell through to the invisible panel blit.
     * A failed re-bind must leave a GC exactly as it found it.
     */
    had_drawable = (v->drawable != 0);
    v->block[0] = v->block;      /* handed back to us as createBuffer's arg0 */
    v->block[1] = (void *)ca_create_buffer;
    v->block[2] = (void *)ca_destroy_buffer;
    v->block[3] = 0;
    v->block[4] = 0;
    v->block[5] = gc;            /* +0x14 in the engine's block */
    v->block[6] = 0;
    v->block[7] = 0;

    r = ((ca_bind_fn)vt[1])(drawable, fourcc, v->block);
    w("[mbxshim]   drawable->bind(fourcc="); wx(fourcc); w(") -> "); wd((unsigned)r);
    w("\n");
    if (!r) {
        /* Leave every other view alone -- and leave THIS one alone too if it
         * already had a drawable. Only release a slot this call created, so a
         * GC that never bound does not hold one forever. */
        if (!had_drawable) {
            v->gc = 0;
        }
        return 0;
    }
    v->drawable = drawable;

    /* The engine asks for the first buffer here, inside the bind, through
     * _ViewTextureBeginIfNeeded. Do the same: it is what makes CA announce its
     * surfaces, and until it happens there is nowhere to render. */
    v->need_buffer = 1;
    if (!ca_next_buffer(v)) {
        /* The one case where the unbind callback is correct -- see the
         * contract above; the stock engine takes exactly this path. */
        if (vt[2]) {
            ((ca_unbind_fn)vt[2])(drawable);
        }
        v->drawable = 0;
        v->gc = 0;
        w("[mbxshim]   bind failed: no buffer from the drawable\n");
        return 0;
    }
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
    /* Whose frame this is. A GC that never bound a view has no slot, and falls
     * through to the panel blit below exactly as before. */
    ca_view_t *v = ca_view_for_gc(gc, 0);

    (void)view;

    /* This frame's target. CA hands out a different surface each frame, so the
     * one to render into is asked for now, not remembered from bind time. */
    ca_next_buffer(v);

    if (v && v->ref && v->base) {
        int rr;
        unsigned lockseed = 0;
        long long r;

        if (p_IOSurfaceLock) p_IOSurfaceLock(v->ref, 0, &lockseed);
        /* Re-read the base each frame: CA is entitled to move or reallocate a
         * surface between frames, and caching it would write into whatever now
         * owns the old address. */
        if (p_IOSurfaceGetBaseAddress) {
            v->base =
                (unsigned)(unsigned long)p_IOSurfaceGetBaseAddress(v->ref);
        }
        r = qc(GLES_OP_PRESENT_SURFACE, gc, 5,
               A(v->base, v->stride, v->width,
                 v->height, v->format));
        if (p_IOSurfaceUnlock) p_IOSurfaceUnlock(v->ref, 0, &lockseed);
        if (r != 0) {
            return 0;
        }

        /*
         * Tell CoreAnimation the surface now holds a finished frame. Until this
         * call CA has no reason to composite: the pixels are in its buffer but
         * nothing has said so. The stock engine does exactly this, and only
         * after its own flush -- GLESPresentView at 0xd6fc is
         * `ldr pc,[r3,#0x10]` with the drawable in r0 and 1 in r1, reached only
         * after _FlushHW has returned.
         */
        if (v->drawable) {
            void **vt = v->drawable;
            rr = ((ca_present_fn)vt[4])(v->drawable, 1);
            /* The engine re-arms its "wants a buffer" flag on the way out of
             * present (0xd70c), which is what makes it one nextBuffer per
             * frame rather than one for the whole context. */
            v->need_buffer = 1;
            /*
             * Count every one of these, not just the first.
             *
             * This used to log once per process, which said only that the FIRST
             * present succeeded -- and the first present succeeding is exactly
             * what a layer that CA later drops looks like. A running tally is
             * the difference between "we never signal" and "we signal and CA
             * refuses", and those have completely different fixes.
             */
            if (rr) present_ok++; else present_fail++;
            if (!present_logged || (present_ok + present_fail) % 300 == 0) {
                present_logged = 1;
                w("[mbxshim] present tally: ok="); wd(present_ok);
                w(" failed="); wd(present_fail);
                w(" (last -> "); wd((unsigned)rr); w(")\n");
            }
            return rr != 0;
        }
        return 1;
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
