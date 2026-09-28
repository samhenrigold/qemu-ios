/*
 * glishim -- a drop-in replacement for the iPad's (iOS 3.2.2, 7B500)
 * /System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/GLEngine
 *
 * The stock GLEngine drives the SGX through libGFXShared and a gld* driver
 * plugin. This one forwards ES 1.1 and ES 2.0 calls to QEMU over the same
 * guest-services channel mbxshim uses, so it is mbxshim with a GLI front end:
 * the marshalling, the hand-written ES1 thunks, and the CoreAnimation drawable
 * handling are all mbxshim.c, included below. The ABI is in
 * docs/ipad1/userland-gl-display.md section 3; the slot table is
 * docs/ipad1/gli-dispatch-<BUILD>.tsv, turned into gli_fwd.h by gligen.py
 * (one GLEngine-<BUILD> per dispatch layout; 7B500's also serves 7B367).
 *
 * What differs from the MBX path:
 *   - 826 dispatch slots, not 822. 3.2 inserted three at 761, so the 3.1.3
 *     numbers mbxshim fills are moved up by 3 from 761 on. The WIRE numbers
 *     sent to the host stay 3.1.3's, so gles-host.c needs no change for ES1.
 *   - EAGL binds the CA drawable itself (renderbufferStorage:fromDrawable:)
 *     and tells us about its surface through gliSetInteger(0x38E) and
 *     gliBindViewES; we only present into it.
 *   - The host has no ES2 executor yet: ES2 slots are forwarded with their
 *     3.2 numbers (identical to 3.1.3 below 761) and land in its
 *     unhandled-slot warning until it grows one.
 */

#ifndef GLI_NO_BATCH             /* -DGLI_NO_BATCH: one trap per call, for A/B timing */
#define GLES_BATCH
#endif
#define GLISHIM                  /* mbxshim.c: the host-call code and handlers only, not its MBX table */
#include "../it-gles/mbxshim.c"
#include "gli_fwd.h"

/*
 * Command buffer. Each trap is a guest exception round trip, so a call that
 * returns nothing and hands the host no guest pointer (gli_batchable, from gligen.py) is queued in its GC's buffer instead:
 * [slot | argc << 16, args...]. The buffer goes to the host as one
 * GLES_OP_BATCH trap when the same GC makes any other call -- a query, a
 * pointer call, a draw, glFlush/glFinish, present, context teardown -- or when
 * it fills. Deferring such calls is invisible to the guest: nothing it can
 * observe happens between them and the next unbatchable call.
 */
#ifdef GLES_BATCH
#define GLES_OP_BATCH   0x1009
#define BATCH_WORDS     4096

static void batch_flush(GuestGC *gc)
{
    unsigned n = gc->batch_len;

    if (!n) return;
    gc->batch_len = 0;
    /* Direct trap: GLES_OP_BATCH is never batchable itself. */
    qc(GLES_OP_BATCH, gc, 2, A((unsigned)(unsigned long)gc->batch, n));
}

static int gles_batch(unsigned slot, void *gcp, unsigned argc, const unsigned *args)
{
    GuestGC *gc = gcp;
    unsigned i;

    if (!gc || !gc->host) return 0;
    if (slot >= sizeof(gli_batchable) || !gli_batchable[slot] || argc > 12) {
        if (slot != GLES_OP_BATCH) batch_flush(gc);
        return 0;
    }
    if (!gc->batch && !(gc->batch = calloc(BATCH_WORDS, sizeof(unsigned)))) return 0;
    if (gc->batch_len + 1 + argc > BATCH_WORDS) batch_flush(gc);
    gc->batch[gc->batch_len++] = slot | argc << 16;
    for (i = 0; i < argc; i++) gc->batch[gc->batch_len++] = args[i];
    return 1;
}
#endif

/*
 * A slot nothing implements: report it once, by name, to the host log
 * (mbxshim's w() goes to fd 2 and through GLES_OP_LOG to QEMU's stderr), so
 * an app that renders wrong names the entry point it lost. Returns 0.
 */
