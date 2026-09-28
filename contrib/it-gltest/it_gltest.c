/*
 * it_gltest -- a GL fixture that needs no app launch (so no activation): a
 * launchd job that draws a known ES 1.1 scene into a CAEAGLLayer on its own
 * remote CAContext, which SpringBoard's CoreAnimation server composites above
 * everything else. tests/ipad1/gltest.py screendumps it and counts colours as
 * the iPod GLES check does.
 *
 * The scene, in a 400x600 layer at (100,100): magenta field, cyan left half
 * (a vertex-array quad), yellow lower-right quarter (a scissored clear). None
 * of the three colours appears in the iOS UI. Before the first present it
 * reads four pixels back with glReadPixels and logs them, so the serial log
 * says whether the host drew the scene even if compositing fails:
 *   it_gltest: readback cyan magenta yellow cyan -> PASS
 * After the first frame a blue band sweeps the magenta quadrant, 6 rows per
 * frame, presented as fast as CA takes frames, for RUN_S seconds; then it
 * exits. The band is what tests/ipad1/gltest.py times (fps, tearing). Plain C with the ObjC
 * runtime dlopen'd, as contrib/it-gles/glapp.c (and for the same reason).
 */
extern long write(int, const void *, unsigned long);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern unsigned sleep(unsigned);
extern void _exit(int);
#define RTLD_NOW 2
#define START_S 12          /* SpringBoard's CA server is up by then */
#define RUN_S 90
#define LX 60
#define LY 80
#define LW 400
#define LH 600

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void w(const char *s) { write(2, s, slen(s)); }
static void wd(unsigned v)
{
    char b[12], *p = b + 11;
    *p = 0;
    if (!v) *--p = '0';
    while (v) { *--p = '0' + v % 10; v /= 10; }
    w(p);
}
static void die(const char *s) { w("it_gltest: "); w(s); w(" -> FAIL\n"); _exit(1); }

typedef void *id_;
typedef struct { float x, y, w, h; } Rect_;
static id_ (*p_getClass)(const char *);
static id_ (*p_sel)(const char *);
static void *p_send;
#define S(n) p_sel(n)
#define C(n) p_getClass(n)
#define M0(o, s) ((id_ (*)(id_, id_))p_send)(o, S(s))
#define M1(o, s, a) ((id_ (*)(id_, id_, id_))p_send)(o, S(s), a)
#define MU(o, s, a) ((id_ (*)(id_, id_, unsigned))p_send)(o, S(s), a)

static void *gl;
#define G(name) static void *name##_; if (!name##_ && !(name##_ = dlsym(gl, #name))) die("no " #name)
#define GLCALL(name, T, ...) ((T)name##_)(__VA_ARGS__)

/* 0 magenta, 1 cyan, 2 yellow, 3 other */
static int classify(const unsigned char *p)
{
    int r = p[0] > 200, g = p[1] > 200, b = p[2] > 200, lr = p[0] < 60, lg = p[1] < 60, lb = p[2] < 60;
    return r && b && lg ? 0 : g && b && lr ? 1 : r && g && lb ? 2 : 3;
}

