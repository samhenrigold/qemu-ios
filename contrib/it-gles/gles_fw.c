/*
 * gles_fw -- drive the shim through Apple's real OpenGLES.framework.
 *
 * gles_tri and gles_tex prove the renderer by trapping directly, bypassing the
 * framework entirely. This one proves the other half: that
 * MBXGLEngine.bundle's ABI is right, by making the framework itself load our
 * bundle, call GLESGetEGLInterface, build a GC through GLESCreateGC, and then
 * dispatch real gl* calls through the table we filled.
 *
 * Every gl* call below is Apple's, not ours. If a triangle comes out, the
 * framework's six-instruction trampolines found our function pointers at the
 * slots we put them in, with the GC in arg0 and the arguments where we expect.
 *
 * EAGLContext is Objective-C, but this is a plain C file: calling through
 * objc_msgSend directly avoids dragging an ObjC toolchain into the armv6 build
 * for the sake of two messages. Sending -initWithAPI: is what makes the
 * framework call GLESCreateGC at all; without a current context the trampolines
 * have no TSD entry to read and would fault before reaching us.
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern int usleep(unsigned);

/* The ObjC runtime is resolved at runtime too, for the same reason as the
 * framework below: `-lobjc` against the 3.1.3 SDK is also fatal. libSystem
 * turns out to be special-cased by ld rather than "-l libraries are only a
 * warning", which is what the first attempt here assumed. */
static void *(*p_objc_getClass)(const char *);
static void *(*p_sel_registerName)(const char *);
static void *(*p_objc_msgSend)(void *, void *, ...);

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern const char *dlerror(void);
#define RTLD_NOW 2

/*
 * OpenGLES.framework is resolved at runtime rather than linked.
 *
 * `ld -framework OpenGLES` against the 3.1.3 SDK is a hard error: the modern
 * linker reads the 2009 dylib's platform as 'unknown' and, unlike the same
 * mismatch on a plain -l library (which is only a warning), refuses a framework
 * outright. dlopen has no such opinion, and it is closer to what an app does
 * anyway.
 */
static void (*p_glClearColor)(float, float, float, float);
static void (*p_glClear)(unsigned);
static void (*p_glViewport)(int, int, int, int);
static void (*p_glColor4f)(float, float, float, float);
static void (*p_glEnableClientState)(unsigned);
static void (*p_glVertexPointer)(int, unsigned, int, const void *);
static void (*p_glDrawArrays)(unsigned, int, int);
static unsigned (*p_glGetError)(void);

#define GL_TRIANGLES         0x0004
#define GL_FLOAT             0x1406
#define GL_COLOR_BUFFER_BIT  0x00004000
#define GL_DEPTH_BUFFER_BIT  0x00000100
#define GL_VERTEX_ARRAY      0x8074

#define kEAGLRenderingAPIOpenGLES1 1

/* The present op, trapped directly. EAGL would normally reach it through
 * -presentRenderbuffer:, which needs a real renderbuffer bound to a layer; this
 * test is about the dispatch path, so it presents by hand. */
#define QC_GLES         0x140
#define GLES_OP_PRESENT 0x1000

typedef struct __attribute__((packed)) {
    unsigned int call_number;
    struct __attribute__((packed)) {
        unsigned int slot, ctx, argc, spill, args[4];
    } gles;
    long long retval;
    long long error;
} qemu_call_t;

static void present(void)
{
    volatile qemu_call_t q;
    unsigned i;
    for (i = 0; i < 4; i++) q.gles.args[i] = 0;
    q.call_number = QC_GLES;
    q.gles.slot = GLES_OP_PRESENT;
    q.gles.ctx = 0;
    q.gles.argc = 0;
    q.gles.spill = 0;
    q.retval = 0;
    q.error = 0;
    __asm__ __volatile__("mcr p15, 3, %0, c15, c15, 0" : : "r"(&q) : "memory");
}

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(1, s, slen(s)); }
static void wx(unsigned long v)
{
    char b[11], *p = b + 10;
    *p = 0;
    if (!v) *--p = '0';
    while (v) { *--p = "0123456789abcdef"[v & 15]; v >>= 4; }
    w("0x"); w(p);
}