static int gli_unimpl(unsigned slot)
{
    static unsigned char seen[GLI_N_SLOTS];
    if (slot < GLI_N_SLOTS && !seen[slot]) {
        seen[slot] = 1;
        w("[glishim] unimplemented GL entry point "); w(gli_slot_names[slot]);
        w(" (dispatch slot "); wd(slot); w(")\n");
    }
    return 0;
}

extern char *getenv(const char *);

#define GL_RENDERBUFFER 0x8D41

/* The real GLEngine's renderer id is 0x20000-based; the value is not read. */
typedef struct { void *next; unsigned renderer, flags, pad[10]; } GLIPixelFormat;
#define GLI_PF_ACCELERATED 0x100

static void *(*p_IOSurfaceLookup)(unsigned);
static unsigned (*p_IOSurfaceGetID)(void *);
static void (*p_CFRelease)(const void *);

static void gli_iosurface_init(void)
{
    iosurface_init();
    if (p_IOSurfaceLookup || !iosurf) return;
    p_IOSurfaceLookup = dlsym(iosurf, "IOSurfaceLookup");
    p_IOSurfaceGetID  = dlsym(iosurf, "IOSurfaceGetID");
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/"
                      "CoreFoundation", RTLD_NOW);
    if (cf) p_CFRelease = dlsym(cf, "CFRelease");
}

/* glGetString has to answer for the context's API: an ES2 app checks
 * GL_VERSION and GL_SHADING_LANGUAGE_VERSION before it compiles anything. */
static const char *gli_getString(void *gc, unsigned name)
{
    if (!gc || ((GuestGC *)gc)->api != 2) return s_getString(gc, name);
    switch (name) {
    case 0x1F00: return "Imagination Technologies";
    case 0x1F01: return "PowerVR SGX 535";
    case 0x1F02: return "OpenGL ES 2.0";
    case 0x8B8C: return "OpenGL ES GLSL ES 1.00";
    case 0x1F03: return "GL_IMG_texture_compression_pvrtc";
    default:     return "";
    }
}

/* The host reads strings with a debug read that cannot fault pages in; touch
 * them here first, as mbxshim does for texture uploads. */
static int gli_shaderSource(void *gc, unsigned sh, unsigned count,
                            unsigned strs, unsigned lens)
{
    const char *const *s = (const char *const *)(unsigned long)strs;
    const int *l = (const int *)(unsigned long)lens;
    unsigned i;
    if (s && count < 4096) {
        guest_fault_read(strs, count * 4);
        if (l) guest_fault_read(lens, count * 4);
        for (i = 0; i < count; i++) {
            unsigned n = l && l[i] >= 0 ? (unsigned)l[i] : slen(s[i]) + 1;
            guest_fault_read((unsigned long)s[i], n);
        }
    }
    return (int)qc(GLI_SLOT_glShaderSource, gc, 4, A(sh, count, strs, lens));
}
static int gli_bindAttribLocation(void *gc, unsigned p, unsigned idx, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(GLI_SLOT_glBindAttribLocation, gc, 3, A(p, idx, name)); }
static int gli_getAttribLocation(void *gc, unsigned p, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(GLI_SLOT_glGetAttribLocation, gc, 2, A(p, name)); }
static int gli_getUniformLocation(void *gc, unsigned p, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(GLI_SLOT_glGetUniformLocation, gc, 2, A(p, name)); }

/* GLI_N_SLOTS slots in this firmware's numbering. Hand-written mbxshim thunks
 * (3.1.3 numbering, gli_slot313) win, then the generated forwarders, then the
 * log-once stubs. */
