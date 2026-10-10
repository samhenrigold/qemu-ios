/*
 * opengles -- the GL bridge's one front end: a drop-in /System/Library/Frameworks/OpenGLES.framework/OpenGLES
 * for every emulated iOS from 2.x to 5.x, iPad 1 and iPod touch 2G, over the one mbxshim core.
 *
 * The seam is the public API (docs/ipad1/gles-public-seam.md): every consumer of GL in every firmware read goes
 * through OpenGLES's exports and EAGL, and that layer barely moves between releases, while everything under it
 * (GLEngine, libGFXShared, the gld plugin, MBXGLEngine's interface) changes at every point release. So this file
 * replaces the framework whole and nothing below it is loaded: no GLEngine, no gld plugin, no libGFXShared.
 *
 *   gl*       one forwarder per name any firmware's OpenGLES exports (opengles.exports, the union of 2.2.1 to
 *             5.1.1), each routed to the core by its gles-names.h row: the hand thunk for the row's wire id if the
 *             core has one, else the generated forwarder. No current context: a no-op returning 0.
 *   egl*      2.x QuartzCore's compositor API (unchanged from gles2x.c): pixmap surfaces over CoreSurface
 *             buffers, window surfaces over a native window.
 *   EAGL      EAGLContext / EAGLSharegroup with every method any firmware's classes have, public and private:
 *             renderbufferStorage:fromDrawable: binds the layer's -nativeWindow (the closure's `version` word says
 *             whether it has 4.x's `properties` entry), presentRenderbuffer: presents into the layer's surface,
 *             attachImage:toCoreSurface:invertedRender: and 4.x's texImageIOSurface:... bind an IOSurface as a
 *             texture or as the renderbuffer, swapNotification: signals the framebuffer's swap as unaccelerated
 *             stock EAGL does (IOMobileFramebufferSwapSignal on the framebuffer it is given), 5.x's
 *             GetMacroContextPrivate hands back {GC, dispatch table} laid out as this firmware's
 *             __GLIFunctionDispatchRec, whose @encode is read out of the stock OpenGLES image in the mapped shared
 *             cache (5.x QuartzCore and CoreImage call GL only through it).
 *
 * Nothing is chosen by build number. What differs between firmwares is found when it is used: the closure's
 * version word, IOSurface or CoreSurface (iosurface_init), ES 2.0 where the firmware ships the SGX engine
 * (OpenGLES.framework/GLEngine.bundle), the dispatch layout where a caller asks for a macro context. FirmwareKit's
 * fit check proves each of these exists in the firmware at prepare time (FitCheck.glesFrontEnd).
 *
 * Linking: legacy (2.x dyld: no LC_DYLD_INFO_ONLY, classic relocations; newer dyld reads it too), r9 reserved
 * (2.x's thread pointer), fat armv6 + armv7, ARM code. The EAGL classes are static ObjC 2 class data bound to
 * CoreFoundation's NSObject, as on every firmware from 2.0 on. Build with build.sh.
 *
 * Nothing runs at load: QuartzCore links this framework into every UIKit process. The first context says hello.
 */
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)-2)
#endif
#define GLES_BATCH
#include "../it-gles/mbxshim.c"

#include <pthread.h>

#define GLES_FN(n, f, id, argc, fl) GLES_ROW_##n,
enum {
#include "../../include/hw/arm/guest-services/gles-names.h"
};
#undef GLES_FN

extern int access(const char *, int);
extern int syscall(int, ...);
extern void *memset(void *, int, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
extern int strcmp(const char *, const char *);

/* ------------------------------------------------------------ the command buffer --- */

/*
 * Each trap is a guest exception round trip, so a call that returns nothing and hands the host no guest pointer
 * (gles_batchable, from the name table) is queued in its GC's buffer instead: [id | argc << 16, args...]. The
 * buffer goes to the host as one GLES_OP_BATCH trap when the same GC makes any other call, when it fills, and
 * when its context stops being current (another context may read what it queued). Nothing the guest can observe
 * happens between the queued calls and the flush.
 */
#define GLES_OP_BATCH   0x1009
#define BATCH_WORDS     4096

static void batch_flush(GuestGC *gc)
{
    unsigned n = gc ? gc->batch_len : 0;
    if (!n) return;
    gc->batch_len = 0;
    qc(GLES_OP_BATCH, gc, 2, A((unsigned)(unsigned long)gc->batch, n));   /* never batchable itself */
}

static int gles_batch(unsigned slot, void *gcp, unsigned argc, const unsigned *args)
{
    GuestGC *gc = gcp;
    unsigned i;
    if (!gc || !gc->host) return 0;
    if (slot >= sizeof(gles_batchable) || !gles_batchable[slot] || argc > 12) {
        if (slot != GLES_OP_BATCH) batch_flush(gc);
        return 0;
    }
    if (!gc->batch && !(gc->batch = calloc(BATCH_WORDS, sizeof(unsigned)))) return 0;
    if (gc->batch_len + 1 + argc > BATCH_WORDS) batch_flush(gc);
    gc->batch[gc->batch_len++] = slot | argc << 16;
    for (i = 0; i < argc; i++) gc->batch[gc->batch_len++] = args[i];
    return 1;
}

static void fe_destroy_gc(GuestGC *gc)
{
    unsigned *batch = gc ? gc->batch : 0;
    GLESDestroyGC(gc);          /* its DELETE_CONTEXT trap flushes the buffer first */
    free(batch);
}

/* ------------------------------------------------------------ the current context --- */

/* Per thread: the GC gl* calls go to, and whose it is (an EAGLContext or an egl context) so
 * +currentContext / eglGetCurrentContext answer with the object that was made current. */
typedef struct { GuestGC *gc; void *owner; int is_egl; void *draw, *read; } fe_cur_t;
static pthread_key_t fe_key;
static pthread_once_t fe_once = PTHREAD_ONCE_INIT;
static void *fe_hand[GLES_N_HAND];

static void fe_key_init(void) { pthread_key_create(&fe_key, free); }

static fe_cur_t *fe_cur(int create)
{
    fe_cur_t *c;
    pthread_once(&fe_once, fe_key_init);
    c = pthread_getspecific(fe_key);
    if (!c && create && (c = calloc(1, sizeof *c))) pthread_setspecific(fe_key, c);
    return c;
}

static void fe_set_current(GuestGC *gc, void *owner, int is_egl, void *draw, void *read)
{
    fe_cur_t *c = fe_cur(gc != 0);
    if (!c) return;
    if (c->gc && c->gc != gc) batch_flush(c->gc);
    c->gc = gc; c->owner = owner; c->is_egl = is_egl; c->draw = draw; c->read = read;
}

static GuestGC *fe_gc(void)
{
    fe_cur_t *c = fe_cur(0);
    return c ? c->gc : 0;
}

/* ------------------------------------------------------------ what this firmware has --- */

/* ES 2.0 is the SGX's: the firmware ships its engine as OpenGLES.framework/GLEngine.bundle (the iPad's; the iPod
 * touch 2G's has MBXGLEngine.bundle, 2.x none), whether or not the binary inside is in the shared cache. */
static int fe_es2(void)
{
    static int have = -1;
    if (have < 0) have = access("/System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle", 0) == 0;
    return have;
}

/* APPLE_sync (6.x and 7.x CoreAnimation fence every frame): the host runs each call to completion before the
 * next, so a fence is signaled the moment it exists. Answered here, nothing goes to the host. */
#define FE_GL_ALREADY_SIGNALED 0x911A
static unsigned fe_fenceSync(void *gc, unsigned condition, unsigned flags)
{
    static unsigned next;
    (void)gc; (void)condition; (void)flags;
    return ++next ? next : ++next;                          /* 0 is no sync */
}
static unsigned fe_isSync(void *gc, unsigned sync) { (void)gc; return sync != 0; }
static void fe_deleteSync(void *gc, unsigned sync) { (void)gc; (void)sync; }
static unsigned fe_clientWaitSync(void *gc, unsigned sync, unsigned flags, unsigned lo, unsigned hi)
{
    (void)gc; (void)sync; (void)flags; (void)lo; (void)hi;
    return FE_GL_ALREADY_SIGNALED;
}
static void fe_waitSync(void *gc, unsigned sync, unsigned flags, unsigned lo, unsigned hi)
{
    (void)gc; (void)sync; (void)flags; (void)lo; (void)hi;
}
static void fe_getInteger64v(void *gc, unsigned pname, unsigned *params)   /* MAX_SERVER_WAIT_TIMEOUT: 0 */
{
    (void)gc; (void)pname;
    if (params) params[0] = params[1] = 0;
}
static void fe_getSynciv(void *gc, unsigned sync, unsigned pname, int bufsize, int *length, int *values)
{
    int v = pname == 0x9112 ? 0x9116 : pname == 0x9113 ? 0x9117 : pname == 0x9114 ? 0x9119 : 0;
    (void)gc; (void)sync;              /* OBJECT_TYPE: SYNC_FENCE, CONDITION: GPU_COMMANDS_COMPLETE, STATUS: SIGNALED */
    if (bufsize > 0 && values) values[0] = v;
    if (length) *length = bufsize > 0 ? 1 : 0;
}

static const char *fe_getString(void *gc, unsigned name);
static int fe_getIntegerv(void *gc, unsigned pname, unsigned params);
static int fe_getFloatv(void *gc, unsigned pname, unsigned params);
static int fe_getBooleanv(void *gc, unsigned pname, unsigned params);

/* The host reads a string argument with a debug read that cannot fault pages in: touch them first. */
static int fe_shaderSource(void *gc, unsigned sh, unsigned count, unsigned strs, unsigned lens)
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
    return (int)qc(GLES_ID_glShaderSource, gc, 4, A(sh, count, strs, lens));
}
static int fe_bindAttribLocation(void *gc, unsigned p, unsigned idx, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(GLES_ID_glBindAttribLocation, gc, 3, A(p, idx, name)); }
static int fe_getAttribLocation(void *gc, unsigned p, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(GLES_ID_glGetAttribLocation, gc, 2, A(p, name)); }
static int fe_getUniformLocation(void *gc, unsigned p, unsigned name)
    { guest_fault_read(name, slen((const char *)(unsigned long)name) + 1);
      return (int)qc(GLES_ID_glGetUniformLocation, gc, 2, A(p, name)); }

