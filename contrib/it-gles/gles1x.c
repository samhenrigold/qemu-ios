/*
 * The iPhone OS 1.x OpenGLES front end over the shared guest transport core.
 * LayerKit renders through EGL/CoreSurface; 1.x has no EAGL and uses the old
 * Objective-C runtime. This file is plain C and exports opengles-1x.exports.
 * iOS 2.x–5.x use contrib/gles-public/opengles.c and its one EAGL implementation.
 *
 * The GL forwarders use gles-names.h wire ids and the current context's GC.
 * Nothing runs at load; the first context reports the transport protocol.
 * Build with build-gles1x.sh using the shared legacy armv6 linker.
 */
#include "mbxshim.c"

#include <pthread.h>

#define GLES_FN(n, f, id, argc, fl) GLES_ROW_##n,
enum {
#include "../../include/hw/arm/guest-services/gles-names.h"
};
#undef GLES_FN

/* ------------------------------------------------------------ the current context --- */

/* Per thread: the GC gl* calls go to, and whose it is (an EAGLContext or an egl context) so
 * +currentContext / eglGetCurrentContext answer with the object that was made current. */
typedef struct { GuestGC *gc; void *owner; int is_egl; void *draw, *read; } gles2x_cur_t;
static pthread_key_t gles2x_key;
static pthread_once_t gles2x_once = PTHREAD_ONCE_INIT;
static void *gles2x_hand[GLES_N_SLOTS];

static void gles2x_key_init(void) { pthread_key_create(&gles2x_key, free); }

static gles2x_cur_t *gles2x_cur(int create)
{
    gles2x_cur_t *c;
    pthread_once(&gles2x_once, gles2x_key_init);
    c = pthread_getspecific(gles2x_key);
    if (!c && create && (c = calloc(1, sizeof *c))) pthread_setspecific(gles2x_key, c);
    return c;
}

static void gles2x_set_current(GuestGC *gc, void *owner, int is_egl, void *draw, void *read)
{
    gles2x_cur_t *c = gles2x_cur(gc != 0);
    if (!c) return;
    c->gc = gc; c->owner = owner; c->is_egl = is_egl; c->draw = draw; c->read = read;
}

static GuestGC *gles2x_gc(void)
{
    gles2x_cur_t *c = gles2x_cur(0);
    return c ? c->gc : 0;
}

/* The first context: hello, and the front end reported the way gles_discover reports a table. */
static void gles2x_hello(void)
{
    static int done;
    long long hello;
    unsigned n = 0, hand = 0;
    if (done) return;
    done = 1;
    gles_layout_reset();
    gles_hand_table(gles2x_hand);
    hello = gles_hello();
#define GLES2X_FWD(export, row) n++; if (gles_fns[GLES_ROW_##row].id < GLES_N_SLOTS && gles2x_hand[gles_fns[GLES_ROW_##row].id]) hand++;
#include "gles2x_exports.h"
#undef GLES2X_FWD
    w("[gles] dispatch layout from export names (1.x/2.x OpenGLES is the driver): "); wd(n);
    w(" gl exports, "); wd(hand); w(" to hand thunks, "); wd(n - hand);
    w(" forwarded (name table version "); wx(GLES_NAMES_VERSION); w("), host protocol ");
    wd((unsigned)(hello & 0xff)); w("\n");
    if (hello < 0 || (hello & 0xff) != GLES_HELLO_PROTO) {
        w("[gles] the host speaks another wire protocol: GL calls may go astray\n");
        refused("hello:", "protocol", ~0u);
    }
}

/* ------------------------------------------------------------------------ gl* --- */

typedef int (*gles2x_f)(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                        unsigned, unsigned, unsigned, unsigned, unsigned);

/* Row `row` on gc. Every core entry point takes (gc, a0..a{argc-1}); AAPCS lets a caller pass
 * more words than the callee reads, so one 12-word signature calls them all. */