static void gli_fill(void **front, void **back, void *const *mbx)
{
    unsigned i;
    for (i = 0; i < GLI_N_SLOTS; i++) {
        int old = gli_slot313[i];
        void *fn = gli_fwd_table[i];
        if (old >= 0 && old < GLES_N_SLOTS && mbx[old] != gles_default_table[old])
            fn = mbx[old];
        front[i] = fn;
    }
    front[GLI_SLOT_glGetString] = (void *)gli_getString;
    front[GLI_SLOT_glShaderSource] = (void *)gli_shaderSource;
    front[GLI_SLOT_glBindAttribLocation] = (void *)gli_bindAttribLocation;
    front[GLI_SLOT_glGetAttribLocation] = (void *)gli_getAttribLocation;
    front[GLI_SLOT_glGetUniformLocation] = (void *)gli_getUniformLocation;
    if (back)
        for (i = 0; i < GLI_N_SLOTS; i++) back[i] = front[i];
}

/* ------------------------------------------------------------ gli* ABI --- */

/*
 * 4.x: EAGL creates a sharegroup only through libGFXShared's
 * gfxCreateSharedState, which needs a registered gld plugin and device for the
 * pixel format's renderer ID. The stock GLEngine registers them here, passing
 * EAGL's arguments on to gfxInitializeLibrary with its IOSurface callback
 * inserted, then gfxPluginConnectAll; so does this one, and
 * gfxPluginConnectAll finds gldshim (GLRendererFloatQEMU.bundle, see
 * gldshim.c). 3.2.x's OpenGLES never loads libGFXShared, so the lookups fail
 * there and nothing changes. gli_device is gldshim's device ID, 0 until it is
 * registered: then pixel formats stay unaccelerated and 4.x EAGL makes no
 * context (CA stays in software), and the log says why.
 */
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)-2)
#endif
#define GLD_RENDERER 0x7000                             /* gldshim.c's */
#define GLD_DEVICE (1u << 24 | 0x20000 | GLD_RENDERER)  /* libGFXShared: first device of that plugin */
static unsigned gli_device;
static int gli_eagl4;                   /* 4.x EAGL: libGFXShared loaded */
static void *gli_no_surface(void) { return 0; }

void gliInitializeLibrary(const void *svcs, unsigned z, unsigned n, void *cb,
                          void *u, void *io, void *init)
{
    void (*init_lib)(const void *, unsigned, unsigned, void *, void *, void *, void *) =
        dlsym(RTLD_DEFAULT, "gfxInitializeLibrary");
    void (*connect)(void) = dlsym(RTLD_DEFAULT, "gfxPluginConnectAll");
    void *(*plugin)(unsigned) = dlsym(RTLD_DEFAULT, "gfxGetPluginWithDriverID");
    void *(*device)(unsigned) = dlsym(RTLD_DEFAULT, "gfxGetDeviceWithDeviceID");
    (void)u;
    w("[glishim] gliInitializeLibrary\n");
    if (!init_lib) return;                   /* 3.2.x */
    gli_eagl4 = 1;
    if (!connect || !plugin || !device) {
        w("[glishim] libGFXShared lacks gfxPluginConnectAll/gfxGet*WithID: no GL\n");
        return;
    }
    init_lib(svcs, z, n, cb, (void *)gli_no_surface, io, init);
    connect();
    if (plugin(GLD_DEVICE & 0xffff00) && device(GLD_DEVICE & ~0xffu)) {
        gli_device = GLD_DEVICE;
        w("[glishim] gld plugin registered, device "); wx(GLD_DEVICE); w("\n");
    } else
        w("[glishim] libGFXShared registered no gldshim device (GLRendererFloatQEMU.bundle missing?): no GL\n");
}

void gliTerminateLibrary(void) {}

/* Nonzero is OK. EAGL dlcloses the engine if this reports no plugin. */
int gliGetVersion(int *major, int *minor, int *renderer)
{
    if (major) *major = 2;
    if (minor) *minor = 3;
    if (renderer) *renderer = 0x20000;
    return 1;
}

/*
 * One pixel format. The accelerated bit decides whether CoreAnimation's own
 * compositor (which asks for Accelerated=YES, ES2) comes here too; apps
 * default to Accelerated=NO and get a context either way. Off unless
 * GLI_ACCELERATED is set in the process environment, since CA in software is
 * the path that is known to work.
 */
