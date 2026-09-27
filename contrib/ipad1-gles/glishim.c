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
 * docs/ipad1/gli-dispatch-7B500.tsv, turned into gli_fwd.h by gligen.py.
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

#include "../it-gles/mbxshim.c"
#include "gli_fwd.h"

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
    return (int)qc(592, gc, 4, A(sh, count, strs, lens));
}
static int gli_bindAttribLocation(void *gc, unsigned p, unsigned idx, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(527, gc, 3, A(p, idx, name)); }
static int gli_getAttribLocation(void *gc, unsigned p, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(529, gc, 2, A(p, name)); }
static int gli_getUniformLocation(void *gc, unsigned p, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(623, gc, 2, A(p, name)); }

/* 826 slots in 3.2 numbering. Hand-written mbxshim thunks win, then the
 * generated forwarders, then the log-once stubs. */
static void gli_fill(void **front, void **back, void *const *mbx)
{
    unsigned i;
    for (i = 0; i < GLI_N_SLOTS; i++) {
        /* 3.1.3 slot i >= 761 is 3.2 slot i + 3; 761..763 are new. */
        int old = i < 761 ? (int)i : i < 764 ? -1 : (int)i - 3;
        void *fn = gli_fwd_table[i];
        if (old >= 0 && old < GLES_N_SLOTS && mbx[old] != gles_default_table[old])
            fn = mbx[old];
        front[i] = fn;
        if (back) back[i] = fn;
    }
    front[117] = (void *)gli_getString;
    front[592] = (void *)gli_shaderSource;
    front[527] = (void *)gli_bindAttribLocation;
    front[529] = (void *)gli_getAttribLocation;
    front[623] = (void *)gli_getUniformLocation;
    if (back) {
        back[117] = front[117]; back[592] = front[592]; back[527] = front[527];
        back[529] = front[529]; back[623] = front[623];
    }
}

/* ------------------------------------------------------------ gli* ABI --- */

void gliInitializeLibrary(const void *svcs, unsigned z, unsigned n, void *cb,
                          void *u, void *io, void *init)
{
    (void)svcs; (void)z; (void)n; (void)cb; (void)u; (void)io; (void)init;
    w("[glishim] gliInitializeLibrary\n");
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
    pf->renderer = 0x20000 | 0x2000;
    pf->flags = getenv("GLI_ACCELERATED") ? GLI_PF_ACCELERATED : 0;
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
int gliCreateContext(void **out, GLIPixelFormat *pf, void *share,
                     void **front, void **back, unsigned api_bits)
{
    void *mbx[GLES_N_SLOTS];
    GuestGC *sg = share ? ((GuestGC *)share)->sg : 0;
    GuestGC *gc = 0;
    int owns = 0;
    (void)pf;

    if (!out || !front) return 10014;
    if (!sg) {
        void *p = 0;
        if (!GLESCreateSharegroup(&p)) return 10015;
        sg = p;
        owns = 1;
    }
    if (!GLESCreateGC(sg, mbx, 0, (void **)&gc)) {
        if (owns) GLESDestroySharegroup(sg);
        return 10015;
    }
    gc->sg = sg;
    gc->owns_sg = owns;
    gc->api = (api_bits & 8) && !(api_bits & 4) ? 2
            : (api_bits & 4) && !(api_bits & 8) ? 1
            : share ? ((GuestGC *)share)->api : 1;
    gli_fill(front, back, mbx);
    *out = gc;
    w("[glishim] gliCreateContext api="); wd(gc->api);
    w(share ? " shared\n" : " root\n");
    return 0;
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
    if (v) v->drawable = 0;
    GLESDestroyGC(gc);
    if (owns) GLESDestroySharegroup(sg);
    return 0;
}

/* An IOSurface EAGL attached as the colour renderbuffer: remember it as this
 * context's view surface and size the host drawable to it. */
static int gli_attach_renderbuffer(GuestGC *gc, void *surf, unsigned wd_, unsigned ht)
{
    ca_view_t *v = ca_view_for_gc(gc, 1);
    if (!v || !surface_capture(v, surf)) return 10014;
    v->need_buffer = 0;
    return qc(GLES_OP_DRAWABLE_STORAGE, gc, 2, A(wd_, ht)) < 0 ? 10014 : 0;
}

int gliSetInteger(void *gc, unsigned pname, const int *v)
{
    void *surf;
    int r;

    if (!gc) return 10014;
    switch (pname) {
    case 0x38E:     /* attach IOSurface {id, target, ifmt, w, h, fmt, type, 0} */
        if (!v) return 10014;
        gli_iosurface_init();
        if (!p_IOSurfaceLookup || !(surf = p_IOSurfaceLookup((unsigned)v[0])))
            return 10014;
        r = v[1] == GL_RENDERBUFFER
            ? gli_attach_renderbuffer(gc, surf, (unsigned)v[3], (unsigned)v[4])
            : GLESBindCoreSurface(gc, (unsigned)v[1], surf) ? 0 : 10014;
        /* CA keeps the surface alive until it tells EAGL to destroy it (0x39B). */
        if (p_CFRelease) p_CFRelease(surf);
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
        }
        return 0;
    }
    case 0x2C1:     /* swap notification {framebuffer, transaction, layer} */
        if (!v) return 10014;
        return GLESSwapNotification(gc, (unsigned)v[0], (unsigned)v[1],
                                    (unsigned)v[2]) ? 0 : 10014;
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
 * drawable->nextBuffer inside GLESPresentView, as on the MBX path. */
void gliBindViewES(void *gc, void *drawable, unsigned char retained, int a, int b)
{
    ca_view_t *v = ca_view_for_gc(gc, drawable != 0);
    (void)retained; (void)a; (void)b;
    if (!v) return;
    if (!drawable) {
        ca_view_t empty = {0};
        *v = empty;
        return;
    }
    v->drawable = drawable;
}

unsigned char gliPresentViewES(void *gc)
{
    return GLESPresentView(gc, 0) != 0;
}

/* Looked up by _eagl_init but never called by OpenGLES 3.2.2. */
int gliAttachDrawable(void) { return 10015; }
int gliAttachDrawableWithOptions(void) { return 10015; }
int gliSwapBuffers(void) { return 10015; }
int gliGetAttribute(void) { return 10015; }
int gliSetAttribute(void) { return 10015; }
int gliCopyAttributes(void) { return 10015; }
