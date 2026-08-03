/*
 * gles_tex -- a textured quad, drawn by the host from guest-owned pixels.
 *
 * Companion to gles_tri. Where that one proves the vertex path, this one proves
 * the two things it does not touch:
 *
 *   - texture upload: glTexImage2D's pixel data is read out of guest memory by
 *     the host at call time, so a wrong address or size shows up as garbage
 *     rather than as a silent no-op.
 *   - the spilled-argument path: glTexImage2D takes nine scalars, and only four
 *     fit inline in qc_gles_args_t, so this is the first call that forces the
 *     host to go back into guest memory for the rest of its own arguments.
 *
 * The texture is a 2x2 checker with NEAREST filtering, so each texel becomes a
 * large flat block on screen. That is deliberate: a wrong row stride, a swapped
 * channel or a bad filter is obvious by eye, where a photograph would just look
 * vaguely wrong.
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern int usleep(unsigned);

#define QC_GLES 0x140

#define GLES_SLOT_BIND_TEXTURE          5
#define GLES_SLOT_CLEAR                10
#define GLES_SLOT_CLEAR_COLOR          12
#define GLES_SLOT_COLOR4F              37
#define GLES_SLOT_DRAW_ARRAYS          65
#define GLES_SLOT_ENABLE               72
#define GLES_SLOT_ENABLE_CLIENT_STATE  73
#define GLES_SLOT_GEN_TEXTURES         98
#define GLES_SLOT_TEXCOORD_POINTER    289
#define GLES_SLOT_TEX_IMAGE_2D        301
#define GLES_SLOT_TEX_PARAMETERI      304
#define GLES_SLOT_VERTEX_POINTER      334
#define GLES_SLOT_VIEWPORT            335
#define GLES_OP_PRESENT            0x1000

#define GL_TRIANGLE_STRIP        0x0005
#define GL_FLOAT                 0x1406
#define GL_UNSIGNED_BYTE         0x1401
#define GL_COLOR_BUFFER_BIT      0x00004000
#define GL_DEPTH_BUFFER_BIT      0x00000100
#define GL_VERTEX_ARRAY          0x8074
#define GL_TEXTURE_COORD_ARRAY   0x8078
#define GL_TEXTURE_2D            0x0DE1
#define GL_RGBA                  0x1908
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_NEAREST               0x2600

#define QC_GLES_INLINE_ARGS 4

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

static unsigned fbits(float f)
{
    union { float f; unsigned u; } c;
    c.f = f;
    return c.u;
}

int main(void)
{
    /* 2x2 RGBA checker: red, green / blue, white. */
    static unsigned char texels[16] = {
        255,   0,   0, 255,    0, 255,   0, 255,
          0,   0, 255, 255,  255, 255, 255, 255,
    };
    /* A quad as a triangle strip, inset from the panel edges so the blue clear
     * stays visible around it and a stretched or offset quad is obvious. */
    static float verts[8] = {
         40.0f,  90.0f,
        280.0f,  90.0f,
         40.0f, 390.0f,
        280.0f, 390.0f,
    };
    static float uvs[8] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };
    static unsigned texid[1] = { 0 };
    unsigned a[12];
    long long r = 0;
    int frame;

    w("TEX: starting\n");

    a[0] = 1; a[1] = (unsigned)(unsigned long)texid;
    gl(GLES_SLOT_GEN_TEXTURES, 2, a);
    w("TEX: glGenTextures gave id "); wd(texid[0]); w("\n");

    a[0] = GL_TEXTURE_2D; a[1] = texid[0];
    gl(GLES_SLOT_BIND_TEXTURE, 2, a);

    a[0] = GL_TEXTURE_2D; a[1] = GL_TEXTURE_MIN_FILTER; a[2] = GL_NEAREST;
    gl(GLES_SLOT_TEX_PARAMETERI, 3, a);
    a[0] = GL_TEXTURE_2D; a[1] = GL_TEXTURE_MAG_FILTER; a[2] = GL_NEAREST;
    gl(GLES_SLOT_TEX_PARAMETERI, 3, a);

    /* Nine arguments -- this one spills. */
    a[0] = GL_TEXTURE_2D;   /* target */
    a[1] = 0;               /* level */
    a[2] = GL_RGBA;         /* internalformat */
    a[3] = 2;               /* width */
    a[4] = 2;               /* height */
    a[5] = 0;               /* border */
    a[6] = GL_RGBA;         /* format */
    a[7] = GL_UNSIGNED_BYTE;/* type */
    a[8] = (unsigned)(unsigned long)texels;
    r = gl(GLES_SLOT_TEX_IMAGE_2D, 9, a);
    w("TEX: glTexImage2D (9 args, spilled) -> "); wd(r); w("\n");
    if (r == -12345) {
        w("TEX: RESULT=NO_TRAP\n");
        _exit(1);
    }
    if (r != 0) {
        w("TEX: RESULT=TEXIMAGE_FAILED (host could not read the pixels)\n");
        _exit(1);
    }

    for (frame = 0; frame < 400; frame++) {
        a[0] = 0; a[1] = 0; a[2] = 320; a[3] = 480;
        gl(GLES_SLOT_VIEWPORT, 4, a);

        /* Mid-grey, deliberately not any of the four texel colours. A blue
         * clear made the blue texel invisible against the background, which
         * left "that quadrant rendered" and "that quadrant was never covered"
         * indistinguishable -- the quad sits on top of the clear, so both look
         * identical. All four texels have to be tellable apart from it. */
        a[0] = fbits(0.25f); a[1] = fbits(0.25f);
        a[2] = fbits(0.25f); a[3] = fbits(1.0f);
        gl(GLES_SLOT_CLEAR_COLOR, 4, a);

        a[0] = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
        gl(GLES_SLOT_CLEAR, 1, a);

        /* White, so the default GL_MODULATE leaves the texels' own colours
         * alone -- any tint in the result means the texture path is wrong. */
        a[0] = fbits(1.0f); a[1] = fbits(1.0f);
        a[2] = fbits(1.0f); a[3] = fbits(1.0f);
        gl(GLES_SLOT_COLOR4F, 4, a);

        a[0] = GL_TEXTURE_2D;
        gl(GLES_SLOT_ENABLE, 1, a);
        a[0] = GL_TEXTURE_2D; a[1] = texid[0];
        gl(GLES_SLOT_BIND_TEXTURE, 2, a);

        a[0] = GL_VERTEX_ARRAY;
        gl(GLES_SLOT_ENABLE_CLIENT_STATE, 1, a);
        a[0] = GL_TEXTURE_COORD_ARRAY;
        gl(GLES_SLOT_ENABLE_CLIENT_STATE, 1, a);

        a[0] = 2; a[1] = GL_FLOAT; a[2] = 0;
        a[3] = (unsigned)(unsigned long)verts;
        gl(GLES_SLOT_VERTEX_POINTER, 4, a);

        a[0] = 2; a[1] = GL_FLOAT; a[2] = 0;
        a[3] = (unsigned)(unsigned long)uvs;
        gl(GLES_SLOT_TEXCOORD_POINTER, 4, a);

        a[0] = GL_TRIANGLE_STRIP; a[1] = 0; a[2] = 4;
        gl(GLES_SLOT_DRAW_ARRAYS, 3, a);

        gl(GLES_OP_PRESENT, 0, a);
        usleep(25000);
    }

    w("TEX: RESULT=CALLS_COMPLETED (400 frames presented)\n");
    _exit(0);
    return 0;
}