int gliChoosePixelFormat(GLIPixelFormat **out, const int *attribs)
{
    (void)attribs;
    if (!out) return 10014;
    GLIPixelFormat *pf = calloc(1, sizeof(*pf));
    if (!pf) return 10014;
    pf->renderer = gli_device ? gli_device : 0x20000 | 0x2000;
    /* 4.x EAGL only loads accelerated formats */
    pf->flags = gli_device || getenv("GLI_ACCELERATED") ? GLI_PF_ACCELERATED : 0;
    *out = pf;
    return 0;
}

void gliDestroyPixelFormat(GLIPixelFormat *pf)
{
    while (pf) { GLIPixelFormat *n = pf->next; free(pf); pf = n; }
}

int gliQueryRendererInfo(void) { return 10015; }
int gliDestroyRendererInfo(void) { return 0; }

/*
 * EAGL calls this twice per API: once from -[EAGLSharegroup loadGLIPlugin:]
 * with share == NULL (the sharegroup's own context) and once per EAGLContext
 * with share == that one. So a NULL share makes a host sharegroup, and every
 * context created against it joins that group. api_bits: 4 = ES1, 8 = ES2.
 */
/* A context on host sharegroup sg (a new one if 0; then owns = 1). api_bits 4
 * = ES1, 8 = ES2; if both or neither, the sharegroup's other contexts' API. */
static int gli_new_context(void **out, void *sg, int owns, int api_else,
                           void **front, void **back, unsigned api_bits)
{
    void *mbx[GLES_N_SLOTS];
    GuestGC *gc = 0;

    if (!out || !front) return 10014;
    if (!sg) {
        void *p = 0;
        if (!GLESCreateSharegroup(&p)) return 10015;
        sg = p;
    }
    if (!GLESCreateGC(sg, mbx, 0, (void **)&gc)) {
        if (owns) GLESDestroySharegroup(sg);
        return 10015;
    }
    gc->sg = sg;
    gc->owns_sg = owns;
    gc->api = (api_bits & 8) && !(api_bits & 4) ? 2
            : (api_bits & 4) && !(api_bits & 8) ? 1 : api_else;
    gli_fill(front, back, mbx);
    *out = gc;
    w("[glishim] gliCreateContext api="); wd(gc->api);
    return 0;
}

int gliCreateContext(void **out, GLIPixelFormat *pf, void *share,
                     void **front, void **back, unsigned api_bits)
{
    GuestGC *sh = share;
    int r = gli_new_context(out, sh ? sh->sg : 0, !sh, sh ? sh->api : 1, front, back, api_bits);
    (void)pf;
    if (!r) w(share ? " shared\n" : " root\n");
    return r;
}

/*
 * 4.x's EAGL creates every context of a GLI sharegroup through this one:
 * pf is the pixel format EAGLSharegroup embeds (_EAGLSharegroupPrivate + 0xc),
 * so its address identifies the group; shared is its gfxCreateSharedState
 * object, libGFXShared state this engine never sets up. The group's contexts
 * share one host sharegroup, released with the last of them (owns_sg = 2).
 */
#define GLI_GROUPS 16
static struct { const void *key; void *sg; int refs, api; } gli_groups[GLI_GROUPS];

int gliCreateContextWithShared(void **out, GLIPixelFormat *pf, void *shared,
                               void **front, void **back, unsigned api_bits)
{
    int i, free_ = -1, r;
    (void)shared;
    for (i = 0; i < GLI_GROUPS; i++) {
        if (gli_groups[i].refs && gli_groups[i].key == pf) break;
        if (!gli_groups[i].refs && free_ < 0) free_ = i;
    }
    if (i == GLI_GROUPS && (i = free_) < 0) return 10015;
    r = gli_new_context(out, gli_groups[i].refs ? gli_groups[i].sg : 0, 2,
                        gli_groups[i].refs ? gli_groups[i].api : 1, front, back, api_bits);
    if (r) return r;
    if (!gli_groups[i].refs) {
        gli_groups[i].key = pf;
        gli_groups[i].sg = ((GuestGC *)*out)->sg;
        gli_groups[i].api = ((GuestGC *)*out)->api;
    }
    gli_groups[i].refs++;
    w(gli_groups[i].refs > 1 ? " group, shared\n" : " group, root\n");
    return 0;
}