/* The first context: hello, the hand table, and what this front end found. */
static void fe_hello(void)
{
    static int done;
    long long hello;
    unsigned n = 0, hand = 0;
    if (done) return;
    done = 1;
    gles_hand_table(fe_hand);
    fe_hand[GLES_ID_glGetString] = (void *)fe_getString;
    fe_hand[GLES_ID_glGetIntegerv] = (void *)fe_getIntegerv;
    fe_hand[GLES_ID_glGetFloatv] = (void *)fe_getFloatv;
    fe_hand[GLES_ID_glGetBooleanv] = (void *)fe_getBooleanv;
    fe_hand[GLES_ID_glShaderSource] = (void *)fe_shaderSource;
    fe_hand[GLES_ID_glBindAttribLocation] = (void *)fe_bindAttribLocation;
    fe_hand[GLES_ID_glGetAttribLocation] = (void *)fe_getAttribLocation;
    fe_hand[GLES_ID_glGetUniformLocation] = (void *)fe_getUniformLocation;
    fe_hand[GLES_ID_glFenceSyncAPPLE] = (void *)fe_fenceSync;
    fe_hand[GLES_ID_glIsSyncAPPLE] = (void *)fe_isSync;
    fe_hand[GLES_ID_glDeleteSyncAPPLE] = (void *)fe_deleteSync;
    fe_hand[GLES_ID_glClientWaitSyncAPPLE] = (void *)fe_clientWaitSync;
    fe_hand[GLES_ID_glWaitSyncAPPLE] = (void *)fe_waitSync;
    fe_hand[GLES_ID_glGetInteger64vAPPLE] = (void *)fe_getInteger64v;
    fe_hand[GLES_ID_glGetSyncivAPPLE] = (void *)fe_getSynciv;
    hello = gles_hello();
    iosurface_init();
#define GLES2X_FWD(export, row) n++; if (gles_fns[GLES_ROW_##row].id < GLES_N_HAND && fe_hand[gles_fns[GLES_ROW_##row].id]) hand++;
#include "gles2x_exports.h"
#undef GLES2X_FWD
    w("[gles] OpenGLES front end (contrib/gles-public): "); wd(n); w(" gl exports, "); wd(hand);
    w(" to hand thunks, "); wd(n - hand); w(" forwarded (name table version "); wx(GLES_NAMES_VERSION);
    w("), host protocol "); wd((unsigned)(hello & 0xff));
    w(fe_es2() ? "; ES 1.1 + 2.0 (GLEngine.bundle)" : "; ES 1.1 only (no GLEngine.bundle)");
    w(p_IOSurfaceGetID ? "; IOSurface" : p_IOSurfaceGetBaseAddress ? "; CoreSurface" : "; no surfaces");
    w("\n");
    if (hello < 0 || (hello & 0xff) != GLES_HELLO_PROTO) {
        w("[gles] the host speaks another wire protocol: GL calls may go astray\n");
        refused("hello:", "protocol", ~0u);
    }
}

/* One line the first time CoreAnimation's compositor takes a path through this file, so the log proves CA
 * composites through the host (a software CoreAnimation draws the same home screen and never comes here). */
static void fe_ca_path(const char *how)
{
    static int said;
    if (said++) return;
    w("[gles] CoreAnimation composites through the host (first "); w(how); w(")\n");
}

/* ------------------------------------------------------------------------ gl* --- */

typedef int (*fe_f)(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                    unsigned, unsigned, unsigned, unsigned, unsigned);

/* Row `row` on gc. Every core entry point takes (gc, a0..a{argc-1}); AAPCS lets a caller pass
 * more words than the callee reads, so one 12-word signature calls them all. */
static int fe_row(GuestGC *gc, unsigned row, unsigned a0, unsigned a1, unsigned a2, unsigned a3,
                  unsigned a4, unsigned a5, unsigned a6, unsigned a7, unsigned a8, unsigned a9,
                  unsigned a10, unsigned a11)
{
    unsigned id = gles_fns[row].id;
    void *f = id < GLES_N_HAND && fe_hand[id] ? fe_hand[id] : gles_fn_ptr[row];
    return ((fe_f)f)(gc, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}
static int fe_rowv(GuestGC *gc, unsigned row, const unsigned *a)
{
    return fe_row(gc, row, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}
/* A GL call of this file's own, on gc: GL(gc, glBindTexture, target, name). */
#define GL(gc, row, ...) fe_rowv(gc, GLES_ROW_##row, (const unsigned[12]){ __VA_ARGS__ })

#define GLES2X_FWD(export, row)                                                                     \
    int export(unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5,        \
               unsigned a6, unsigned a7, unsigned a8, unsigned a9, unsigned a10, unsigned a11);     \
    int export(unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5,        \
               unsigned a6, unsigned a7, unsigned a8, unsigned a9, unsigned a10, unsigned a11)      \
    {                                                                                               \
        GuestGC *gc = fe_gc();                                                                      \
        return gc ? fe_row(gc, GLES_ROW_##row, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) : 0; \
    }
#include "gles2x_exports.h"
#undef GLES2X_FWD

/* 2.x: (target, CoreSurfaceBuffer); the stock one takes only GL_TEXTURE_RECTANGLE and sets
 * INVALID_ENUM otherwise, which the host's bind does as a counted refusal. */
void glTexImageCoreSurfaceAPPLE(unsigned target, void *buffer);
void glTexImageCoreSurfaceAPPLE(unsigned target, void *buffer)
{
    GuestGC *gc = fe_gc();
    if (gc) GLESBindCoreSurface(gc, target, buffer);
}

const char *glGetString(unsigned name);
const char *glGetString(unsigned name)
{
    GuestGC *gc = fe_gc();
    return gc ? fe_getString(gc, name) : 0;
}

void glFinishTextureAPPLE(unsigned target);
void glFinishTextureAPPLE(unsigned target)
{
    GuestGC *gc = fe_gc();
    if (gc) GLESFinishTexture(gc, target);
}

/* ------------------------------------------------------------------------ egl* --- */

typedef int EGLint;
typedef unsigned EGLBoolean;
#define EGL_SUCCESS 0x3000
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_CONFIG 0x3005
#define EGL_BAD_CONTEXT 0x3006
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_MATCH 0x3009
#define EGL_BAD_NATIVE_PIXMAP 0x300A
#define EGL_BAD_PARAMETER 0x300C
#define EGL_BAD_SURFACE 0x300D
#define EGL_NONE 0x3038
#define EGL_PIXMAP_BIT 0x02
#define EGL_WINDOW_BIT 0x04
#define FE_DISPLAY ((void *)1)
/* gles.h GLES_SURFACE_WINDOW_ORDER: a pixmap's first row is the top of the picture, GL's last
 * (measured: CA's home screen came out upside down in texture order) */
#define FE_WINDOW_ORDER 0x80000000u

/* The three QuartzCore's gles_get_config tells apart (0x31db1278: R,G,B,A of 8,8,8,8 / 5,6,5,0 /
 * 4,4,4,4, and no depth, stencil or samples). */
static const struct { EGLint r, g, b, a; unsigned fourcc, ifmt; } fe_configs[] = {
    { 8, 8, 8, 8, 0x42475241, 0x8058 },     /* 'BGRA', GL_RGBA8_OES */
    { 5, 6, 5, 0, 0x4c353635, 0x8d62 },     /* 'L565', GL_RGB565_OES */
    { 4, 4, 4, 4, 0x34343434, 0x8056 },     /* '4444', GL_RGBA4_OES */
};
#define FE_NCONFIGS 3
static EGLint egl_error = EGL_SUCCESS;       /* ponytail: one for the process, not per thread */

static EGLBoolean egl_fail(EGLint e) { egl_error = e; return 0; }
static void *egl_null(EGLint e) { egl_error = e; return 0; }
static int egl_config(void *c) { unsigned i = (unsigned)(unsigned long)c; return i >= 1 && i <= FE_NCONFIGS ? (int)i - 1 : -1; }

typedef struct egl_surf egl_surf_t;
typedef struct { unsigned magic; GuestGC *gc; void *sg; int config;
                 egl_surf_t *surfaces; } egl_ctx_t;
struct egl_surf {
    unsigned magic, kind;           /* EGL_PIXMAP_BIT or EGL_WINDOW_BIT */
    int config;
    void *native;                   /* the CoreSurfaceBuffer, or the native window */
    unsigned width, height, fourcc;
    GuestGC *gc;                    /* the context its texture and framebuffer names belong to */
    unsigned tex, fbo;
    egl_ctx_t *owner;               /* owns the GC used by cached GL names */
    egl_surf_t *owner_next;
};
static void egl_release_names(egl_surf_t *s);
#define EGL_CTX_MAGIC 0x45474c43
#define EGL_SURF_MAGIC 0x45474c53
static egl_ctx_t *egl_ctx(void *p) { return p && ((egl_ctx_t *)p)->magic == EGL_CTX_MAGIC ? p : 0; }
static egl_surf_t *egl_surf(void *p) { return p && ((egl_surf_t *)p)->magic == EGL_SURF_MAGIC ? p : 0; }

static EGLint egl_config_attrib(int c, EGLint attr)
{
    switch (attr) {
    case 0x3020: return fe_configs[c].r + fe_configs[c].g + fe_configs[c].b + fe_configs[c].a;
    case 0x3024: return fe_configs[c].r;
    case 0x3023: return fe_configs[c].g;
    case 0x3022: return fe_configs[c].b;
    case 0x3021: return fe_configs[c].a;
    case 0x3028: return c + 1;                              /* EGL_CONFIG_ID */
    case 0x3033: return EGL_PIXMAP_BIT | EGL_WINDOW_BIT;    /* EGL_SURFACE_TYPE */
    case 0x302E: return (EGLint)fe_configs[c].fourcc;       /* EGL_NATIVE_VISUAL_ID */
    case 0x3040: return 1;                                  /* EGL_RENDERABLE_TYPE: ES 1 */
    case 0x3027: return 0x3038;                             /* EGL_CONFIG_CAVEAT: none */
    case 0x302C: case 0x302A: return 2048;                  /* MAX_PBUFFER_WIDTH/HEIGHT */
    case 0x302D: return 0;                                  /* MAX_SWAP_INTERVAL... */
    default: return 0;                                      /* depth, stencil, samples: none */
    }
}

void *eglGetDisplay(void *native);
void *eglGetDisplay(void *native) { (void)native; return FE_DISPLAY; }
EGLBoolean eglInitialize(void *dpy, EGLint *major, EGLint *minor);
EGLBoolean eglInitialize(void *dpy, EGLint *major, EGLint *minor)
{
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (major) *major = 1;
    if (minor) *minor = 1;
    return 1;
}
EGLBoolean eglTerminate(void *dpy);
EGLBoolean eglTerminate(void *dpy) { return dpy == FE_DISPLAY ? 1 : egl_fail(EGL_BAD_DISPLAY); }
EGLint eglGetError(void);
EGLint eglGetError(void) { EGLint e = egl_error; egl_error = EGL_SUCCESS; return e; }
const char *eglQueryString(void *dpy, EGLint name);
const char *eglQueryString(void *dpy, EGLint name)
{
    (void)dpy;
    switch (name) {
    case 0x3053: return "Imagination Technologies";   /* EGL_VENDOR */
    case 0x3054: return "1.1";                        /* EGL_VERSION */
    case 0x3055: return "";                           /* EGL_EXTENSIONS */
    default: return egl_null(EGL_BAD_PARAMETER);
    }
}
/* Apple's own switch (QuartzCore calls it with 1 before eglGetDisplay): nothing to switch here. */
EGLBoolean eglEnableInternalSurface(EGLint on);
EGLBoolean eglEnableInternalSurface(EGLint on) { (void)on; return 1; }

EGLBoolean eglGetConfigs(void *dpy, void **configs, EGLint size, EGLint *num);
EGLBoolean eglGetConfigs(void *dpy, void **configs, EGLint size, EGLint *num)
{
    EGLint i, n = 0;
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!num) return egl_fail(EGL_BAD_PARAMETER);
    for (i = 0; i < FE_NCONFIGS && (!configs || n < size); i++) {
        if (configs) configs[n] = (void *)(unsigned long)(i + 1);
        n++;
    }
    *num = n;
    return 1;
}

/* EGL's matching: a size asked for is a minimum, the surface type a mask; everything else here
 * matches every config (EGL_DONT_CARE = -1 too). */
EGLBoolean eglChooseConfig(void *dpy, const EGLint *attribs, void **configs, EGLint size, EGLint *num);
EGLBoolean eglChooseConfig(void *dpy, const EGLint *attribs, void **configs, EGLint size, EGLint *num)
{
    EGLint i, n = 0;
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!num) return egl_fail(EGL_BAD_PARAMETER);
    for (i = 0; i < FE_NCONFIGS; i++) {
        const EGLint *a = attribs;
        int ok = 1;
        while (a && a[0] != EGL_NONE && ok) {
            EGLint have = egl_config_attrib(i, a[0]);
            if (a[1] != -1) {
                if (a[0] == 0x3033) ok = (have & a[1]) == a[1];
                else if (a[0] == 0x3028) ok = have == a[1];
                else if (a[0] >= 0x3020 && a[0] <= 0x3026) ok = have >= a[1];
            }
            a += 2;
        }
        if (!ok) continue;
        if (configs && n < size) configs[n] = (void *)(unsigned long)(i + 1);
        if (!configs || n < size) n++;
    }
    *num = n;
    return 1;
}

EGLBoolean eglGetConfigAttrib(void *dpy, void *config, EGLint attr, EGLint *value);
EGLBoolean eglGetConfigAttrib(void *dpy, void *config, EGLint attr, EGLint *value)
{
    int c = egl_config(config);
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (c < 0) return egl_fail(EGL_BAD_CONFIG);
    if (value) *value = egl_config_attrib(c, attr);
    return 1;
}

/* A context: the core's GC, in its own sharegroup or its share context's. */
static GuestGC *fe_new_gc(void *share_sg, void **sg_out, unsigned api)
{
    void *sg = share_sg, *gc = 0;
    fe_hello();
    if (!sg && !GLESCreateSharegroup(&sg)) return 0;
    if (!GLESCreateGCWithAPI(sg, 0, 0, &gc, api)) {
        if (!share_sg) GLESDestroySharegroup(sg);
        return 0;
    }
    ((GuestGC *)gc)->api = api;
    ((GuestGC *)gc)->sg = sg;
    if (sg_out) *sg_out = sg;
    return gc;
}

void *eglCreateContext(void *dpy, void *config, void *share, const EGLint *attribs);
void *eglCreateContext(void *dpy, void *config, void *share, const EGLint *attribs)
{
    egl_ctx_t *c, *s = egl_ctx(share);
    (void)attribs;
    if (dpy != FE_DISPLAY) return egl_null(EGL_BAD_DISPLAY);
    if (egl_config(config) < 0) return egl_null(EGL_BAD_CONFIG);
    if (share && !s) return egl_null(EGL_BAD_CONTEXT);
    if (!(c = calloc(1, sizeof *c))) return egl_null(0x3003);   /* EGL_BAD_ALLOC */
    if (!(c->gc = fe_new_gc(s ? s->sg : 0, &c->sg, 1))) { free(c); return egl_null(0x3003); }
    c->magic = EGL_CTX_MAGIC;
    c->config = egl_config(config);
    if (s) c->sg = s->sg;
    return c;
}

EGLBoolean eglDestroyContext(void *dpy, void *ctx);
EGLBoolean eglDestroyContext(void *dpy, void *ctx)
{
    egl_ctx_t *c = egl_ctx(ctx);
    fe_cur_t *cur = fe_cur(0);
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!c) return egl_fail(EGL_BAD_CONTEXT);
    if (cur && cur->owner == c) fe_set_current(0, 0, 0, 0, 0);
    /* Surfaces may outlive the context. Release their cached names while the
     * GC is still live, then detach so later destruction/rebinding cannot use it. */
    while (c->surfaces) egl_release_names(c->surfaces);
    fe_destroy_gc(c->gc);
    c->magic = 0;
    free(c);   /* ponytail: the sharegroup outlives its contexts (a leak per destroyed group) */
    return 1;
}