static int gles2x_row(GuestGC *gc, unsigned row, unsigned a0, unsigned a1, unsigned a2, unsigned a3,
                      unsigned a4, unsigned a5, unsigned a6, unsigned a7, unsigned a8, unsigned a9,
                      unsigned a10, unsigned a11)
{
    unsigned id = gles_fns[row].id;
    void *f = id < GLES_N_SLOTS && gles2x_hand[id] ? gles2x_hand[id] : gles_fn_ptr[row];
    return ((gles2x_f)f)(gc, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}
static int gles2x_rowv(GuestGC *gc, unsigned row, const unsigned *a)
{
    return gles2x_row(gc, row, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}
/* A GL call of this file's own, on gc: GL(gc, glBindTexture, target, name). */
#define GL(gc, row, ...) gles2x_rowv(gc, GLES_ROW_##row, (const unsigned[12]){ __VA_ARGS__ })

#define GLES2X_FWD(export, row)                                                                     \
    int export(unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5,        \
               unsigned a6, unsigned a7, unsigned a8, unsigned a9, unsigned a10, unsigned a11);     \
    int export(unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5,        \
               unsigned a6, unsigned a7, unsigned a8, unsigned a9, unsigned a10, unsigned a11)      \
    {                                                                                               \
        GuestGC *gc = gles2x_gc();                                                                  \
        return gc ? gles2x_row(gc, GLES_ROW_##row, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) \
                  : 0;                                                                              \
    }
#include "gles2x_exports.h"
#undef GLES2X_FWD

/* 2.x: (target, CoreSurfaceBuffer); the stock one takes only GL_TEXTURE_RECTANGLE and sets
 * INVALID_ENUM otherwise, which the host's bind does as a counted refusal. */
void glTexImageCoreSurfaceAPPLE(unsigned target, void *buffer);
void glTexImageCoreSurfaceAPPLE(unsigned target, void *buffer)
{
    GuestGC *gc = gles2x_gc();
    if (gc) GLESBindCoreSurface(gc, target, buffer);
}

/* The core's strings, plus the two extensions QuartzCore 2.x refuses GL compositing without
 * ("CoreAnimation: unsupported graphics hardware; need APPLE_texture_rectangle extension; need
 * APPLE_core_surface_texture extension"). Both are real here: glTexImageCoreSurfaceAPPLE is the
 * core's BindCoreSurface, and the host samples GL_TEXTURE_RECTANGLE (3.x CA uses it too). */
const char *glGetString(unsigned name);
const char *glGetString(unsigned name)
{
    static char ext[256];
    const char *s;
    GuestGC *gc = gles2x_gc();
    if (!gc) return 0;
    s = (const char *)(unsigned long)GL(gc, glGetString, name);
    if (name == 0x1F03 && s && !ext[0]) {
        const char *add = " GL_APPLE_texture_rectangle GL_APPLE_core_surface_texture";
        unsigned i = 0, k = 0;
        while (s[k] && i < sizeof ext - 1) ext[i++] = s[k++];
        for (k = 0; add[k] && i < sizeof ext - 1;) ext[i++] = add[k++];
        ext[i] = 0;
    }
    return name == 0x1F03 && ext[0] ? ext : s;
}

void glFinishTextureAPPLE(unsigned target);
void glFinishTextureAPPLE(unsigned target)
{
    GuestGC *gc = gles2x_gc();
    if (gc) GLESFinishTexture(gc, target);
}

/* 1.x only: an ARB vertex-program entry point with no ES 1.1 row (nothing on the device imports it;
 * LayerKit does not). A counted refusal, so a caller shows up in gles-rejects. */
void glVertexAttribPointerARB(unsigned index, int size, unsigned type, unsigned char norm, int stride,
                              const void *ptr);
void glVertexAttribPointerARB(unsigned index, int size, unsigned type, unsigned char norm, int stride,
                              const void *ptr)
{
    (void)index; (void)size; (void)type; (void)norm; (void)stride; (void)ptr;
    refused("gl:", "VertexAttribPointerARB", ~0u);
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
#define GLES2X_DISPLAY ((void *)1)
/* gles.h GLES_SURFACE_WINDOW_ORDER: a pixmap's first row is the top of the picture, GL's last
 * (measured: CA's home screen came out upside down in texture order) */
#define GLES2X_WINDOW_ORDER 0x80000000u

/* The three QuartzCore's gles_get_config tells apart (0x31db1278: R,G,B,A of 8,8,8,8 / 5,6,5,0 /
 * 4,4,4,4, and no depth, stencil or samples). */
static const struct { EGLint r, g, b, a; unsigned fourcc, ifmt; } gles2x_configs[] = {
    { 8, 8, 8, 8, 0x42475241, 0x8058 },     /* 'BGRA', GL_RGBA8_OES */
    { 5, 6, 5, 0, 0x4c353635, 0x8d62 },     /* 'L565', GL_RGB565_OES */
    { 4, 4, 4, 4, 0x34343434, 0x8056 },     /* '4444', GL_RGBA4_OES */
};
#define GLES2X_NCONFIGS 3
static EGLint egl_error = EGL_SUCCESS;       /* ponytail: one for the process, not per thread */

static EGLBoolean egl_fail(EGLint e) { egl_error = e; return 0; }
static void *egl_null(EGLint e) { egl_error = e; return 0; }
static int egl_config(void *c) { unsigned i = (unsigned)(unsigned long)c; return i >= 1 && i <= GLES2X_NCONFIGS ? (int)i - 1 : -1; }

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
    case 0x3020: return gles2x_configs[c].r + gles2x_configs[c].g + gles2x_configs[c].b + gles2x_configs[c].a;
    case 0x3024: return gles2x_configs[c].r;
    case 0x3023: return gles2x_configs[c].g;
    case 0x3022: return gles2x_configs[c].b;
    case 0x3021: return gles2x_configs[c].a;
    case 0x3028: return c + 1;                              /* EGL_CONFIG_ID */
    case 0x3033: return EGL_PIXMAP_BIT | EGL_WINDOW_BIT;    /* EGL_SURFACE_TYPE */
    case 0x302E: return (EGLint)gles2x_configs[c].fourcc;   /* EGL_NATIVE_VISUAL_ID */
    case 0x3040: return 1;                                  /* EGL_RENDERABLE_TYPE: ES 1 */
    case 0x3027: return 0x3038;                             /* EGL_CONFIG_CAVEAT: none */
    case 0x302C: case 0x302A: return 2048;                  /* MAX_PBUFFER_WIDTH/HEIGHT */
    case 0x302D: return 0;                                  /* MAX_SWAP_INTERVAL... */
    default: return 0;                                      /* depth, stencil, samples: none */
    }
}

void *eglGetDisplay(void *native);
void *eglGetDisplay(void *native) { (void)native; return GLES2X_DISPLAY; }
EGLBoolean eglInitialize(void *dpy, EGLint *major, EGLint *minor);
EGLBoolean eglInitialize(void *dpy, EGLint *major, EGLint *minor)
{
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (major) *major = 1;
    if (minor) *minor = 1;
    return 1;
}
EGLBoolean eglTerminate(void *dpy);
EGLBoolean eglTerminate(void *dpy) { return dpy == GLES2X_DISPLAY ? 1 : egl_fail(EGL_BAD_DISPLAY); }
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
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!num) return egl_fail(EGL_BAD_PARAMETER);
    for (i = 0; i < GLES2X_NCONFIGS && (!configs || n < size); i++) {
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
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!num) return egl_fail(EGL_BAD_PARAMETER);
    for (i = 0; i < GLES2X_NCONFIGS; i++) {
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
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (c < 0) return egl_fail(EGL_BAD_CONFIG);
    if (value) *value = egl_config_attrib(c, attr);
    return 1;
}

/* A context: the core's GC, in its own sharegroup or its share context's. */
static GuestGC *gles2x_new_gc(void *share_sg, void **sg_out)
{
    void *sg = share_sg, *gc = 0;
    gles2x_hello();
    if (!sg && !GLESCreateSharegroup(&sg)) return 0;
    if (!GLESCreateGCWithAPI(sg, 0, 0, &gc, 1)) {
        if (!share_sg) GLESDestroySharegroup(sg);
        return 0;
    }
    if (sg_out) *sg_out = sg;
    return gc;
}

void *eglCreateContext(void *dpy, void *config, void *share, const EGLint *attribs);
void *eglCreateContext(void *dpy, void *config, void *share, const EGLint *attribs)
{
    egl_ctx_t *c, *s = egl_ctx(share);
    (void)attribs;
    if (dpy != GLES2X_DISPLAY) return egl_null(EGL_BAD_DISPLAY);
    if (egl_config(config) < 0) return egl_null(EGL_BAD_CONFIG);
    if (share && !s) return egl_null(EGL_BAD_CONTEXT);
    if (!(c = calloc(1, sizeof *c))) return egl_null(0x3003);   /* EGL_BAD_ALLOC */
    if (!(c->gc = gles2x_new_gc(s ? s->sg : 0, &c->sg))) { free(c); return egl_null(0x3003); }
    c->magic = EGL_CTX_MAGIC;
    c->config = egl_config(config);
    if (s) c->sg = s->sg;
    return c;
}

EGLBoolean eglDestroyContext(void *dpy, void *ctx);
EGLBoolean eglDestroyContext(void *dpy, void *ctx)
{
    egl_ctx_t *c = egl_ctx(ctx);
    gles2x_cur_t *cur = gles2x_cur(0);
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!c) return egl_fail(EGL_BAD_CONTEXT);
    if (cur && cur->owner == c) gles2x_set_current(0, 0, 0, 0, 0);
    /* Release cached surface names while their GC is still live. */
    while (c->surfaces) egl_release_names(c->surfaces);
    GLESDestroyGC(c->gc);
    c->magic = 0;
    free(c);   /* ponytail: the sharegroup outlives its contexts (a leak per destroyed group) */
    return 1;
}