/* Drop one context's hold on its group; nonzero when it was the last. */
static int gli_group_put(void *sg)
{
    for (int i = 0; i < GLI_GROUPS; i++)
        if (gli_groups[i].refs && gli_groups[i].sg == sg)
            return --gli_groups[i].refs == 0;
    return 1;
}

/* IOSurfaceLookup's reference to each view's attached surface. It has to be
 * held: the lookup can hand back a fresh object, and releasing it at once
 * left v->ref dangling for the first present to lock. */
static void *gli_owned[CA_MAX_VIEWS];

static void gli_own(ca_view_t *v, void *surf)
{
    void **slot = &gli_owned[v - ca_views];
    if (*slot && p_CFRelease) p_CFRelease(*slot);
    *slot = surf;
}

/* EAGL owns the CA binding in the GLI path, so the view is forgotten, not
 * unbound, and the sharegroup goes with the context that created it. */
int gliDestroyContext(void *ctx)
{
    GuestGC *gc = ctx;
    ca_view_t *v = ca_view_for_gc(gc, 0);
    void *sg;
    int owns;

    if (!gc) return 10014;
    sg = gc->sg;
    owns = gc->owns_sg;
    if (v) { v->drawable = 0; gli_own(v, 0); }
    GLESDestroyGC(gc);
    if (owns == 1 || (owns == 2 && gli_group_put(sg))) GLESDestroySharegroup(sg);
    return 0;
}

/* An IOSurface EAGL attached as the colour renderbuffer: remember it as this
 * context's view surface and size the host drawable to it. */
static int gli_attach_renderbuffer(GuestGC *gc, void *surf, unsigned wd_, unsigned ht)
{
    ca_view_t *v = ca_view_for_gc(gc, 1);
    if (!v || !surface_capture(v, surf)) return 10014;
    gli_own(v, surf);
    v->need_buffer = 0;
    return qc(GLES_OP_DRAWABLE_STORAGE, gc, 2, A(wd_, ht)) < 0 ? 10014 : 0;
}

/*
 * CoreAnimation's GL compositor queues each IOMFB swap to wait for a token the
 * GPU driver sends once the frame is rendered; with no token the swaps never
 * complete and the display stays off. EAGL hands us the framebuffer's
 * IOMobileFramebufferGetID, not a connection, so signal the main display the
 * way EAGL does for a non-accelerated context (IOMobileFramebufferSwapSignal,
 * selector 20 on the framebuffer's connection). The iPad has one display.
 */
static int gli_swap_signal(void *gc, unsigned txn, unsigned layer)
{
    static int (*get_main)(void **);
    static int (*signal)(void *, unsigned, unsigned);
    static void *fb;

    if (!signal) {
        void *h = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/"
                         "IOMobileFramebuffer", RTLD_NOW);
        if (h) {
            get_main = dlsym(h, "IOMobileFramebufferGetMainDisplay");
            signal = dlsym(h, "IOMobileFramebufferSwapSignal");
        }
    }
    if (!fb && get_main) get_main(&fb);
    if (!fb || !signal) return 10015;
    qc(89, gc, 0, A(0));        /* glFinish: the frame is in the surface first */
    return signal(fb, txn, layer) ? 10014 : 0;
}

