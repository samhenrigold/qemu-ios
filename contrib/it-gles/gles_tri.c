/*
 * gles_tri -- drive the GLES high-level emulation from the guest, end to end.
 *
 * This is the smallest thing that exercises the whole pixel path: an
 * unprivileged guest process issues GL calls, they trap to the host through
 * QEMU_CALL, the host renders them with real OpenGL, and the result lands where
 * the panel scans it out. If a triangle appears on the screen, every link in
 * that chain works.
 *
 * It deliberately does NOT go through OpenGLES.framework. The framework path
 * needs the whole MBXGLEngine bundle ABI to be right before a single pixel can
 * be checked, and if any part of it is wrong the failure is a SpringBoard crash
 * with nothing to look at. Calling the same slot numbers directly isolates the
 * renderer from the bundle ABI, so each can be brought up and blamed
 * separately. The slot numbers here are the framework's real ones, so the
 * marshalling this exercises is the marshalling the shim will use.
 *
 * Build with contrib/armv6-toolchain (see its README).
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern int usleep(unsigned);

#define QC_GLES 0x140

/* Slots -- OpenGLES.framework dispatch-table offsets / 4. Must agree with
 * include/hw/arm/guest-services/gles.h. */
#define GLES_SLOT_CLEAR                10
#define GLES_SLOT_CLEAR_COLOR          12
#define GLES_SLOT_COLOR4F              37
#define GLES_SLOT_DRAW_ARRAYS          65
#define GLES_SLOT_ENABLE_CLIENT_STATE  73
#define GLES_SLOT_VERTEX_POINTER      334
#define GLES_SLOT_VIEWPORT            335
#define GLES_OP_PRESENT            0x1000

/* GL enums we need, in their ES 1.1 values (identical to desktop). */
#define GL_TRIANGLES            0x0004
#define GL_FLOAT                0x1406
#define GL_COLOR_BUFFER_BIT     0x00004000
#define GL_DEPTH_BUFFER_BIT     0x00000100
#define GL_VERTEX_ARRAY         0x8074

#define QC_GLES_INLINE_ARGS 4

/* Mirrors qemu_call_t exactly: call_number(4) + args(32) + retval(8) +
 * error(8) = 52 bytes packed. The args union is frozen at 32 bytes because
 * agents already compiled into NAND images depend on retval's offset. */
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

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(1, s, slen(s)); }

static void wd(long long v)
{
    char buf[24], *p = buf + 23;
    int neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)-v : (unsigned long long)v;
    *p = 0;
    if (!u) *--p = '0';
    while (u) { *--p = '0' + (u % 10); u /= 10; }
    if (neg) *--p = '-';
    w(p);
}

/* One GL call. Up to four arguments ride inline; anything longer is spilled to
 * a buffer whose address the host reads back out of guest memory. */
static long long gl(unsigned slot, unsigned argc, const unsigned *args)
{
    volatile qemu_call_t q;
    unsigned i;
    static unsigned spill[16];

    for (i = 0; i < sizeof(q.gles.args) / sizeof(q.gles.args[0]); i++)
        q.gles.args[i] = 0;

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

static unsigned fbits(float f)
{
    union { float f; unsigned u; } c;
    c.f = f;
    return c.u;
}

int main(void)
{
    /* A triangle in framebuffer pixel coordinates. The host pre-loads an ortho
     * matrix covering 320x480, so no matrix calls are needed -- one fewer thing
     * that can be wrong on the first run. */
    static float verts[6] = {
         20.0f,  20.0f,
        300.0f,  20.0f,
        160.0f, 460.0f,
    };
    unsigned a[4];
    long long r = 0, rdraw = 0;
    int frame;

    w("TRI: starting\n");

    /* Redraw and present for a few seconds rather than once. CoreAnimation is
     * still compositing the lock screen into the same framebuffer, so a single
     * frame would very likely be overwritten before a screendump could catch
     * it -- and each boot of this guest is expensive enough that "probably
     * missed it" is not a result worth collecting. */
    for (frame = 0; frame < 400; frame++) {
        a[0] = 0; a[1] = 0; a[2] = 320; a[3] = 480;
        gl(GLES_SLOT_VIEWPORT, 4, a);

        /* Opaque blue clear, so a correct frame is unmistakable against both
         * the lock screen and a black panel. */
        a[0] = fbits(0.0f); a[1] = fbits(0.0f);
        a[2] = fbits(1.0f); a[3] = fbits(1.0f);
        gl(GLES_SLOT_CLEAR_COLOR, 4, a);

        a[0] = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
        gl(GLES_SLOT_CLEAR, 1, a);

        a[0] = fbits(1.0f); a[1] = fbits(0.0f);
        a[2] = fbits(0.0f); a[3] = fbits(1.0f);
        gl(GLES_SLOT_COLOR4F, 4, a);

        a[0] = GL_VERTEX_ARRAY;
        gl(GLES_SLOT_ENABLE_CLIENT_STATE, 1, a);

        /* size, type, stride, pointer -- the pointer is a guest VA the host
         * reads with cpu_memory_rw_debug at draw time. */
        a[0] = 2; a[1] = GL_FLOAT; a[2] = 0;
        a[3] = (unsigned)(unsigned long)verts;
        gl(GLES_SLOT_VERTEX_POINTER, 4, a);

        a[0] = GL_TRIANGLES; a[1] = 0; a[2] = 3;
        rdraw = gl(GLES_SLOT_DRAW_ARRAYS, 3, a);

        r = gl(GLES_OP_PRESENT, 0, a);

        if (frame == 0) {
            w("TRI: glDrawArrays -> "); wd(rdraw); w("\n");
            w("TRI: present     -> "); wd(r); w("\n");
            if (r == -12345) {
                w("TRI: RESULT=NO_TRAP (the host never answered)\n");
                _exit(1);
            }
        }
        usleep(25000);
    }

    w("TRI: RESULT=CALLS_COMPLETED (400 frames presented)\n");
    _exit(0);
    return 0;
}