static egl_surf_t *egl_new_surface(void *dpy, void *config, unsigned kind, void *native)
{
    egl_surf_t *s;
    if (dpy != FE_DISPLAY) return egl_null(EGL_BAD_DISPLAY);
    if (egl_config(config) < 0) return egl_null(EGL_BAD_CONFIG);
    if (!native) return egl_null(kind == EGL_PIXMAP_BIT ? EGL_BAD_NATIVE_PIXMAP : 0x300B);
    if (!(s = calloc(1, sizeof *s))) return egl_null(0x3003);
    s->magic = EGL_SURF_MAGIC;
    s->kind = kind;
    s->config = egl_config(config);
    s->native = native;
    return s;
}

/* The pixmap is a CoreSurfaceBuffer (QuartzCore: a display buffer or a layer's offscreen surface). */
void *eglCreatePixmapSurface(void *dpy, void *config, void *pixmap, const EGLint *attribs);
void *eglCreatePixmapSurface(void *dpy, void *config, void *pixmap, const EGLint *attribs)
{
    egl_surf_t *s;
    (void)attribs;
    iosurface_init();
    if (!p_IOSurfaceGetWidth || !p_IOSurfaceGetHeight || !p_IOSurfaceGetPixelFormat)
        return egl_null(EGL_BAD_NATIVE_PIXMAP);
    if (!(s = egl_new_surface(dpy, config, EGL_PIXMAP_BIT, pixmap))) return 0;
    s->width = p_IOSurfaceGetWidth(pixmap);
    s->height = p_IOSurfaceGetHeight(pixmap);
    s->fourcc = p_IOSurfaceGetPixelFormat(pixmap);
    return s;
}

static int fe_bind_layer(GuestGC *gc, void *drawable);

/* A native window is the closure EAGL binds (fe_bind_layer): 2.x QuartzCore's -nativeWindow shape. */
void *eglCreateWindowSurface(void *dpy, void *config, void *win, const EGLint *attribs);
void *eglCreateWindowSurface(void *dpy, void *config, void *win, const EGLint *attribs)
{
    (void)attribs;
    return egl_new_surface(dpy, config, EGL_WINDOW_BIT, win);
}

void *eglCreatePbufferSurface(void *dpy, void *config, const EGLint *attribs);
void *eglCreatePbufferSurface(void *dpy, void *config, const EGLint *attribs)
{
    (void)dpy; (void)config; (void)attribs;
    refused("egl:", "pbuffer", ~0u);
    return egl_null(EGL_BAD_MATCH);
}

static void egl_release_names(egl_surf_t *s)
{
    /* The wire names the gc, so it need not be current, and 2.x CA destroys its surfaces with no
     * context current (measured). Left alive, the texture's host surface stayed dirty over the
     * buffer CA frees next, and the next flush wrote into the unmapped pages (SpringBoard SIGSEGV). */
    if (s->kind == EGL_PIXMAP_BIT && s->gc) {
        GL(s->gc, glDeleteFramebuffers, 1, (unsigned)(unsigned long)&s->fbo);
        GL(s->gc, glDeleteTextures, 1, (unsigned)(unsigned long)&s->tex);
    }
    if (s->kind == EGL_WINDOW_BIT && s->gc) fe_bind_layer(s->gc, 0);
    if (s->owner) {
        egl_surf_t **link = &s->owner->surfaces;
        while (*link && *link != s) link = &(*link)->owner_next;
        if (*link) *link = s->owner_next;
    }
    s->owner = 0;
    s->owner_next = 0;
    s->gc = 0;
    s->tex = s->fbo = 0;
}

static void egl_surface_owner(egl_ctx_t *owner, egl_surf_t *s)
{
    if (s->owner == owner) return;
    if (s->owner) egl_release_names(s);
    s->owner = owner;
    s->owner_next = owner->surfaces;
    owner->surfaces = s;
}

EGLBoolean eglDestroySurface(void *dpy, void *surface);
EGLBoolean eglDestroySurface(void *dpy, void *surface)
{
    egl_surf_t *s = egl_surf(surface);
    fe_cur_t *cur = fe_cur(0);
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!s) return egl_fail(EGL_BAD_SURFACE);
    egl_release_names(s);
    if (cur && (cur->draw == s || cur->read == s)) cur->draw = cur->read = 0;
    s->magic = 0;
    free(s);
    return 1;
}

/* Make gc render into s. A pixmap gets, once per context, a texture aliasing its buffer (the host
 * uploads it on bind and writes it back when a flush finds it rendered into) on a framebuffer of
 * its own; a window gets bound as EAGL binds a drawable, and draws to framebuffer 0. */
static int egl_attach(egl_ctx_t *owner, egl_surf_t *s)
{
    GuestGC *gc = owner->gc;
    if (s->gc && s->gc != gc) egl_release_names(s);
    egl_surface_owner(owner, s);
    if (s->kind == EGL_WINDOW_BIT) {
        if (!s->gc && !fe_bind_layer(gc, s->native)) {
            refused("egl:", "window", ~0u);
            return 0;
        }
        s->gc = gc;
        GL(gc, glBindFramebuffer, 0x8D40, 0);
        return 1;
    }
    if (!s->gc) {
        unsigned bound = 0;
        GL(gc, glGetIntegerv, 0x8069, (unsigned)(unsigned long)&bound);       /* TEXTURE_BINDING_2D */
        GL(gc, glGenTextures, 1, (unsigned)(unsigned long)&s->tex);
        GL(gc, glBindTexture, 0x0DE1, s->tex);
        GL(gc, glTexParameteri, 0x0DE1, 0x2801, 0x2601);                       /* MIN_FILTER LINEAR */
        GL(gc, glTexParameteri, 0x0DE1, 0x2800, 0x2601);
        if (!GLESBindCoreSurface(gc, 0x0DE1 | FE_WINDOW_ORDER, s->native)) {
            GL(gc, glBindTexture, 0x0DE1, bound);
            GL(gc, glDeleteTextures, 1, (unsigned)(unsigned long)&s->tex);
            s->tex = 0;
            refused("egl:", "pixmap", ~0u);
            return 0;
        }
        GL(gc, glGenFramebuffers, 1, (unsigned)(unsigned long)&s->fbo);
        GL(gc, glBindFramebuffer, 0x8D40, s->fbo);
        GL(gc, glFramebufferTexture2D, 0x8D40, 0x8CE0, 0x0DE1, s->tex, 0);
        GL(gc, glBindTexture, 0x0DE1, bound);
        s->gc = gc;
        {   /* the host log's evidence that CoreAnimation took the GL path (regress.py's 2.x gles leg) */
            static int said;
            if (!said++) {
                w("[gles] egl: first pixmap surface "); wd(s->width); w("x"); wd(s->height);
                w(": CoreAnimation renders through the host\n");
            }
            fe_ca_path("egl pixmap surface");
        }
    }
    GL(gc, glBindFramebuffer, 0x8D40, s->fbo);
    return 1;
}

EGLBoolean eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
EGLBoolean eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx)
{
    egl_ctx_t *c = egl_ctx(ctx);
    egl_surf_t *d = egl_surf(draw);
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    {
        /* A pixmap made not-current is a finished frame: 2.x CA brackets each frame with
         * eglMakeCurrent(buffer) ... eglMakeCurrent(none) and swaps the buffer right after, never
         * calling glFlush (the MBX driver's swap token waits for the GPU instead). So the host's
         * rendered copy goes to the buffer's memory here, before the swap; left to the next
         * frame, an animation's last frame never reached the panel (a Safari close zoom stuck
         * with its icons half way). */
        fe_cur_t *cur = fe_cur(0);
        egl_surf_t *was = cur && cur->is_egl ? egl_surf(cur->draw) : 0;
        if (was && was->kind == EGL_PIXMAP_BIT && was->gc && (was != d || !ctx))
            GL(was->gc, glFlush, 0);
    }
    if (!ctx) {
        fe_set_current(0, 0, 0, 0, 0);
        return 1;
    }
    if (!c) return egl_fail(EGL_BAD_CONTEXT);
    if ((draw && !d) || (read && !egl_surf(read))) return egl_fail(EGL_BAD_SURFACE);
    fe_set_current(c->gc, c, 1, d, read);
    if (d && !egl_attach(c, d)) return egl_fail(EGL_BAD_MATCH);
    return 1;
}