int gliSetInteger(void *gc, unsigned pname, const int *v)
{
    void *surf;
    int r;

    static unsigned logged;
    if (!gc) return 10014;
    if (logged++ < 64) {
        w("[glishim] gliSetInteger "); wx(pname);
        if (v) { w(" "); wx((unsigned)v[0]); w(" "); wx((unsigned)v[1]); }
        w("\n");
    }
    switch (pname) {
    case 0x38E:     /* attach IOSurface {id, target, ifmt, w, h, fmt, type, 0} */
        if (!v) return 10014;
        gli_iosurface_init();
        if (!p_IOSurfaceLookup || !(surf = p_IOSurfaceLookup((unsigned)v[0])))
            return 10014;
        if (v[1] == GL_RENDERBUFFER) {
            r = gli_attach_renderbuffer(gc, surf, (unsigned)v[3], (unsigned)v[4]);
            if (r && p_CFRelease) p_CFRelease(surf);
            return r;
        }
        r = GLESBindCoreSurface(gc, (unsigned)v[1], surf) ? 0 : 10014;
        if (p_CFRelease) p_CFRelease(surf);   /* the host copied what it needs */
        return r;
    case 0x39B: {   /* detach {id, target} */
        ca_view_t *view = ca_view_for_gc(gc, 0);
        if (!v) return 10014;
        if (v[1] != GL_RENDERBUFFER) {
            GLESBindCoreSurface(gc, (unsigned)v[1], 0);
            return 0;
        }
        gli_iosurface_init();
        /* Presenting into a destroyed surface writes into freed memory. */
        if (view && view->ref && p_IOSurfaceGetID &&
            p_IOSurfaceGetID(view->ref) == (unsigned)v[0]) {
            view->ref = 0;
            view->base = 0;
            gli_own(view, 0);
        }
        return 0;
    }
    case 0x2C1:     /* swap notification {framebuffer ID, transaction, layer} */
        if (!v) return 10014;
        return gli_swap_signal(gc, (unsigned)v[1], (unsigned)v[2]);
    default:        /* 0x399 flip, 0x3E3 legacy flag, 0x7AA profiler, app params */
        return 0;
    }
}

int gliGetInteger(void *gc, unsigned pname, int *v)
{
    (void)gc; (void)pname;
    if (v) *v = 0;
    return 0;
}

/* NULL releases the view. Otherwise EAGL has already bound the drawable and
 * attached its first surface (0x38E); later frames' surfaces come from
 * drawable->nextBuffer inside GLESPresentView, as on the MBX path.
 *
 * NULL also UNBINDS the old drawable (vt[2]), as the stock engine does
 * (_gliBindViewES 0xe6d02: NULL with a view on the bound framebuffer calls
 * [view+8]). renderbufferStorage:fromDrawable: starts with BindView(NULL) and
 * then binds the layer again; CA refuses that bind while the layer still
 * belongs to the old binding, so an app that rebuilds its framebuffer (Super
 * Monkey Ball, on its rotate to landscape) got NO and presented nowhere. */
/*
 * 4.x's EAGL leaves the drawable to the engine: -renderbufferStorage:fromDrawable:
 * only calls gliBindViewES, and the stock engine binds CA's drawable itself
 * (drawable->bind(fourcc, block), the first nextBuffer, then its own 0x38E
 * attach of that surface as GL_RENDERBUFFER), as mbxshim's GLESBindView does
 * for 3.1.3. The 4.x block has a third callback, preflight(ctx, a, b, c),
 * which the stock engine answers with gliGetInteger(0x25B, {a, b, c})[0]; we
 * leave a as it is. 3.2.x's EAGL did all of this before calling here.
 */
static unsigned gli_preflight(void *ctx, unsigned a, unsigned b, unsigned c)
{
    (void)ctx; (void)b; (void)c;
    return a;
}