int main(void)
{
    static const char *const names[] = { "magenta", "cyan", "yellow", "other" };
    /* On the stack, not const: the host reads client arrays straight from guest
     * memory and cannot fault in a page the guest never touched. */
    float quad[8] = { 0, 0, LW / 2, 0, 0, LH, LW / 2, LH };
    /* (x, y) in GL window coordinates, origin bottom-left: expected colour */
    static const int probe[4][3] = { { 50, 500, 1 }, { 350, 500, 0 }, { 350, 100, 2 }, { 50, 100, 1 } };
    unsigned rb = 0, fb = 0, frame;
    int ok = 1, i;
    id_ o, layer, ctx, eagl, pool;

    sleep(START_S);
    w("it_gltest: start\n");
    if (!(o = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW))) die("no libobjc");
    p_getClass = dlsym(o, "objc_getClass");
    p_sel = dlsym(o, "sel_registerName");
    p_send = dlsym(o, "objc_msgSend");
    if (!dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW) ||
        !dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW) ||
        !(gl = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW)))
        die("frameworks missing");
    pool = M0(M0(C("NSAutoreleasePool"), "alloc"), "init");

    layer = M0(C("CAEAGLLayer"), "layer");
    ((void (*)(id_, id_, Rect_))p_send)(layer, S("setFrame:"), (Rect_){ LX, LY, LW, LH });
    MU(layer, "setOpaque:", 1);
    ctx = M0(C("CAContext"), "remoteContext");
    if (!layer || !ctx) die("no CAEAGLLayer / CAContext");
    ((void (*)(id_, id_, float))p_send)(ctx, S("setLevel:"), 100000.0f);
    M1(ctx, "setLayer:", layer);
    MU(ctx, "orderAbove:", 0);       /* a new context is not ordered in: above all others */

    eagl = MU(M0(C("EAGLContext"), "alloc"), "initWithAPI:", 1);
    if (!eagl) die("EAGLContext initWithAPI:1 is nil (no GL context on this image)");
    if (!M1(C("EAGLContext"), "setCurrentContext:", eagl)) die("setCurrentContext");

    G(glGenRenderbuffersOES); G(glBindRenderbufferOES); G(glGenFramebuffersOES); G(glBindFramebufferOES);
    G(glFramebufferRenderbufferOES); G(glCheckFramebufferStatusOES); G(glViewport); G(glClearColor);
    G(glClear); G(glEnable); G(glDisable); G(glScissor); G(glMatrixMode); G(glLoadIdentity); G(glOrthof);
    G(glColor4f); G(glEnableClientState); G(glVertexPointer); G(glDrawArrays); G(glReadPixels); G(glFinish);
    GLCALL(glGenRenderbuffersOES, void (*)(int, unsigned *), 1, &rb);
    GLCALL(glBindRenderbufferOES, void (*)(unsigned, unsigned), 0x8D41, rb);
    if (!((int (*)(id_, id_, unsigned, id_))p_send)(eagl, S("renderbufferStorage:fromDrawable:"), 0x8D41, layer))
        die("renderbufferStorage:fromDrawable:");
    GLCALL(glGenFramebuffersOES, void (*)(int, unsigned *), 1, &fb);
    GLCALL(glBindFramebufferOES, void (*)(unsigned, unsigned), 0x8D40, fb);
    GLCALL(glFramebufferRenderbufferOES, void (*)(unsigned, unsigned, unsigned, unsigned), 0x8D40, 0x8CE0, 0x8D41, rb);
    if (GLCALL(glCheckFramebufferStatusOES, unsigned (*)(unsigned), 0x8D40) != 0x8CD5) die("framebuffer incomplete");

    extern long time(long *);
    long t_end = time(0) + RUN_S;
    for (frame = 0; time(0) < t_end; frame++) {
        GLCALL(glViewport, void (*)(int, int, int, int), 0, 0, LW, LH);
        GLCALL(glMatrixMode, void (*)(unsigned), 0x1701);
        GLCALL(glLoadIdentity, void (*)(void));
        GLCALL(glOrthof, void (*)(float, float, float, float, float, float), 0, LW, 0, LH, -1, 1);
        GLCALL(glMatrixMode, void (*)(unsigned), 0x1700);
        GLCALL(glLoadIdentity, void (*)(void));
        GLCALL(glClearColor, void (*)(float, float, float, float), 1, 0, 1, 1);
        GLCALL(glClear, void (*)(unsigned), 0x4000);
        GLCALL(glColor4f, void (*)(float, float, float, float), 0, 1, 1, 1);
        GLCALL(glEnableClientState, void (*)(unsigned), 0x8074);
        GLCALL(glVertexPointer, void (*)(int, unsigned, int, const void *), 2, 0x1406, 0, quad);
        GLCALL(glDrawArrays, void (*)(unsigned, int, int), 5, 0, 4);
        GLCALL(glEnable, void (*)(unsigned), 0x0C11);                 /* GL_SCISSOR_TEST */
        GLCALL(glScissor, void (*)(int, int, int, int), LW / 2, 0, LW / 2, LH / 2);
        GLCALL(glClearColor, void (*)(float, float, float, float), 1, 1, 0, 1);
        GLCALL(glClear, void (*)(unsigned), 0x4000);
        if (frame) {    /* a blue band sweeping the magenta quadrant: something to time */
            GLCALL(glScissor, void (*)(int, int, int, int), LW / 2, LH / 2 + (int)(frame * 6 % (LH / 2 - 20)), LW / 2, 20);
            GLCALL(glClearColor, void (*)(float, float, float, float), 0, 0, 1, 1);
            GLCALL(glClear, void (*)(unsigned), 0x4000);
        }
        GLCALL(glDisable, void (*)(unsigned), 0x0C11);
        if (!frame) {
            GLCALL(glFinish, void (*)(void));
            w("it_gltest: readback");
            for (i = 0; i < 4; i++) {
                unsigned char px[4] = { 0 };
                int k;
                GLCALL(glReadPixels, void (*)(int, int, int, int, unsigned, unsigned, void *),
                       probe[i][0], probe[i][1], 1, 1, 0x1908, 0x1401, px);
                k = classify(px);
                ok &= k == probe[i][2];
                w(" "); w(names[k]); w("("); wd(px[0]); w(","); wd(px[1]); w(","); wd(px[2]); w(")");
            }
            w(ok ? " -> PASS\n" : " -> FAIL\n");
        }
        GLCALL(glBindRenderbufferOES, void (*)(unsigned, unsigned), 0x8D41, rb);
        MU(eagl, "presentRenderbuffer:", 0x8D41);
        M0(C("CATransaction"), "flush");
        if (!frame) { w("it_gltest: presented, context "); wd((unsigned)(unsigned long)M0(ctx, "contextId")); w("\n"); }
        if (frame % 200 == 0) { w("it_gltest: frame "); wd(frame); w(" at "); wd((unsigned)time(0)); w("\n"); }
    }
    w("it_gltest: done, "); wd(frame); w(" frames\n");
    M0(pool, "release");
    _exit(ok ? 0 : 1);
    return 0;
}