static int fe_present(GuestGC *gc);

EGLBoolean eglSwapBuffers(void *dpy, void *surface);
EGLBoolean eglSwapBuffers(void *dpy, void *surface)
{
    egl_surf_t *s = egl_surf(surface);
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!s || !s->gc) return egl_fail(EGL_BAD_SURFACE);
    if (s->kind == EGL_WINDOW_BIT) return fe_present(s->gc) ? 1 : egl_fail(EGL_BAD_SURFACE);
    GL(s->gc, glFlush, 0);                  /* a pixmap is written back at the flush */
    return 1;
}

EGLBoolean eglCopyBuffers(void *dpy, void *surface, void *pixmap);
EGLBoolean eglCopyBuffers(void *dpy, void *surface, void *pixmap)
{
    (void)dpy; (void)surface; (void)pixmap;
    refused("egl:", "copybuffers", ~0u);
    return egl_fail(EGL_BAD_MATCH);
}

EGLBoolean eglBindTexImage(void *dpy, void *surface, EGLint buffer);
EGLBoolean eglBindTexImage(void *dpy, void *surface, EGLint buffer)
{
    (void)dpy; (void)surface; (void)buffer;
    refused("egl:", "bindteximage", ~0u);
    return egl_fail(EGL_BAD_MATCH);
}

EGLBoolean eglReleaseTexImage(void *dpy, void *surface, EGLint buffer);
EGLBoolean eglReleaseTexImage(void *dpy, void *surface, EGLint buffer)
{
    (void)dpy; (void)surface; (void)buffer;
    return egl_fail(EGL_BAD_MATCH);
}

EGLBoolean eglQuerySurface(void *dpy, void *surface, EGLint attr, EGLint *value);
EGLBoolean eglQuerySurface(void *dpy, void *surface, EGLint attr, EGLint *value)
{
    egl_surf_t *s = egl_surf(surface);
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!s) return egl_fail(EGL_BAD_SURFACE);
    if (!value) return egl_fail(EGL_BAD_PARAMETER);
    switch (attr) {
    case 0x3057: *value = (EGLint)s->width; return 1;       /* EGL_WIDTH */
    case 0x3056: *value = (EGLint)s->height; return 1;      /* EGL_HEIGHT */
    case 0x3028: *value = s->config + 1; return 1;          /* EGL_CONFIG_ID */
    default: return egl_fail(EGL_BAD_ATTRIBUTE);
    }
}

EGLBoolean eglSurfaceAttrib(void *dpy, void *surface, EGLint attr, EGLint value);
EGLBoolean eglSurfaceAttrib(void *dpy, void *surface, EGLint attr, EGLint value)
{
    (void)attr; (void)value;
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    return egl_surf(surface) ? 1 : egl_fail(EGL_BAD_SURFACE);
}

EGLBoolean eglQueryContext(void *dpy, void *ctx, EGLint attr, EGLint *value);
EGLBoolean eglQueryContext(void *dpy, void *ctx, EGLint attr, EGLint *value)
{
    egl_ctx_t *c = egl_ctx(ctx);
    if (dpy != FE_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!c) return egl_fail(EGL_BAD_CONTEXT);
    if (attr != 0x3028 || !value) return egl_fail(EGL_BAD_ATTRIBUTE);
    *value = c->config + 1;
    return 1;
}

void *eglGetCurrentContext(void);
void *eglGetCurrentContext(void)
{
    fe_cur_t *c = fe_cur(0);
    return c && c->is_egl ? c->owner : 0;
}

void *eglGetCurrentSurface(EGLint which);
void *eglGetCurrentSurface(EGLint which)
{
    fe_cur_t *c = fe_cur(0);
    if (!c || !c->is_egl) return 0;
    return which == 0x305A ? c->read : c->draw;             /* EGL_READ, else EGL_DRAW */
}

void *eglGetCurrentDisplay(void);
void *eglGetCurrentDisplay(void) { return eglGetCurrentContext() ? FE_DISPLAY : 0; }

EGLBoolean eglWaitGL(void);
EGLBoolean eglWaitGL(void) { GuestGC *gc = fe_gc(); if (gc) GL(gc, glFinish, 0); return 1; }
EGLBoolean eglWaitNative(EGLint engine);
EGLBoolean eglWaitNative(EGLint engine) { (void)engine; return 1; }
EGLBoolean eglSwapInterval(void *dpy, EGLint interval);
EGLBoolean eglSwapInterval(void *dpy, EGLint interval) { (void)interval; return dpy == FE_DISPLAY ? 1 : egl_fail(EGL_BAD_DISPLAY); }
void *eglGetProcAddress(const char *name);
void *eglGetProcAddress(const char *name) { return name ? dlsym(RTLD_DEFAULT, name) : 0; }

/* ----------------------------------------------------- the layer, surfaces, the swap --- */

/*
 * A CAEAGLLayer's -nativeWindow is a closure {version, attach, detach, begin, swap, collect[, properties]}
 * (_EAGLNativeWindowObject; `properties` from 4.2.1, when version > 1). The engine hands attach a callbacks block
 * {callback_data, create_buffer, destroy_buffer} (3.x's EAGLNativeWindowCallbacksRec), plus 4.x's fourth word, a
 * preflight(ctx, a, b, c) the stock engine answers with gliGetInteger(0x25B, {a, b, c})[0]; here a stays as it is.
 * The 3-word readers never see the fourth. Then the first begin (nextBuffer) gives the surface to render into, whose
 * size is the renderbuffer's (transposed when properties says so, bit 2). mbxshim.c's CoreAnimation-drawable notes
 * have the contract as read out of the stock engines.
 */
static unsigned fe_preflight(void *ctx, unsigned a, unsigned b, unsigned c)
{
    (void)ctx; (void)b; (void)c;
    return a;
}

/* NULL releases the layer (unbinds it: CA refuses a second bind while the layer still belongs to the first). */
static int fe_bind_layer(GuestGC *gc, void *drawable)
{
    void **vt = drawable;
    ca_view_t *v = ca_view_for_gc(gc, drawable != 0);
    unsigned flags = 0, wd_, ht;
    /* The layer this view held, and its buffers' size: a rebind of the same layer may have to start from them. */
    void *was = v ? v->drawable : 0;
    unsigned was_w = v ? v->width : 0, was_h = v ? v->height : 0;
    if (v) ca_detach_view(v);
    if (!drawable) return 1;
    if (!v) {
        refused("view:", "no-slot", ~0u);
        return 0;
    }
    iosurface_init();
    v->gc = gc;
    v->block[0] = v->block;             /* create_buffer's arg0: finds this view */
    v->block[1] = (void *)ca_create_buffer;
    v->block[2] = (void *)ca_destroy_buffer;
    v->block[3] = (void *)fe_preflight;
    v->block[4] = v->block[6] = v->block[7] = 0;
    v->block[5] = gc;
    /* Always a 32-bit surface: the host renders 32-bit, and a 565 layer never reached 3.1.3's panel (GLESBindView). */
    if (!((ca_bind_fn)vt[1])(drawable, CA_FOURCC_BGRA, v->block)) {
        ca_view_t empty = {0};
        *v = empty;
        refused("ca:", "bind", ~0u);
        return 0;
    }
    v->drawable = drawable;
    if (*(int *)drawable > 1 && vt[6]) flags = ((unsigned (*)(void *))vt[6])(drawable) & ~8u;
    v->need_buffer = 1;
    /* A layer bound again keeps the buffers it has, and while the render server still holds both of them (the one
     * on screen, and the one the detach just presented) nextBuffer has none to give. They come free when this
     * transaction commits, which waits for the bind to return: waiting here only blocks in CA (about 1 s a try)
     * until the launch watchdog fires. So a rebind of the same layer takes the size its buffers have and asks for
     * one at the next frame (fe_present). Contre Jour 1.01 binds its layer again on its second turn (4.2.1), and the
     * failed bind left every frame after it nowhere: black for good. */
    if (!ca_next_buffer(v)) {
        if (drawable != was || !was_w || !was_h) {
            ca_detach_view(v);
            refused("ca:", "first-buffer", ~0u);
            return 0;
        }
        v->width = was_w;
        v->height = was_h;
    }
    wd_ = v->width; ht = v->height;
    if (flags & 4) { unsigned t = wd_; wd_ = ht; ht = t; }
    if (qc(GLES_OP_DRAWABLE_STORAGE, gc, 2, A(wd_, ht)) < 0) {
        ca_detach_view(v);
        return 0;
    }
    return 1;
}

/* The frame goes into this GC's layer surface (or, for the CA compositor, the surface it attached as the
 * renderbuffer). The host writes it with a debug write, which cannot fault pages in, and a buffer CA has just
 * allocated may have none mapped: take the frame's buffer now and touch every page of it. */
static int fe_present(GuestGC *gc)
{
    ca_view_t *v = ca_view_for_gc(gc, 0);
    /* A rebound layer whose buffers are still out (fe_bind_layer) drops this frame rather than blit it to the panel. */
    if (v && v->drawable && !v->ref && !ca_next_buffer(v)) return 1;
    if (v && ca_next_buffer(v) && v->base) {
        volatile unsigned char *p = (volatile unsigned char *)(unsigned long)v->base;
        unsigned i, n = v->stride * v->height;
        for (i = 0; i < n; i += 4096) p[i] = p[i];
        if (n) p[n - 1] = p[n - 1];
    }
    return GLESPresentView(gc, 0) != 0;
}

static void *(*p_CFRetain)(const void *);
static void (*p_CFRelease)(const void *);

static void fe_cf(void)
{
    void *cf;
    if (p_CFRelease) return;
    cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (cf) { p_CFRetain = dlsym(cf, "CFRetain"); p_CFRelease = dlsym(cf, "CFRelease"); }
}

/* The surface each view holds as its renderbuffer (attachImage: with GL_RENDERBUFFER): retained, as the stock
 * engine holds the surface it looks up by ID, so a caller's release cannot leave the view presenting into it. */
static void *fe_owned[CA_MAX_VIEWS];

static void fe_own(ca_view_t *v, void *surf)
{
    void **slot = &fe_owned[v - ca_views];
    fe_cf();
    if (surf && p_CFRetain) p_CFRetain(surf);
    if (*slot && p_CFRelease) p_CFRelease(*slot);
    *slot = surf;
}

/* An IOSurface as the color renderbuffer: this GC's view surface, the host drawable sized to it. */
static int fe_attach_renderbuffer(GuestGC *gc, void *surf)
{
    ca_view_t *v = ca_view_for_gc(gc, 1);
    if (!surf) {
        if (v) { fe_own(v, 0); v->ref = 0; v->base = 0; }
        return 1;
    }
    if (!v || !surface_capture(v, surf)) return 0;
    fe_own(v, surf);
    v->need_buffer = 0;
    return qc(GLES_OP_DRAWABLE_STORAGE, gc, 2, A(v->width, v->height)) >= 0;
}