static int gli_bind_view4(void *gc, void *drawable, unsigned ifmt)
{
    void **vt = drawable;
    ca_view_t *v = ca_view_for_gc(gc, 1);
    int rgb565 = ifmt == GL_RGB565_OES, a[9];
    unsigned flags = 0, t;
    void *s;

    if (!v) return 0;
    ca_detach_view(v);
    v->gc = gc;
    v->block[0] = v->block;             /* createBuffer's arg0: finds this view */
    v->block[1] = (void *)ca_create_buffer;
    v->block[2] = (void *)ca_destroy_buffer;
    v->block[3] = (void *)gli_preflight;
    v->block[4] = v->block[6] = v->block[7] = 0;
    v->block[5] = gc;
    if (!((ca_bind_fn)vt[1])(drawable, rgb565 ? CA_FOURCC_565L : CA_FOURCC_BGRA, v->block)) {
        w("[glishim] drawable->bind failed\n");
        return 0;
    }
    v->drawable = drawable;
    if (*(int *)drawable > 1) flags = ((unsigned (*)(void *))vt[6])(drawable) & ~8u;
    if (!(s = ((ca_next_fn)vt[3])(drawable))) {
        ((ca_unbind_fn)vt[2])(drawable);
        v->drawable = 0;
        w("[glishim] drawable->nextBuffer gave no surface\n");
        return 0;
    }
    gli_iosurface_init();
    if (!p_IOSurfaceGetID || !p_IOSurfaceGetWidth || !p_IOSurfaceGetHeight) return 0;
    a[0] = (int)p_IOSurfaceGetID(s);
    a[1] = GL_RENDERBUFFER;
    a[2] = (int)ifmt;
    a[3] = (int)p_IOSurfaceGetWidth(s);
    a[4] = (int)p_IOSurfaceGetHeight(s);
    if (flags & 4) { t = a[3]; a[3] = a[4]; a[4] = (int)t; }
    a[5] = rgb565 ? 0x1907 : 0x80E1;    /* GL_RGB / GL_BGRA */
    a[6] = rgb565 ? 0x8363 : 0x8367;    /* 5_6_5 / 8_8_8_8_REV */
    a[7] = 0;
    a[8] = (int)flags;
    w("[glishim] bound CA drawable, surface "); wd((unsigned)a[3]); w("x"); wd((unsigned)a[4]); w("\n");
    return gliSetInteger(gc, 0x38E, a) == 0;
}

/* Nonzero on success: 4.x EAGL returns it from -renderbufferStorage:fromDrawable:. */
int gliBindViewES(void *gc, void *drawable, unsigned char retained, int a, int b)
{
    ca_view_t *v = ca_view_for_gc(gc, drawable != 0);
    (void)retained; (void)b;
    w("[glishim] gliBindViewES drawable="); wx((unsigned long)drawable); w("\n");
    if (!v) return !drawable;
    if (!drawable) {
        ca_view_t empty = {0};
        void **vt = v->drawable;
        if (vt && vt[2]) ((ca_unbind_fn)vt[2])(vt);
        gli_own(v, 0);
        *v = empty;
        return 1;
    }
    if (gli_eagl4) return gli_bind_view4(gc, drawable, (unsigned)a);
    v->drawable = drawable;
    return 1;
}

unsigned char gliPresentViewES(void *gc)
{
    static int logged;
    ca_view_t *v = ca_view_for_gc(gc, 0);
    if (!logged++) w("[glishim] gliPresentViewES\n");
    /* The host writes the frame with a debug write, which cannot fault pages
     * in, and a buffer CA has just allocated may have none mapped yet. Take
     * this frame's buffer now and touch every page of it (GLESPresentView then
     * finds it already taken). */
    if (v && ca_next_buffer(v) && v->base) {
        volatile unsigned char *p = (volatile unsigned char *)(unsigned long)v->base;
        unsigned i, n = v->stride * v->height;
        for (i = 0; i < n; i += 4096) p[i] = p[i];
        if (n) p[n - 1] = p[n - 1];
    }
    return GLESPresentView(gc, 0) != 0;
}

/* Looked up by _eagl_init but never called by OpenGLES 3.2.2. */
int gliAttachDrawable(void) { return 10015; }
int gliAttachDrawableWithOptions(void) { return 10015; }
int gliSwapBuffers(void) { return 10015; }
int gliGetAttribute(void) { return 10015; }
int gliSetAttribute(void) { return 10015; }
int gliCopyAttributes(void) { return 10015; }