static egl_surf_t *egl_new_surface(void *dpy, void *config, unsigned kind, void *native)
{
    egl_surf_t *s;
    if (dpy != GLES2X_DISPLAY) return egl_null(EGL_BAD_DISPLAY);
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

/* A native window is the closure EAGL binds (see GLESBindView): 1.x LayerKit's and 2.x
 * QuartzCore's -nativeWindow shape. */
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
    if (s->kind == EGL_WINDOW_BIT && s->gc) GLESBindView(s->gc, 0, 0, 0);
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
    gles2x_cur_t *cur = gles2x_cur(0);
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
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
        if (!s->gc && !GLESBindView(gc, s->native, (void *)(unsigned long)gles2x_configs[s->config].ifmt, 0)) {
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
        if (!GLESBindCoreSurface(gc, 0x0DE1 | GLES2X_WINDOW_ORDER, s->native)) {
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
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    {
        /* A pixmap made not-current is a finished frame: 2.x CA brackets each frame with
         * eglMakeCurrent(buffer) ... eglMakeCurrent(none) and swaps the buffer right after, never
         * calling glFlush (the MBX driver's swap token waits for the GPU instead). So the host's
         * rendered copy goes to the buffer's memory here, before the swap; left to the next
         * frame, an animation's last frame never reached the panel (a Safari close zoom stuck
         * with its icons half way). */
        gles2x_cur_t *cur = gles2x_cur(0);
        egl_surf_t *was = cur && cur->is_egl ? egl_surf(cur->draw) : 0;
        if (was && was->kind == EGL_PIXMAP_BIT && was->gc && (was != d || !ctx))
            GL(was->gc, glFlush, 0);
    }
    if (!ctx) {
        gles2x_set_current(0, 0, 0, 0, 0);
        return 1;
    }
    if (!c) return egl_fail(EGL_BAD_CONTEXT);
    if ((draw && !d) || (read && !egl_surf(read))) return egl_fail(EGL_BAD_SURFACE);
    gles2x_set_current(c->gc, c, 1, d, read);
    if (d && !egl_attach(c, d)) return egl_fail(EGL_BAD_MATCH);
    return 1;
}

EGLBoolean eglSwapBuffers(void *dpy, void *surface);
EGLBoolean eglSwapBuffers(void *dpy, void *surface)
{
    egl_surf_t *s = egl_surf(surface);
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!s || !s->gc) return egl_fail(EGL_BAD_SURFACE);
    if (s->kind == EGL_WINDOW_BIT) return GLESPresentView(s->gc, 0) ? 1 : egl_fail(EGL_BAD_SURFACE);
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
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
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
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    return egl_surf(surface) ? 1 : egl_fail(EGL_BAD_SURFACE);
}

EGLBoolean eglQueryContext(void *dpy, void *ctx, EGLint attr, EGLint *value);
EGLBoolean eglQueryContext(void *dpy, void *ctx, EGLint attr, EGLint *value)
{
    egl_ctx_t *c = egl_ctx(ctx);
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    if (!c) return egl_fail(EGL_BAD_CONTEXT);
    if (attr != 0x3028 || !value) return egl_fail(EGL_BAD_ATTRIBUTE);
    *value = c->config + 1;
    return 1;
}

void *eglGetCurrentContext(void);
void *eglGetCurrentContext(void)
{
    gles2x_cur_t *c = gles2x_cur(0);
    return c && c->is_egl ? c->owner : 0;
}

void *eglGetCurrentSurface(EGLint which);
void *eglGetCurrentSurface(EGLint which)
{
    gles2x_cur_t *c = gles2x_cur(0);
    if (!c || !c->is_egl) return 0;
    return which == 0x305A ? c->read : c->draw;             /* EGL_READ, else EGL_DRAW */
}

void *eglGetCurrentDisplay(void);
void *eglGetCurrentDisplay(void) { return eglGetCurrentContext() ? GLES2X_DISPLAY : 0; }

EGLBoolean eglWaitGL(void);
EGLBoolean eglWaitGL(void) { GuestGC *gc = gles2x_gc(); if (gc) GL(gc, glFinish, 0); return 1; }
EGLBoolean eglWaitNative(EGLint engine);
EGLBoolean eglWaitNative(EGLint engine) { (void)engine; return 1; }
EGLBoolean eglSwapInterval(void *dpy, EGLint interval);
EGLBoolean eglSwapInterval(void *dpy, EGLint interval) { (void)interval; return dpy == GLES2X_DISPLAY ? 1 : egl_fail(EGL_BAD_DISPLAY); }
void *eglGetProcAddress(const char *name);
void *eglGetProcAddress(const char *name) { return name ? dlsym(RTLD_DEFAULT, name) : 0; }

/* 1.x only: (dpy, surface, ...) -> EGLBoolean, the egl form of 2.x's -swapNotification:forTransaction:
 * onLayer:, which only prints that it is unimplemented. Nothing on 1.x imports it. */
EGLBoolean eglSwapNotification(void *dpy, void *surface, unsigned a, unsigned b);
EGLBoolean eglSwapNotification(void *dpy, void *surface, unsigned a, unsigned b)
{
    (void)a; (void)b;
    if (dpy != GLES2X_DISPLAY) return egl_fail(EGL_BAD_DISPLAY);
    return egl_surf(surface) ? 1 : egl_fail(EGL_BAD_SURFACE);
}

/* ------------------------------------------------------------------------- EAGL --- */

void EAGLGetVersion(unsigned *major, unsigned *minor);
void EAGLGetVersion(unsigned *major, unsigned *minor) { *major = 1; *minor = 0; }   /* 0x3128d734 */

/* The stock one is a breakpoint target its SetError calls; nothing calls ours. */
void opengl_error_break(void);
void opengl_error_break(void) {}