/* 5.x makes some layer IOSurfaces with no pixel format (IOSurfaceGetPixelFormat 0: the home screen's icon labels)
 * and describes them only by the GL format and type it binds them with; the host takes a fourcc. */
static unsigned fe_gl_fourcc(unsigned fmt, unsigned type)
{
    int ub = type == 0x1401;                          /* GL_UNSIGNED_BYTE */
    if (fmt == 0x80E1 && (ub || type == 0x8367)) return 0x42475241;   /* GL_BGRA: 'BGRA' */
    if (fmt == 0x1908 && ub) return 0x52474241;                       /* GL_RGBA: 'RGBA' */
    if (fmt == 0x1906 && ub) return 0x41303038;                       /* GL_ALPHA: 'A008' */
    if (fmt == 0x1909 && ub) return 0x4c303038;                       /* GL_LUMINANCE: 'L008' */
    if (fmt == 0x190A && ub) return 0x32433038;                       /* GL_LUMINANCE_ALPHA: '2C08' */
    if (fmt == 0x1907 && type == 0x8363) return 0x4c353635;           /* GL_RGB 5_6_5: 'L565' */
    return 0;
}

/* The framebuffer's swap, signaled once this GC's frame is in its surface: what stock EAGL does for a context the
 * GPU does not complete swaps for (8C148 -[EAGLContext swapNotification:...] 0x3555737c,
 * IOMobileFramebufferSwapSignal on the framebuffer it is given). */
static void *(*p_fbGetMain)(void **);
static int (*p_fbGetID)(void *, unsigned *);
static int (*p_fbSignal)(void *, unsigned, unsigned);

static int fe_iomfb(void)
{
    static void *h;
    if (!h && (h = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_NOW))) {
        p_fbGetMain = dlsym(h, "IOMobileFramebufferGetMainDisplay");
        p_fbGetID = dlsym(h, "IOMobileFramebufferGetID");
        p_fbSignal = dlsym(h, "IOMobileFramebufferSwapSignal");
    }
    return p_fbSignal != 0;
}

static int fe_swap_signal(GuestGC *gc, void *fb, unsigned txn, unsigned layer)
{
    qc(GLES_ID_glFinish, gc, 0, A(0));          /* the frame is in the surface first */
    if (!fb || !fe_iomfb()) {
        refused("eagl:", "swap-signal", ~0u);
        return 0;
    }
    return p_fbSignal(fb, txn, layer) == 0;
}

/* ------------------------------------------- the scaler's GPU-conditioned transfers --- */

/*
 * 4.x CoreAnimation scales a layer through the M2 scaler (IOSurfaceAccelerator) after drawing its source with GL: on
 * the iPod 4's 2x panel, every 1x app's CAEAGLLayer (8C148 QuartzCore). With an ES2 render server
 * (EAGLServer::supports_iosurface_accelerator_tokens) it queues IOSurfaceAcceleratorConditionalTransferSurfaceWithSwap,
 * which returns a token and leaves the transfer waiting in AppleM2ScalerCSCDriver's copy_surface for a condition, then
 * hands the token to GL: CAEAGLContextScalarNotification -> [ctx sendNotification:IOSurfaceAcceleratorGetID(accel)
 * forTransaction:token onLayer:0]. The stock engine forwards that to the SGX, whose kernel driver releases the transfer
 * once the frame is drawn. No SGX runs here and no user client releases a condition (the scaler's has none), so the
 * transfer never ran and the layer stayed black. The GL here is done when the notification arrives, so the front end
 * takes the condition's place: the conditional call is recorded instead of queued, and the notification issues it as
 * the unconditional IOSurfaceAcceleratorTransferSurfaceWithSwap, which takes the same ten arguments (the conditional one
 * adds only the token out); likewise the plain ConditionalTransferSurface (seven) that 5.x also imports. Bound by name in each image that imports it (its lazy/non-lazy symbol pointers), so nothing
 * is per build; processes that do not import it are untouched.
 * 7.x's QuartzCore records the conditional transfers but never sends the notification to this context (11D257: every
 * token dropped, the 1x app's CAEAGLLayer black). The host finishes every call before the next, so the condition
 * only has to mean "the frame is drawn": once tokens drop with no release ever seen, each recorded transfer finishes
 * the recording thread's GL and is released at once.
 */
