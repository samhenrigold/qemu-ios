/*
 * gles_surf -- verify the surface present path writes correct pixels.
 *
 * The panel present is easy to eyeball. The surface present is not: it writes
 * into a buffer nobody displays, so "it worked" and "it did nothing" both leave
 * the screen unchanged. This checks the buffer's actual contents instead.
 *
 * The buffer is prefilled with a poison byte first. That matters: a freshly
 * allocated page is already zero, and black happens to be a plausible render
 * result, so a zero-filled buffer cannot distinguish "wrote black" from "never
 * wrote". Poison makes any untouched byte obvious.
 *
 * It also uses a deliberately odd stride -- wider than the row -- so that a
 * host that ignored stride and packed rows tightly would put the centre sample
 * in the wrong place and fail, rather than passing by accident on a
 * stride == width * 4 buffer.
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);

#define QC_GLES 0x140

#define GLES_SLOT_CLEAR                10
#define GLES_SLOT_CLEAR_COLOR          12
#define GLES_SLOT_COLOR4F              37
#define GLES_SLOT_DRAW_ARRAYS          65
#define GLES_SLOT_ENABLE_CLIENT_STATE  73
#define GLES_SLOT_VERTEX_POINTER      334
#define GLES_SLOT_VIEWPORT            335
#define GLES_OP_PRESENT_SURFACE    0x1001

#define GL_TRIANGLES         0x0004
#define GL_FLOAT             0x1406
#define GL_COLOR_BUFFER_BIT  0x00004000
#define GL_DEPTH_BUFFER_BIT  0x00000100
#define GL_VERTEX_ARRAY      0x8074

#define SURF_W      320
#define SURF_H      480
#define SURF_STRIDE (SURF_W * 4 + 64)   /* deliberately not width*4 */
#define SURF_FMT    0x42475241          /* 'BGRA' */
#define POISON      0xA5

#define QC_GLES_INLINE_ARGS 4

typedef struct __attribute__((packed)) {
    unsigned int call_number;
    struct __attribute__((packed)) {
        unsigned int slot, ctx, argc, spill, args[QC_GLES_INLINE_ARGS];
    } gles;
    long long retval;
    long long error;
} qemu_call_t;

static unsigned char surface[SURF_STRIDE * SURF_H];

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(1, s, slen(s)); }
static void wd(long long v)
{
    char b[24], *p = b + 23;
    int neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)-v : (unsigned long long)v;
    *p = 0;
    if (!u) *--p = '0';
    while (u) { *--p = '0' + (u % 10); u /= 10; }
    if (neg) *--p = '-';
    w(p);
}

static long long gl(unsigned slot, unsigned argc, const unsigned *args)
{
    volatile qemu_call_t q;
    unsigned i;
    static unsigned spill[16];

    for (i = 0; i < QC_GLES_INLINE_ARGS; i++) q.gles.args[i] = 0;
    q.call_number = QC_GLES;
    q.gles.slot = slot;
    q.gles.ctx = 0;
    q.gles.argc = argc;
    q.gles.spill = 0;
    q.retval = -12345;
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

static unsigned fbits(float f) { union { float f; unsigned u; } c; c.f = f; return c.u; }

/* Sample a pixel, honouring the stride. Surface origin is top-left. */
static unsigned char *px(unsigned x, unsigned y)
{
    return &surface[(unsigned long)y * SURF_STRIDE + x * 4];
}

static int check(const char *what, unsigned x, unsigned y,
                 int b, int g, int r)
{
    unsigned char *p = px(x, y);
    int ok = (p[0] == b && p[1] == g && p[2] == r);
    w("  "); w(what); w(" at ("); wd(x); w(","); wd(y); w(") = ");
    wd(p[0]); w(","); wd(p[1]); w(","); wd(p[2]);
    w(ok ? "  OK\n" : "  WRONG\n");
    return ok;
}

int main(void)
{
    static float verts[6] = { 20.0f, 20.0f, 300.0f, 20.0f, 160.0f, 460.0f };
    unsigned a[8];
    long long r;
    unsigned long i;
    int ok = 1;

    w("SURF: starting\n");

    for (i = 0; i < sizeof(surface); i++) surface[i] = POISON;

    a[0] = 0; a[1] = 0; a[2] = SURF_W; a[3] = SURF_H;
    gl(GLES_SLOT_VIEWPORT, 4, a);
    /* Blue clear, red triangle -- same as gles_tri, so the values are known. */
    a[0] = fbits(0.0f); a[1] = fbits(0.0f); a[2] = fbits(1.0f); a[3] = fbits(1.0f);
    gl(GLES_SLOT_CLEAR_COLOR, 4, a);
    a[0] = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
    gl(GLES_SLOT_CLEAR, 1, a);
    a[0] = fbits(1.0f); a[1] = fbits(0.0f); a[2] = fbits(0.0f); a[3] = fbits(1.0f);
    gl(GLES_SLOT_COLOR4F, 4, a);
    a[0] = GL_VERTEX_ARRAY;
    gl(GLES_SLOT_ENABLE_CLIENT_STATE, 1, a);
    a[0] = 2; a[1] = GL_FLOAT; a[2] = 0; a[3] = (unsigned)(unsigned long)verts;
    gl(GLES_SLOT_VERTEX_POINTER, 4, a);
    a[0] = GL_TRIANGLES; a[1] = 0; a[2] = 3;
    gl(GLES_SLOT_DRAW_ARRAYS, 3, a);

    a[0] = (unsigned)(unsigned long)surface;
    a[1] = SURF_STRIDE;
    a[2] = SURF_W;
    a[3] = SURF_H;
    a[4] = SURF_FMT;
    r = gl(GLES_OP_PRESENT_SURFACE, 5, a);
    w("SURF: present-surface -> "); wd(r); w("\n");
    if (r != 0) { w("SURF: RESULT=PRESENT_FAILED\n"); _exit(1); }

    /* Surface is BGRA and top-left origin. The GL triangle's apex is at y=460
     * in GL space, i.e. near the top of the surface. */
    ok &= check("corner (clear, blue)",  4,   4, 255, 0, 0);
    ok &= check("centre (tri, red)",   160, 300,   0, 0, 255);

    /* The gap between width*4 and stride must be untouched: writing into it
     * would mean the host is ignoring stride and running rows together. */
    {
        unsigned char *gap = &surface[(unsigned long)10 * SURF_STRIDE + SURF_W * 4];
        int clean = (gap[0] == POISON && gap[16] == POISON && gap[60] == POISON);
        w("  stride gap preserved: "); w(clean ? "OK\n" : "CLOBBERED\n");
        ok &= clean;
    }

    w(ok ? "SURF: RESULT=SURFACE_PIXELS_CORRECT\n"
         : "SURF: RESULT=SURFACE_PIXELS_WRONG\n");
    _exit(ok ? 0 : 1);
    return 0;
}