int main(void)
{
    static float verts[6] = { 20.0f, 20.0f, 300.0f, 20.0f, 160.0f, 460.0f };
    void *cls, *ctx, *h;
    int frame;

    w("FW: starting\n");

    h = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW);
    if (!h) {
        const char *e = dlerror();
        w("FW: dlopen OpenGLES FAILED ["); w(e ? e : "(null)"); w("]\n");
        _exit(1);
    }
    w("FW: OpenGLES loaded\n");

    p_glClearColor        = dlsym(h, "glClearColor");
    p_glClear             = dlsym(h, "glClear");
    p_glViewport          = dlsym(h, "glViewport");
    p_glColor4f           = dlsym(h, "glColor4f");
    p_glEnableClientState = dlsym(h, "glEnableClientState");
    p_glVertexPointer     = dlsym(h, "glVertexPointer");
    p_glDrawArrays        = dlsym(h, "glDrawArrays");
    p_glGetError          = dlsym(h, "glGetError");
    if (!p_glClear || !p_glDrawArrays || !p_glVertexPointer || !p_glClearColor) {
        w("FW: RESULT=MISSING_GL_SYMBOLS\n");
        _exit(1);
    }

    {
        void *o = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
        if (!o) { w("FW: RESULT=NO_LIBOBJC\n"); _exit(1); }
        p_objc_getClass = dlsym(o, "objc_getClass");
        p_sel_registerName = dlsym(o, "sel_registerName");
        p_objc_msgSend = dlsym(o, "objc_msgSend");
        if (!p_objc_getClass || !p_sel_registerName || !p_objc_msgSend) {
            w("FW: RESULT=MISSING_OBJC_SYMBOLS\n");
            _exit(1);
        }
    }

    cls = p_objc_getClass("EAGLContext");
    if (!cls) { w("FW: RESULT=NO_EAGLCONTEXT_CLASS\n"); _exit(1); }

    ctx = p_objc_msgSend(cls, p_sel_registerName("alloc"));
    ctx = p_objc_msgSend(ctx, p_sel_registerName("initWithAPI:"),
                         kEAGLRenderingAPIOpenGLES1);
    w("FW: EAGLContext = "); wx((unsigned long)ctx); w("\n");
    if (!ctx) {
        /* The usual cause is GLESCreateSharegroup or GLESCreateGC handing back
         * something the framework reads as failure. */
        w("FW: RESULT=INITWITHAPI_RETURNED_NIL\n");
        _exit(1);
    }

    if (!p_objc_msgSend(cls, p_sel_registerName("setCurrentContext:"), ctx)) {
        w("FW: RESULT=SETCURRENTCONTEXT_FAILED\n");
        _exit(1);
    }
    w("FW: context is current; all gl* below are Apple's\n");

    /*
     * Hand EAGL a real CAEAGLLayer.
     *
     * -renderbufferStorage:fromDrawable: is the call that makes CoreAnimation
     * allocate a drawable and hand it to the engine, which is the ONLY way to
     * find out three things that have so far only been assumed: which of
     * GLESBindCoreSurface / GLESBindView actually carries the render target,
     * what pixel format CA really picks, and whether the surface address moves
     * between frames.
     *
     * The layer is not in a window, so nothing composites it -- this process is
     * not a UIApplication. That limits what this can prove: it exercises the
     * handoff and the format, but a triangle drawn here does not reach the
     * panel through CA. A real .app is still required for that.
     */
    {
        void *qc_h = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
                            RTLD_NOW);
        void *lcls, *layer;
        long long ok;

        if (!qc_h) { w("FW: QuartzCore dlopen failed\n"); }
        lcls = p_objc_getClass("CAEAGLLayer");
        w("FW: CAEAGLLayer class = "); wx((unsigned long)lcls); w("\n");
        if (!lcls) { w("FW: RESULT=NO_CAEAGLLAYER_CLASS\n"); _exit(1); }

        layer = p_objc_msgSend(lcls, p_sel_registerName("alloc"));
        layer = p_objc_msgSend(layer, p_sel_registerName("init"));
        w("FW: layer = "); wx((unsigned long)layer); w("\n");
        p_objc_msgSend(layer, p_sel_registerName("setOpaque:"), 1);

        /* -setBounds: takes a CGRect by value (four floats). Under the armv6
         * soft-float ABI that is four core-register words, which spills past
         * r0-r3, so it is passed the same way a normal call would. */
        {
            struct { float x, y, w, h; } b = { 0.0f, 0.0f, 320.0f, 480.0f };
            p_objc_msgSend(layer, p_sel_registerName("setBounds:"), b);
        }

        /* EAGL needs a bound renderbuffer before it will attach a drawable. */
        {
            unsigned rb = 0;
            void (*p_glGenRenderbuffersOES)(int, unsigned *) =
                dlsym(h, "glGenRenderbuffersOES");
            void (*p_glBindRenderbufferOES)(unsigned, unsigned) =
                dlsym(h, "glBindRenderbufferOES");
            if (!p_glGenRenderbuffersOES || !p_glBindRenderbufferOES) {
                w("FW: RESULT=NO_OES_RENDERBUFFER_SYMBOLS\n");
                _exit(1);
            }
            p_glGenRenderbuffersOES(1, &rb);
            w("FW: renderbuffer = "); wx(rb); w("\n");
            p_glBindRenderbufferOES(0x8D41 /* GL_RENDERBUFFER_OES */, rb);
        }

        ok = (long long)(long)p_objc_msgSend(
                 ctx, p_sel_registerName("renderbufferStorage:fromDrawable:"),
                 0x8D41, layer);
        w("FW: renderbufferStorage:fromDrawable: -> "); wx((unsigned long)ok);
        w("\n");
        w("FW: (see [mbxshim] lines above for which bind fired, and the "
          "surface geometry/format)\n");
    }

    for (frame = 0; frame < 400; frame++) {
        p_glViewport(0, 0, 320, 480);
        p_glClearColor(0.0f, 0.35f, 0.0f, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        p_glColor4f(1.0f, 1.0f, 0.0f, 1.0f);
        p_glEnableClientState(GL_VERTEX_ARRAY);
        p_glVertexPointer(2, GL_FLOAT, 0, verts);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
        if (frame == 0 && p_glGetError) {
            w("FW: glGetError -> "); wx(p_glGetError()); w("\n");
        }
        present();
        usleep(25000);
    }

    w("FW: RESULT=FRAMEWORK_DISPATCH_COMPLETED\n");
    _exit(0);
    return 0;
}