static unsigned fe_u32(const unsigned char *p);
#define FE_XFERS 8
typedef int (*fe_xfer_fn)(void *, void *, void *, void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
static struct fe_xfer { void *acc, *src, *dst, *props; unsigned arg[6], token, swap; } fe_xfers[FE_XFERS];
static unsigned fe_xfer_next;
static int fe_xfer_notified, fe_xfer_direct;     /* a notification ever released one; none comes: release at once */
static void fe_xfer_kick(void);
static fe_xfer_fn p_xfer, p_xferSwap;
static int (*p_accGetID)(void *, unsigned *);
static pthread_mutex_t fe_xfer_lock = PTHREAD_MUTEX_INITIALIZER;

static int fe_record_xfer(void *acc, void *src, void *dst, void *props, const unsigned *arg, unsigned swap,
                          unsigned *token)
{
    struct fe_xfer *x, old;
    unsigned i;
    fe_cf();
    if (!p_CFRetain || !src || !dst) return 0xE00002BC;            /* kIOReturnError */
    if (fe_xfer_direct && fe_gc()) qc(GLES_ID_glFinish, fe_gc(), 0, A(0));   /* the source is drawn, in guest memory */
    pthread_mutex_lock(&fe_xfer_lock);
    if (!++fe_xfer_next) fe_xfer_next = 1;                          /* 0 is no token */
    x = &fe_xfers[fe_xfer_next % FE_XFERS];
    old = *x;
    x->acc = acc; x->src = p_CFRetain(src); x->dst = p_CFRetain(dst); x->props = props ? p_CFRetain(props) : 0;
    for (i = 0; i < 6; i++) x->arg[i] = i < (swap ? 6u : 3u) ? arg[i] : 0;
    x->swap = swap;
    x->token = fe_xfer_next;
    if (token) *token = fe_xfer_next;
    if (fe_xfer_direct) {
        x->swap |= 2;
        fe_xfer_kick();
    }
    pthread_mutex_unlock(&fe_xfer_lock);
    if (old.token) {                     /* never notified: dropped, as a transfer whose condition never comes */
        if (!fe_xfer_notified && !fe_xfer_direct) {
            fe_xfer_direct = 1;
            w("[gles] scaler: no condition release arrives; conditional transfers run when recorded\n");
        }
        refused("scaler:", "token-dropped", ~0u);
        p_CFRelease(old.src); p_CFRelease(old.dst); if (old.props) p_CFRelease(old.props);
    }
    return 0;
}

/* IOSurfaceAcceleratorConditionalTransferSurfaceWithSwap: TransferSurfaceWithSwap's ten arguments, then the token. */
static int fe_cond_xfer_swap(void *acc, void *src, void *dst, void *props, unsigned a5, unsigned a6, unsigned a7,
                             unsigned a8, unsigned a9, unsigned a10, unsigned *token)
{
    unsigned arg[6] = { a5, a6, a7, a8, a9, a10 };
    return fe_record_xfer(acc, src, dst, props, arg, 1, token);
}

/* IOSurfaceAcceleratorConditionalTransferSurface (5.x imports it too): TransferSurface's seven, then the token. */
static int fe_cond_xfer(void *acc, void *src, void *dst, void *props, unsigned a5, unsigned a6, unsigned a7,
                        unsigned *token)
{
    unsigned arg[3] = { a5, a6, a7 };
    return fe_record_xfer(acc, src, dst, props, arg, 0, token);
}

/* Issues released transfers in token order, off the notifying thread: the stock condition is released in the kernel
 * while the render server goes on to end its swap, and 5.x's TransferSurfaceWithSwap waits for that swap, so issued
 * from the notification itself it never returned (9A334: SpringBoard hung at the app's first GL frame). */
static pthread_cond_t fe_xfer_ready = PTHREAD_COND_INITIALIZER;

static void *fe_xfer_worker(void *unused)
{
    (void)unused;
    for (;;) {
        struct fe_xfer x = {0};
        unsigned i;
        pthread_mutex_lock(&fe_xfer_lock);
        for (;;) {
            for (i = 0; i < FE_XFERS; i++)
                if (fe_xfers[i].token && fe_xfers[i].swap & 2 && (!x.token || fe_xfers[i].token < x.token))
                    x = fe_xfers[i];
            if (x.token) break;
            pthread_cond_wait(&fe_xfer_ready, &fe_xfer_lock);
        }
        fe_xfers[x.token % FE_XFERS].token = 0;
        pthread_mutex_unlock(&fe_xfer_lock);
        if ((x.swap & 1 ? p_xferSwap : p_xfer)(x.acc, x.src, x.dst, x.props, x.arg[0], x.arg[1], x.arg[2], x.arg[3],
                                               x.arg[4], x.arg[5]) != 0)    /* the plain call reads only its seven */
            refused("scaler:", "transfer", ~0u);
        p_CFRelease(x.src); p_CFRelease(x.dst); if (x.props) p_CFRelease(x.props);
    }
    return 0;
}

/* Releases the recorded transfer the notification (accelerator ID, token) names; 0 if it names none. */
/* Wakes the worker for a released transfer (fe_xfer_lock held). */
static void fe_xfer_kick(void)
{
    static pthread_t worker;
    if (!worker && pthread_create(&worker, 0, fe_xfer_worker, 0)) worker = 0;
    pthread_cond_signal(&fe_xfer_ready);
}

static int fe_release_xfer(unsigned id, unsigned token)
{
    unsigned i, acc_id, found = 0;
    pthread_mutex_lock(&fe_xfer_lock);
    for (i = 0; i < FE_XFERS && token && p_accGetID; i++)
        if (fe_xfers[i].token == token && p_accGetID(fe_xfers[i].acc, &acc_id) == 0 && acc_id == id) {
            fe_xfers[i].swap |= 2;      /* released */
            found = fe_xfer_notified = 1;
            fe_xfer_kick();
            break;
        }
    pthread_mutex_unlock(&fe_xfer_lock);
    return found;
}

/* Point every loaded image's symbol pointers for the two conditional calls at their recorders (32-bit Mach-O, shared
 * cache or not). */
unsigned _dyld_image_count(void);
const void *_dyld_get_image_header(unsigned);
long _dyld_get_image_vmaddr_slide(unsigned);

__attribute__((constructor)) static void fe_bind_xfer(void)
{
    unsigned n = _dyld_image_count(), i;
    p_xferSwap = (fe_xfer_fn)dlsym(RTLD_DEFAULT, "IOSurfaceAcceleratorTransferSurfaceWithSwap");
    p_xfer = (fe_xfer_fn)dlsym(RTLD_DEFAULT, "IOSurfaceAcceleratorTransferSurface");
    p_accGetID = dlsym(RTLD_DEFAULT, "IOSurfaceAcceleratorGetID");
    if (!p_xferSwap || !p_xfer || !p_accGetID) return;
    for (i = 0; i < n; i++) {
        const unsigned char *h = _dyld_get_image_header(i), *lc;
        unsigned long slide = (unsigned long)_dyld_get_image_vmaddr_slide(i), le = 0, k, c;
        const unsigned *symtab = 0, *dysym = 0;
        if (!h || fe_u32(h) != 0xFEEDFACE) continue;
        for (lc = h + 28, k = 0; k < fe_u32(h + 16); k++, lc += fe_u32(lc + 4)) {
            if (fe_u32(lc) == 1 && !memcmp(lc + 8, "__LINKEDIT", 11))       /* vmaddr - fileoff */
                le = fe_u32(lc + 24) + slide - fe_u32(lc + 32);
            else if (fe_u32(lc) == 2) symtab = (const unsigned *)lc;
            else if (fe_u32(lc) == 11) dysym = (const unsigned *)lc;
        }
        if (!le || !symtab || !dysym) continue;
        const unsigned char *syms = (const unsigned char *)(le + symtab[2]), *strs = (const unsigned char *)(le + symtab[4]);
        const unsigned *ind = (const unsigned *)(le + dysym[14]);
        for (lc = h + 28, k = 0; k < fe_u32(h + 16); k++, lc += fe_u32(lc + 4)) {
            if (fe_u32(lc) != 1) continue;
            for (c = 0; c < fe_u32(lc + 48); c++) {
                const unsigned char *sec = lc + 56 + 68 * c;
                unsigned type = fe_u32(sec + 56) & 0xff, j;
                void **ptrs = (void **)(fe_u32(sec + 32) + slide);
                if (type != 6 && type != 7) continue;                   /* S_NON_LAZY / S_LAZY_SYMBOL_POINTERS */
                for (j = 0; j < fe_u32(sec + 36) / 4; j++) {
                    unsigned si = ind[fe_u32(sec + 60) + j];      /* reserved1: its first indirect entry */
                    if (si & 0xC0000000u) continue;                     /* INDIRECT_SYMBOL_LOCAL / ABS */
                    const char *name = (const char *)strs + fe_u32(syms + 12 * si);
                    if (!strcmp(name, "_IOSurfaceAcceleratorConditionalTransferSurfaceWithSwap"))
                        ptrs[j] = (void *)fe_cond_xfer_swap;
                    else if (!strcmp(name, "_IOSurfaceAcceleratorConditionalTransferSurface"))
                        ptrs[j] = (void *)fe_cond_xfer;
                }
            }
        }
    }
}

/* ------------------------------------------------------ the 5.x macro context --- */

/* The stock OpenGLES image as the shared cache maps it into every process (the file this one replaces is not
 * loaded, but its bytes are there): the {__GLIFunctionDispatchRec=...} @encode naming the dispatch fields in order
 * is a C string in its __TEXT (__cstring on 3.x, __objc_methtype on 4.x/5.x), and only OpenGLES carries it. The
 * cache is found by shared_region_check_np (syscall 294), its images by its own header (dyld_v1). */
static unsigned fe_u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static const unsigned *fe_stock_mh;      /* the stock OpenGLES's header in the cache, once fe_stock_encode found it */
static unsigned fe_stock_slide;

/* The stock OpenGLES's own export `_name` (name without the underscore), from its symbol table in the cache's
 * shared __LINKEDIT (symoff/stroff are cache file offsets): Thumb code gets its low bit. 6.x and 7.x carry the
 * dispatch @encode without field names, so their trampolines name the slots (gles_layout_from_exports). */
static void *fe_stock_symbol(const char *name)
{
    const unsigned char *lc;
    unsigned k, ncmds, symoff = 0, nsyms = 0, stroff = 0, le_vm = 0, le_off = 0;
    if (!fe_stock_mh) return 0;
    ncmds = fe_stock_mh[4];
    lc = (const unsigned char *)fe_stock_mh + 28;
    for (k = 0; k < ncmds; k++) {
        const unsigned *cmd = (const unsigned *)lc;
        if (cmd[0] == 2) { symoff = cmd[2]; nsyms = cmd[3]; stroff = cmd[4]; }           /* LC_SYMTAB */
        else if (cmd[0] == 1 && gles_streq((const char *)lc + 8, "__LINKEDIT")) { le_vm = cmd[6]; le_off = cmd[8]; }
        lc += cmd[1];
    }
    if (!nsyms || !le_vm) return 0;
    {
        const unsigned char *le = (const unsigned char *)(unsigned long)(le_vm + fe_stock_slide);
        const unsigned char *syms = le + (symoff - le_off);
        const char *strs = (const char *)le + (stroff - le_off);
        for (k = 0; k < nsyms; k++) {
            const unsigned char *nl = syms + 12 * k;
            const char *sym = strs + fe_u32(nl);
            unsigned value = fe_u32(nl + 8), desc = nl[6] | nl[7] << 8;
            if ((nl[4] & 0x0e) != 0x0e || !value || sym[0] != '_' || !gles_streq(sym + 1, name)) continue;   /* N_SECT */
            return (void *)(unsigned long)((value + fe_stock_slide) | ((desc & 0x0008) ? 1 : 0));   /* N_ARM_THUMB_DEF */
        }
    }
    return 0;
}

/* `path`'s Mach-O header as the shared cache maps it (setting fe_stock_slide), and its __TEXT (*base 0 if none). */
static const unsigned *fe_cache_image(const char *path, const char **base, unsigned long *size)
{
    unsigned long long start = 0;
    const unsigned char *c;
    unsigned moff, ioff, icount, i, slide;
    *base = 0;
    if (syscall(294, &start) != 0 || !start || start >> 32) return 0;
    c = (const unsigned char *)(unsigned long)start;
    if (c[0] != 'd' || c[1] != 'y' || c[2] != 'l' || c[3] != 'd' || c[4] != '_' || c[5] != 'v' || c[6] != '1') return 0;
    moff = fe_u32(c + 0x10); ioff = fe_u32(c + 0x18); icount = fe_u32(c + 0x1c);
    slide = (unsigned)start - fe_u32(c + moff);                /* mapping 0's address: the cache's unslid base */
    for (i = 0; i < icount; i++) {
        const unsigned char *img = c + ioff + 32 * i;
        const unsigned *mh;
        const unsigned char *lc;
        unsigned k, ncmds;
        if (!gles_streq((const char *)c + fe_u32(img + 24), path)) continue;
        mh = (const unsigned *)(unsigned long)(fe_u32(img) + slide);
        if (mh[0] != 0xfeedface) return 0;
        fe_stock_slide = slide;
        ncmds = mh[4];
        lc = (const unsigned char *)mh + 28;
        for (k = 0; k < ncmds; k++) {
            const unsigned *cmd = (const unsigned *)lc;
            if (cmd[0] == 1 && gles_streq((const char *)lc + 8, "__TEXT")) {
                *base = (const char *)(unsigned long)(cmd[6] + slide);
                *size = cmd[7];
            }
            lc += cmd[1];
        }
        return mh;
    }
    return 0;
}

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

static const char *fe_stock_encode(void)
{
    static const char tag[] = "{__GLIFunctionDispatchRec=";
    const char *base;
    unsigned long size = 0;
    fe_stock_mh = fe_cache_image("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", &base, &size);
    return fe_find(base, size, tag, sizeof tag - 1);
}

/* ------------------------------------------------------ what the GPU reports --- */

/* The GPU this firmware drives, as the column of the tables below: the MBX (ES 1.1 only) where there is no
 * GLEngine.bundle, else the SGX 535 (every SGX board here: iPhone 3GS, iPod touch 3G and 4G, iPhone 4, iPad), in an
 * ES 1.1 or an ES 2.0 context. */
enum { FE_MBX, FE_SGX1, FE_SGX2 };
static int fe_gpu(void *gc) { return !fe_es2() ? FE_MBX : gc && ((GuestGC *)gc)->api == 2 ? FE_SGX2 : FE_SGX1; }

/* The image at `path` in the shared cache, else the file itself mapped read-only (2.x and 3.x keep the GL engines
 * out of the cache): its bytes in *base (0 if neither), which stay mapped. */
extern int open(const char *, int, ...);
extern int close(int);
extern long long lseek(int, long long, int);
extern void *mmap(void *, unsigned long, int, int, int, long long);
static void fe_image(const char *path, const char **base, unsigned long *size)
{
    int fd;
    long long n;
    void *p;
    if (fe_cache_image(path, base, size) && *base) return;
    *base = 0; *size = 0;
    if ((fd = open(path, 0)) < 0) return;
    n = lseek(fd, 0, 2);
    p = n > 0 && n < (1 << 26) ? mmap(0, (unsigned long)n, 1, 2, fd, 0) : (void *)-1;   /* PROT_READ, MAP_PRIVATE */
    close(fd);
    if (p != (void *)-1) { *base = p; *size = (unsigned long)n; }
}

/* The engine whose strings say what the GPU reports: MBXGLEngine on the MBX, the SGX driver for its name and build,
 * GLEngine for the SGX's extension names and GLSL version. */
#define FE_MBX_ENGINE "/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine"
#define FE_SGX_ENGINE "/System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/GLEngine"
#define FE_SGX_DRIVER "/System/Library/Extensions/IMGSGX535GLDriver.bundle/IMGSGX535GLDriver"

/* The first C string in [text, text + size) that starts with `prefix` (at a string's start), or 0. */
static const char *fe_cstring(const char *text, unsigned long size, const char *prefix)
{
    unsigned long n = slen(prefix), off = 0;
    const char *p;
    while (off < size && (p = fe_find(text + off, size - off, prefix, n))) {
        if (p == text || p[-1] == 0) return p;
        off = (unsigned long)(p - text) + 1;
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

/* GL_EXTENSIONS: the extensions the hardware reports (MBXGLEngine's public list on the MBX; Apple's list for the
 * SGX 535 in "OpenGL ES Hardware Platform Guide for iOS", ES 1.1 and ES 2.0) that the bridge implements, each one only
 * where this firmware's engine names it, so a later extension (EXT_discard_framebuffer from 4.0, APPLE_sync from 6.0)
 * stays out of an earlier firmware. FE_X_OLD: on the MBX already in 2.2.1, whose engine is the OpenGLES this file
 * replaces, so with no engine to read only those are given.
 *
 * Left out though the hardware reports them, for want of a host implementation: APPLE_copy_texture_levels,
 * APPLE_framebuffer_multisample, APPLE_rgb_422, EXT_debug_label, EXT_debug_marker, EXT_map_buffer_range,
 * EXT_separate_shader_objects, EXT_shader_framebuffer_fetch, EXT_shader_texture_lod, EXT_texture_storage,
 * OES_element_index_uint, OES_mapbuffer, OES_matrix_palette, OES_stencil8 (a stencil-only renderbuffer the host's
 * framebuffers don't take) and OES_vertex_array_object. APPLE_texture_rectangle and APPLE_core_surface_texture are
 * the MBX engine's private pair, which 2.x CoreAnimation needs before it composites with GL. */
#define FE_X_MBX  1
#define FE_X_SGX1 2
#define FE_X_SGX2 4
#define FE_X_OLD  8
static const struct { const char *name; unsigned char on; } fe_ext_table[] = {
    { "GL_APPLE_core_surface_texture",     FE_X_MBX | FE_X_OLD },
    { "GL_APPLE_sync",                     FE_X_SGX2 },
    { "GL_APPLE_texture_2D_limited_npot",  FE_X_SGX1 },
    { "GL_APPLE_texture_format_BGRA8888",  FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 },
    { "GL_APPLE_texture_max_level",        FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 },
    { "GL_APPLE_texture_rectangle",        FE_X_MBX | FE_X_OLD },
    { "GL_EXT_blend_minmax",               FE_X_SGX1 | FE_X_SGX2 },
    { "GL_EXT_discard_framebuffer",        FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 },
    { "GL_EXT_read_format_bgra",           FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 },
    { "GL_EXT_texture_filter_anisotropic", FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 | FE_X_OLD },
    { "GL_EXT_texture_lod_bias",           FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
    { "GL_IMG_read_format",                FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 | FE_X_OLD },
    { "GL_IMG_texture_compression_pvrtc",  FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 | FE_X_OLD },
    { "GL_IMG_texture_format_BGRA8888",    FE_X_MBX | FE_X_OLD },
    { "GL_OES_blend_equation_separate",    FE_X_SGX1 },
    { "GL_OES_blend_func_separate",        FE_X_SGX1 },
    { "GL_OES_blend_subtract",             FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
    { "GL_OES_compressed_paletted_texture", FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
    { "GL_OES_depth24",                    FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 | FE_X_OLD },
    { "GL_OES_depth_texture",              FE_X_SGX2 },
    { "GL_OES_draw_texture",               FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
    { "GL_OES_fbo_render_mipmap",          FE_X_SGX1 | FE_X_SGX2 },
    { "GL_OES_framebuffer_object",         FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
    { "GL_OES_packed_depth_stencil",       FE_X_SGX1 | FE_X_SGX2 },
    { "GL_OES_point_size_array",           FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
    { "GL_OES_point_sprite",               FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
    { "GL_OES_read_format",                FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
    { "GL_OES_rgb8_rgba8",                 FE_X_MBX | FE_X_SGX1 | FE_X_SGX2 | FE_X_OLD },
    { "GL_OES_standard_derivatives",       FE_X_SGX2 },
    { "GL_OES_stencil_wrap",               FE_X_SGX1 },
    { "GL_OES_texture_float",              FE_X_SGX2 },
    { "GL_OES_texture_half_float",         FE_X_SGX2 },
    { "GL_OES_texture_mirrored_repeat",    FE_X_MBX | FE_X_SGX1 | FE_X_OLD },
};

/* The list for `gpu` into `out` (`cap` bytes), given its engine's __TEXT (`text`, 0 if none was found). */
static void fe_extensions(char *out, unsigned cap, int gpu, const char *text, unsigned long size)
{
    unsigned i, k, n = 0, col = 1u << gpu;
    for (i = 0; i < sizeof fe_ext_table / sizeof fe_ext_table[0]; i++) {
        const char *e = fe_ext_table[i].name;
        if (!(fe_ext_table[i].on & col)) continue;
        if (text ? !fe_names(text, size, e) : !(fe_ext_table[i].on & FE_X_OLD)) continue;
        if (n + slen(e) + 1 >= cap) break;
        if (n) out[n++] = ' ';
        for (k = 0; e[k]; k++) out[n++] = e[k];
    }
    out[n] = 0;
}

/* GL_VERSION: the SGX driver's own ("OpenGL ES 2.0 IMGSGX535-63.24"; 7.x keeps only the build, "IMGSGX535-97.7",
 * and GLEngine puts it together the same way), MBXGLEngine's ("OpenGL ES-CM 1.1 (48)"), or the bare version. */
static const char *fe_version(char *out, unsigned cap, int gpu, const char *text, unsigned long size)
{
    const char *v = gpu == FE_SGX2 ? "OpenGL ES 2.0" : "OpenGL ES-CM 1.1", *b;
    unsigned n = 0, k;
    if (gpu == FE_MBX) return (b = fe_cstring(text, size, v)) ? b : v;
    if (!(b = fe_find(text, size, "IMGSGX535-", 10))) return v;
    for (k = 0; v[k] && n + 1 < cap; k++) out[n++] = v[k];
    if (n + 1 < cap) out[n++] = ' ';
    for (k = 0; b + k < text + size && b[k] && n + 1 < cap; k++) out[n++] = b[k];
    out[n] = 0;
    return out;
}

/* glGetString, answered here (the caller reads the bytes, and this image and the engines are guest memory) as the
 * device's GPU answers, each string read once per column. */
static const char *fe_getString(void *gc, unsigned name)
{
    static char ext[3][1024], ver[3][64];
    static const char *renderer[3], *version[3], *glsl;
    int gpu = fe_gpu(gc);
    if (!renderer[gpu]) {
        const char *text, *drv;
        unsigned long size, dsize;
        fe_image(gpu == FE_MBX ? FE_MBX_ENGINE : FE_SGX_ENGINE, &text, &size);
        fe_extensions(ext[gpu], sizeof ext[gpu], gpu, text, size);
        if (gpu != FE_MBX) {
            if (!glsl && !(glsl = fe_cstring(text, size, "OpenGL ES GLSL ES 1.0"))) glsl = "OpenGL ES GLSL ES 1.00";
            fe_image(FE_SGX_DRIVER, &drv, &dsize);
            text = drv; size = dsize;
        }
        version[gpu] = fe_version(ver[gpu], sizeof ver[gpu], gpu, text, size);
        if (!(renderer[gpu] = fe_cstring(text, size, "PowerVR ")))
            renderer[gpu] = gpu == FE_MBX ? "PowerVR MBXLite with VGPLite" : "PowerVR SGX 535";
    }
    switch (name) {
    case 0x1F00: return "Imagination Technologies";
    case 0x1F01: return renderer[gpu];
    case 0x1F02: return version[gpu];
    case 0x1F03: return ext[gpu];
    case 0x8B8C: if (gpu == FE_SGX2) return glsl;              /* SHADING_LANGUAGE_VERSION: ES 2.0 only */
                 /* fall through */
    default:     return "";
    }
}

/* glGet's implementation limits as the GPU reports them, where the host's (the Mac's: 16384-texel textures, 16
 * units) are larger: an app sizes its atlases and picks its paths from these, and CoreAnimation tiles any layer
 * larger than MAX_TEXTURE_SIZE, as on the device. MBX: MBXGLEngine's get (3.1.3, run on each name). SGX 535: Apple's
 * "OpenGL ES Hardware Platform Guide for iOS" tables 1-2 and 1-3, which agree with the driver's configuration
 * (IMGSGX535GLDriver glrSetConfigData, 3.2: 2048, 511.0, 16.0, 4.0). -1: not this GPU's or not clamped (the SGX's
 * anisotropy and smooth widths, which neither source gives). A pair is a range (low, high) except VIEWPORT_DIMS. */
static const struct { unsigned short pname, n; short v[3][2]; } fe_limit_table[] = {
    /*                                  MBX          SGX ES 1.1   SGX ES 2.0 */
    { 0x0D33, 1, { { 1024 },     { 2048 },     { 2048 } } },        /* MAX_TEXTURE_SIZE */
    { 0x84E8, 1, { { 1024 },     { 2048 },     { 2048 } } },        /* MAX_RENDERBUFFER_SIZE */
    { 0x851C, 1, { { -1 },       { 2048 },     { 2048 } } },        /* MAX_CUBE_MAP_TEXTURE_SIZE */
    { 0x0D3A, 2, { { 1024, 1024 }, { 2048, 2048 }, { 2048, 2048 } } }, /* MAX_VIEWPORT_DIMS */
    { 0x846D, 2, { { 1, 64 },    { 1, 511 },   { 1, 511 } } },      /* ALIASED_POINT_SIZE_RANGE */
    { 0x846E, 2, { { 1, 64 },    { 1, 16 },    { 1, 16 } } },       /* ALIASED_LINE_WIDTH_RANGE */
    { 0x0B12, 2, { { 1, 64 },    { 1, 511 },   { -1 } } },          /* SMOOTH_POINT_SIZE_RANGE */
    { 0x0B22, 2, { { 1, 1 },     { -1 },       { -1 } } },          /* SMOOTH_LINE_WIDTH_RANGE */
    { 0x84E2, 1, { { 2 },        { 8 },        { -1 } } },          /* MAX_TEXTURE_UNITS */
    { 0x0D32, 1, { { 1 },        { 6 },        { -1 } } },          /* MAX_CLIP_PLANES */
    { 0x0D31, 1, { { 8 },        { 8 },        { -1 } } },          /* MAX_LIGHTS */
    { 0x0D36, 1, { { 16 },       { 16 },       { -1 } } },          /* MAX_MODELVIEW_STACK_DEPTH */
    { 0x0D38, 1, { { 2 },        { 2 },        { -1 } } },          /* MAX_PROJECTION_STACK_DEPTH */
    { 0x0D39, 1, { { 4 },        { 4 },        { -1 } } },          /* MAX_TEXTURE_STACK_DEPTH */
    { 0x84FD, 1, { { 2 },        { 4 },        { -1 } } },          /* MAX_TEXTURE_LOD_BIAS_EXT */
    { 0x84FF, 1, { { 2 },        { -1 },       { -1 } } },          /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */
    { 0x8869, 1, { { -1 },       { -1 },       { 16 } } },          /* MAX_VERTEX_ATTRIBS */
    { 0x8DFB, 1, { { -1 },       { -1 },       { 128 } } },         /* MAX_VERTEX_UNIFORM_VECTORS */
    { 0x8DFC, 1, { { -1 },       { -1 },       { 8 } } },           /* MAX_VARYING_VECTORS */
    { 0x8DFD, 1, { { -1 },       { -1 },       { 64 } } },          /* MAX_FRAGMENT_UNIFORM_VECTORS */
    { 0x8872, 1, { { -1 },       { -1 },       { 8 } } },           /* MAX_TEXTURE_IMAGE_UNITS */
    { 0x8B4C, 1, { { -1 },       { -1 },       { 0 } } },           /* MAX_VERTEX_TEXTURE_IMAGE_UNITS */
    { 0x8B4D, 1, { { -1 },       { -1 },       { 8 } } },           /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
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

/* {GC, table}: 5.x QuartzCore and CoreImage load the engine's context from word 0 and call field k through word
 * 1 + k with it as the first argument (9B206 QuartzCore 0x32662ad2..0x32662ae8; docs/ipad1/gles-public-seam.md). */
static void **fe_macro(GuestGC *gc, void ***cache)
{
    static int layout = -1;
    unsigned n;
    void **m;
    if (*cache) return *cache;
    if (layout < 0) {
        gles_encode_override = fe_stock_encode();
        gles_export_lookup = fe_stock_symbol;
        layout = gles_encode_override != 0;
        if (!layout) w("[gles] no __GLIFunctionDispatchRec @encode in the shared cache's OpenGLES: no macro context\n");
    }
    if (!layout) {
        refused("eagl:", "macro-context", ~0u);
        return 0;
    }
    n = gles_discover(0);
    if (!n || !(m = calloc(1 + n, sizeof *m))) return 0;
    m[0] = gc;
    gles_fill(m + 1, n, fe_hand);
    *cache = m;
    fe_ca_path("macro context");
    return m;
}

/* ------------------------------------------------------------------------- EAGL --- */

void EAGLGetVersion(unsigned *major, unsigned *minor);
void EAGLGetVersion(unsigned *major, unsigned *minor)          /* 1.0 on every build (8C148 0x355576c0) */
{
    if (major) *major = 1;
    if (minor) *minor = 0;
}

/* 2.x: the stock one is a breakpoint target its SetError calls; nothing calls ours. */
void opengl_error_break(void);
void opengl_error_break(void) {}

/* 4.x+: QuartzCore's memory-warning hook. The stock one asks the SGX kext to recycle memory (level > 1: a CF
 * property on its IOService) and says whether it did; there is no such kext here, and nothing to recycle. */
int EAGLMemoryNotificationRecycling(int level);
int EAGLMemoryNotificationRecycling(int level) { (void)level; return 0; }

/* 3.x-5.x exports no image of any firmware read imports (gles-public-seam.md): counted refusals. */
#define FE_UNUSED(name) int name(void); int name(void) { refused("eagl:", #name, ~0u); return 0; }
FE_UNUSED(GLCBackDispatch)
FE_UNUSED(GLCFrontDispatch)
FE_UNUSED(GLCGetProfilerStorage)
FE_UNUSED(GLCRestoreDispatch)
FE_UNUSED(GLCRestoreDispatchFunction)
FE_UNUSED(GLCSelectDispatchBounded)
FE_UNUSED(GLCSelectDispatchFunction)
FE_UNUSED(GLCSetProfilerStorage)
FE_UNUSED(GLCSetCompilationPerformanceCallback)   /* 7.x */

typedef signed char BOOL;
typedef unsigned NSUInteger;
@class NSString;

__attribute__((objc_root_class))
@interface NSObject { Class isa; }
+ (id)alloc;
- (id)init;
- (void)dealloc;
- (id)retain;
- (void)release;
- (BOOL)respondsToSelector:(SEL)sel;
- (BOOL)isEqual:(id)other;
- (BOOL)boolValue;
- (id)objectForKey:(id)key;
- (id)drawableProperties;
- (void *)nativeWindow;
@end

NSString *const kEAGLDrawablePropertyRetainedBacking = @"EAGLDrawablePropertyRetainedBacking";
NSString *const kEAGLDrawablePropertyColorFormat = @"EAGLDrawablePropertyColorFormat";
NSString *const kEAGLColorFormatRGB565 = @"EAGLColorFormat565";
NSString *const kEAGLColorFormatRGBA8 = @"EAGLColorFormatRGBA8";
NSString *const kEAGLContextPropertyAccelerated = @"EAGLContextPropertyAccelerated";
NSString *const kEAGLContextPropertySharegroup = @"EAGLContextPropertySharegroup";
NSString *const kEAGLContextPropertyClientRetainRelease = @"EAGLContextPropertyClientRetainRelease";
/* 7.x */
NSString *const kEAGLColorFormatSRGBA8 = @"EAGLColorFormatSRGBA8";
NSString *const kEAGLContextPropertySharedWithCompute = @"EAGLContextPropertySharedWithCompute";
NSString *const kEAGLContextPropertyVisibleInDebugTools = @"EAGLContextPropertyVisibleInDebugTools";

__attribute__((visibility("default")))
@interface EAGLSharegroup : NSObject { @public void *_private; NSUInteger _api; }
@end

@implementation EAGLSharegroup
- (id)init
{
    if (!(self = [super init])) return 0;
    fe_hello();
    if (!GLESCreateSharegroup(&_private)) { [self release]; return 0; }
    return self;
}
/* 3.x's and 4.x/5.x's internal initializers (QuartzCore makes its groups with -init). */
- (id)initWithAPI:(NSUInteger)api { if ((self = [self init])) _api = api; return self; }
- (id)initWithAPI:(NSUInteger)api require_acceleration:(BOOL)accel { (void)accel; return [self initWithAPI:api]; }
- (id)initWithAPI:(NSUInteger)api sharedWithCompute:(BOOL)compute { (void)compute; return [self initWithAPI:api]; }
/* 5.x GLKit's question; the stock one answers the first word of the group's private block.
 * ponytail: the API of the group's first context, taken to be that word; a mask if a caller turns out to need one */
- (NSUInteger)APIs { return _api; }
/* 5.x OpenCL's: libGFXShared state, which this front end never makes. */
- (void *)getGLIShared { refused("eagl:", "getGLIShared", ~0u); return 0; }
- (void)dealloc
{
    if (_private) GLESDestroySharegroup(_private);
    [super dealloc];
}
@end

struct eagl_private { GuestGC *gc; EAGLSharegroup *sharegroup; NSUInteger api; void **macro; };

__attribute__((visibility("default")))
@interface EAGLContext : NSObject { @public struct eagl_private *_private; }
- (id)initWithAPI:(NSUInteger)api sharegroup:(EAGLSharegroup *)sharegroup;
@end

@implementation EAGLContext
- (id)initWithAPI:(NSUInteger)api { return [self initWithAPI:api sharegroup:0]; }

/* API 1 everywhere, 2 where the firmware has the SGX engine; the stock init answers anything else with nil. */
- (id)initWithAPI:(NSUInteger)api sharegroup:(EAGLSharegroup *)sharegroup
{
    if (!(self = [super init])) return 0;
    if ((api != 1 && !(api == 2 && fe_es2())) || !(_private = calloc(1, sizeof *_private))) { [self release]; return 0; }
    _private->api = api;
    _private->sharegroup = sharegroup ? [sharegroup retain] : [[EAGLSharegroup alloc] initWithAPI:api];
    if (!_private->sharegroup ||
        !(_private->gc = fe_new_gc(_private->sharegroup->_private, 0, api))) {
        [self release];
        return 0;
    }
    if (!_private->sharegroup->_api) _private->sharegroup->_api = api;
    return self;
}

/* 3.1+: the compositor's (QuartzCore, CoreImage): {kEAGLContextPropertySharegroup, ...Accelerated,
 * ...ClientRetainRelease}. Every context here is the host's, so only the sharegroup matters. */
- (id)initWithAPI:(NSUInteger)api properties:(id)props
{
    id sg = props ? [props objectForKey:kEAGLContextPropertySharegroup] : 0;
    return [self initWithAPI:api sharegroup:sg];
}
- (id)initWithAPI:(NSUInteger)api sharedWithCompute:(BOOL)compute { (void)compute; return [self initWithAPI:api]; }

- (void)dealloc
{
    if (_private) {
        fe_cur_t *cur = fe_cur(0);
        if (cur && cur->owner == self) fe_set_current(0, 0, 0, 0, 0);
        if (_private->gc) {
            ca_view_t *v = ca_view_for_gc(_private->gc, 0);
            if (v) fe_own(v, 0);
            fe_destroy_gc(_private->gc);
        }
        free(_private->macro);
        [_private->sharegroup release];
        free(_private);
    }
    [super dealloc];
}

/* The stock one retains the new context and releases the old (0x3128dbd8). */
+ (BOOL)setCurrentContext:(EAGLContext *)context
{
    fe_cur_t *cur = fe_cur(0);
    EAGLContext *old = cur && !cur->is_egl ? cur->owner : 0;
    if (context == old) return 1;
    if (context) {
        [context retain];
        fe_set_current(context->_private->gc, context, 0, 0, 0);
    } else {
        fe_set_current(0, 0, 0, 0, 0);
    }
    [old release];
    return 1;
}

+ (EAGLContext *)currentContext
{
    fe_cur_t *cur = fe_cur(0);
    return cur && !cur->is_egl ? cur->owner : 0;
}

- (NSUInteger)API { return _private->api; }
- (EAGLSharegroup *)sharegroup { return _private->sharegroup; }

/* The drawable's -nativeWindow (and its color format, which the host does not need: see fe_bind_layer); a nil
 * drawable releases the storage. */
- (BOOL)renderbufferStorage:(NSUInteger)target fromDrawable:(id)drawable
{
    void *win = 0;
    (void)target;
    if (drawable && (![drawable respondsToSelector:@selector(nativeWindow)] || !(win = [drawable nativeWindow]))) {
        refused("eagl:", "drawable", ~0u);
        return 0;
    }
    return fe_bind_layer(_private->gc, win) != 0;
}

- (BOOL)presentRenderbuffer:(NSUInteger)target
{
    (void)target;
    return fe_present(_private->gc) != 0;
}

/* An IOSurface (2.x: a CoreSurfaceBuffer) as a texture's image or as the renderbuffer's storage; nil detaches. The
 * orientation flag is the stock engine's own business: the host binds surfaces in their memory order. */
- (BOOL)attachImage:(NSUInteger)target toCoreSurface:(void *)surface invertedRender:(BOOL)inverted
{
    (void)inverted;
    fe_ca_path("attachImage:toCoreSurface:invertedRender:");
    if (target == 0x8D41) return fe_attach_renderbuffer(_private->gc, surface);
    return GLESBindCoreSurface(_private->gc, target, surface) != 0;
}

/* 4.2.1+: the same for a texture, with the GL layout of a surface that carries no pixel format. */
- (BOOL)texImageIOSurface:(void *)surface target:(NSUInteger)target internalFormat:(NSUInteger)ifmt
                    width:(NSUInteger)width height:(NSUInteger)height format:(NSUInteger)format type:(NSUInteger)type
                    plane:(NSUInteger)plane invert:(BOOL)invert
{
    (void)ifmt; (void)width; (void)height; (void)plane; (void)invert;
    fe_ca_path("texImageIOSurface:");
    return GLESBindCoreSurfaceAs(_private->gc, target, surface, fe_gl_fourcc(format, type)) != 0;
}

- (void)swapNotification:(void *)fb forTransaction:(unsigned)transaction onLayer:(unsigned)layer
{
    fe_ca_path("swapNotification:");
    fe_swap_signal(_private->gc, fb, transaction, layer);
}

/* 3.2+: the same notification with the framebuffer's ID (IOMobileFramebufferGetID) instead of the framebuffer, or (4.x)
 * an IOSurfaceAccelerator's ID and a token fe_cond_xfer handed out. The one framebuffer a process can name is the main
 * display's; any other ID is a counted refusal. */
- (void)sendNotification:(unsigned)fbid forTransaction:(unsigned)transaction onLayer:(unsigned)layer
{
    void *fb = 0;
    unsigned id = 0;
    fe_ca_path("sendNotification:");
    qc(GLES_ID_glFinish, _private->gc, 0, A(0));       /* the frame is drawn: a scaler transfer may read it */
    if (fe_release_xfer(fbid, transaction)) return;
    if (fe_iomfb() && p_fbGetMain && p_fbGetID) {
        p_fbGetMain(&fb);
        if (fb && (p_fbGetID(fb, &id) != 0 || id != fbid)) fb = 0;
    }
    if (!fb) {
        qc(GLES_ID_glFinish, _private->gc, 0, A(0));
        refused("eagl:", "send-notification-id", ~0u);
        return;
    }
    fe_swap_signal(_private->gc, fb, transaction, layer);
}

/* The stock engines keep a few of these for their own rendering; none changes what the host draws. */
- (BOOL)setParameter:(unsigned)pname to:(const int *)value { (void)pname; (void)value; return 1; }
- (BOOL)getParameter:(unsigned)pname to:(int *)value { (void)pname; (void)value; return 0; }

/* 5.x: {GC, this firmware's dispatch table} (fe_macro); 6.x's QuartzCore asks with a lower-case g. */
- (void *)GetMacroContextPrivate { return fe_macro(_private->gc, &_private->macro); }
- (void *)getMacroContextPrivate { return fe_macro(_private->gc, &_private->macro); }
@end

void *EAGLGetCurrentMacroContextPrivate(void);
void *EAGLGetCurrentMacroContextPrivate(void)
{
    EAGLContext *c = [EAGLContext currentContext];
    return c ? [c GetMacroContextPrivate] : 0;
}

/* The engine's context of an EAGLContext (the stock one's is its private block's +0xc: here the GC). */
void *GLIContextFromEAGLContext(EAGLContext *c);
void *GLIContextFromEAGLContext(EAGLContext *c) { return c ? c->_private->gc : 0; }
