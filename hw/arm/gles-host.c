/*
 * OpenGL ES 1.1 high-level emulation -- host-side renderer.
 *
 * QC_GLES requests arrive here from the guest's MBXGLEngine replacement and are
 * executed against a real OpenGL context on the host. The MBX is not involved
 * at any point: the stock engine's only use of IOKit is inside
 * GLESCreateSharegroup, and once the engine is ours there is no reason to keep
 * that rendezvous alive.
 *
 * The host context is legacy-profile CGL on macOS and an EAGL ES 1.1 context
 * on iOS (see the include block below, and gles-host-eagl.c). CGL on modern
 * macOS is still a
 * genuine fixed-function GL 2.1 implementation (measured: "2.1 Metal - 91.7",
 * glMatrixMode/glOrtho/glColor4f/glVertexPointer/glDrawArrays all real). ES 1.1
 * is close enough to that subset that most entry points forward one-to-one,
 * which is the whole reason this approach is viable -- there is no shader
 * translation anywhere in this file.
 *
 * SCOPE. This is deliberately not 178 entry points, and it is not a guess at
 * what a game might want either. The bring-up set was the smallest thing that
 * gets a triangle onto the panel end to end; it has since been extended by
 * exactly the entry points a real game imports -- `nm -u` on Cube Runner lists
 * 41 gl* symbols and no others, and all 41 are now handled. Everything else is
 * still a logged no-op, which is the right default: an unimplemented state
 * setter that silently does nothing costs a wrong pixel, while guessing at
 * semantics costs a debugging session.
 *
 * Notably absent from that list: textures. The game uploads none, so the
 * texture path here is still only exercised by the bring-up tests.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/arm/ipod_touch_2g.h"
#include "hw/arm/guest-services/general.h"

/*
 * TWO HOSTS, ONE RENDERER.
 *
 * Everything below the include block is platform-neutral: it speaks
 * fixed-function GL and reads the finished frame back into guest memory, and
 * that is all it ever needs the host for. The only parts that differ between
 * macOS and iOS are (a) which header spells the API and (b) who creates the
 * context, so those are the only parts that are conditional:
 *
 *   macOS   OpenGL.framework, a legacy CGL context (created here).
 *   iOS     OpenGLES.framework ES 1.1, an EAGLContext (created in
 *           gles-host-eagl.m -- context creation is the one piece that has to
 *           be Objective-C).
 *
 * iOS is the easier of the two targets, not the harder one: the guest API IS
 * the host API there, so the desktop-vs-ES differences run the other way. What
 * has to be bridged is the handful of places where this file was written
 * against desktop GL -- the *EXT framebuffer-object names are *OES in ES, and
 * glOrtho/glFrustum/glClearDepth take doubles on the desktop and floats in ES.
 * Nothing here is a reimplementation; each shim below is a rename.
 */
#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#define GLES_HOST_EAGL 1
#endif

#ifdef GLES_HOST_EAGL

#include <OpenGLES/ES1/gl.h>
#include <OpenGLES/ES1/glext.h>

/* Framebuffer objects are core in ES 1.1's OES form. Same tokens, same
 * arguments, different suffix. */
#define glGenFramebuffersEXT          glGenFramebuffersOES
#define glBindFramebufferEXT          glBindFramebufferOES
#define glFramebufferTexture2DEXT     glFramebufferTexture2DOES
#define glGenRenderbuffersEXT         glGenRenderbuffersOES
#define glBindRenderbufferEXT         glBindRenderbufferOES
#define glRenderbufferStorageEXT      glRenderbufferStorageOES
#define glFramebufferRenderbufferEXT  glFramebufferRenderbufferOES
#define glCheckFramebufferStatusEXT   glCheckFramebufferStatusOES
#define glDeleteFramebuffersEXT       glDeleteFramebuffersOES
#define glDeleteRenderbuffersEXT      glDeleteRenderbuffersOES
#define glGetRenderbufferParameterivEXT glGetRenderbufferParameterivOES
#define GL_RGB8                       GL_RGB8_OES
#define GL_RGBA8                      GL_RGBA8_OES
#define GL_FRAMEBUFFER_EXT            GL_FRAMEBUFFER_OES
#define GL_RENDERBUFFER_EXT           GL_RENDERBUFFER_OES
#define GL_COLOR_ATTACHMENT0_EXT      GL_COLOR_ATTACHMENT0_OES
#define GL_DEPTH_ATTACHMENT_EXT       GL_DEPTH_ATTACHMENT_OES
#define GL_FRAMEBUFFER_COMPLETE_EXT   GL_FRAMEBUFFER_COMPLETE_OES
#define GL_DEPTH_COMPONENT16          GL_DEPTH_COMPONENT16_OES

/* ES has no double-precision entry points; the guest only ever had floats. */
#define glOrtho(l, r, b, t, n, f)     glOrthof(l, r, b, t, n, f)
#define glFrustum(l, r, b, t, n, f)   glFrustumf(l, r, b, t, n, f)
#define glClearDepth(d)               glClearDepthf(d)

/*
 * Enums that exist only as values on ES: the guest can name a client-array
 * type this host cannot serve, and gles_pointer_ok has to be able to recognise
 * it in order to reject it. Defining them is not claiming support.
 */
#ifndef GL_INT
#define GL_INT                        0x1404
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT               0x1405
#endif
#ifndef GL_DOUBLE
#define GL_DOUBLE                     0x140A
#endif

/*
 * Readback format for a BGRA destination.
 *
 * The desktop asks for GL_BGRA + UNSIGNED_INT_8_8_8_8_REV, which lands as
 * B,G,R,A bytes on a little-endian host. ES has no packed-int pixel types, but
 * GL_EXT_read_format_bgra (present on every iOS GL stack, verified on the
 * runtime) gives the same byte order from GL_BGRA + GL_UNSIGNED_BYTE. Measured
 * against a red triangle: RGBA reads ff0000ff, BGRA reads 0000ffff.
 */
#define GLES_BGRA_READ_TYPE           GL_UNSIGNED_BYTE

/* GL_BGRA is GL_BGRA_EXT in ES's headers -- same token, and the extension that
 * defines it (GL_APPLE_texture_format_BGRA8888) is present on every iOS GL
 * stack. It is what an IOSurface colour attachment is described with. */
#ifndef GL_BGRA
#define GL_BGRA                       GL_BGRA_EXT
#endif

/* Context management and the IOSurface render target live in
 * gles-host-eagl.c; both need Objective-C. */
bool gles_eagl_context_create(void);
void gles_eagl_make_current(void);
bool gles_eagl_iosurface_bind(uint32_t width, uint32_t height,
                              unsigned long target,
                              unsigned long internal_format,
                              unsigned long format, unsigned long type);
void *gles_eagl_iosurface_lock(size_t *stride);
void gles_eagl_iosurface_unlock(void);

#else /* macOS: legacy CGL */

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#define GLES_BGRA_READ_TYPE           GL_UNSIGNED_INT_8_8_8_8_REV

#endif

/* The panel. iOS keeps its framebuffer in portrait and rotates inside it, so
 * these are the framebuffer's dimensions, not the UI's. */
#define GLES_FB_WIDTH  320
#define GLES_FB_HEIGHT 480

/* Ceilings on guest-supplied sizes. The guest is the thing being emulated, so
 * a corrupt or hostile value must not be able to drive a multi-GB g_malloc
 * (which aborts QEMU on failure). A 4Kx4K RGBA texture and 64K object names are
 * both far above anything this device does. */
#define GLES_MAX_TEX_BYTES ((size_t)4096 * 4096 * 4)
#define GLES_MAX_NAMES     65536u

/* Guest vertex/texcoord array state. The guest hands us a pointer into its own
 * address space; nothing is read from it until a draw call, exactly as GL
 * specifies, so the guest is free to refill the buffer between calls. */
/*
 * A buffer object's bytes, held here rather than on the host GL.
 *
 * The host never sees a VBO: the draw path already has to marshal client arrays
 * out of guest memory into scratch and hand GL a host pointer, and a buffer's
 * contents are just that same scratch with a longer lifetime. Keeping them here
 * means the two sources meet at one place (gles_bind_array) instead of forking
 * the whole draw path.
 */
typedef struct {
    uint8_t *data;
    size_t size;
} GLESBuffer;

typedef struct {
    uint32_t enabled;
    uint32_t size;      /* components per element */
    uint32_t type;      /* GL_FLOAT etc, in ES enum values (same as desktop) */
    uint32_t stride;
    /*
     * Guest VA, OR a byte offset into `vbo` when one is set. GL decides which
     * at the moment gl*Pointer is called -- the binding in force then is
     * captured with the pointer -- so the resolution happens there and not at
     * draw time, and the draw path pays a null check rather than a lookup.
     */
    uint32_t ptr;
    const GLESBuffer *vbo;

    /* Scratch for pulling this array across. Grown as needed, never shrunk.
     * Per-array rather than shared, because a single draw needs several of
     * them live at once -- position, colour and normal all point into their
     * own copy while glDrawArrays runs. */
    uint8_t *buf;
    size_t buf_size;
    /* Second scratch, used only to widen GL_FIXED to float. */
    float *fbuf;
    size_t fbuf_count;

    /* Which client-state enum turns this array on, e.g. GL_VERTEX_ARRAY. */
    GLenum client_state;
} GLESArray;

typedef struct {
    bool inited;
    bool failed;
#ifndef GLES_HOST_EAGL
    CGLContextObj cgl;
#endif
    GLuint fbo, tex, depth;

    /*
     * True when the colour attachment is an IOSurface and the present can read
     * the frame straight out of it. False means the readback path below, which
     * is both the macOS behaviour and the iOS fallback.
     */
    bool iosurface;

    GLESArray vertex;
    GLESArray texcoord;
    GLESArray color;
    GLESArray normal;

    /* Scratch for glDrawElements' index list. */
    uint8_t *ibuf;
    size_t ibuf_size;

    /* Grow-never-shrink staging buffer for texture uploads. A streaming
     * glTexSubImage2D runs per frame, so malloc/free'ing it each call (as the
     * texture slots used to) allocates megabyte buffers 60x/s for no reason. */
    uint8_t *txbuf;
    size_t txbuf_size;
    uint8_t *zerobuf;           /* cleared storage for contentless textures */
    size_t zerobuf_size;

    /* Where a compressed upload is expanded to RGBA8 before it goes to the
     * host. Same grow-and-keep idiom, and separate from txbuf because the
     * decode reads one while writing the other. */
    uint8_t *decbuf;
    size_t decbuf_size;

    /* Buffer objects: name -> GLESBuffer. Created lazily; a title that uses no
     * VBOs never allocates it. The two bindings are resolved pointers rather
     * than names, because that is what the draw path wants. */
    GHashTable *buffers;
    uint32_t next_buffer_name;
    const GLESBuffer *array_buffer;
    const GLESBuffer *element_buffer;

    uint8_t *readback;  /* GLES_FB_WIDTH * GLES_FB_HEIGHT * 4 */

    /*
     * Framebuffer objects, wired to real host FBOs -- with ONE exception.
     *
     * Exactly one of the guest's framebuffers is the drawable: CoreAnimation
     * owns its colour renderbuffer and we present out of gh.fbo, so that one
     * has to keep resolving to gh.fbo. Every OTHER framebuffer the guest
     * creates is a genuine offscreen target and gets a genuine host FBO.
     *
     * Telling them apart needs no guesswork. EAGL gives the drawable its
     * storage through -renderbufferStorage:fromDrawable:, which is not a GL
     * call at all, so the drawable renderbuffer is precisely the one the guest
     * never passes to glRenderbufferStorage. rb_sized records the ones it
     * does; a colour attachment missing from that set marks its framebuffer
     * as the drawable, in fbo_drawable.
     */
    GHashTable *rb_sized;       /* renderbuffer name -> given explicit storage */
    GHashTable *fbo_drawable;   /* framebuffer name  -> is the CA drawable */
    uint32_t bound_renderbuffer;
    uint32_t bound_framebuffer;

    uint64_t draws;
    uint64_t presents;

    /* Split by call, and reported per burst of frames. "The scene is empty"
     * has two very different causes -- the app issued no geometry, or it
     * issued plenty and none of it landed on screen -- and a single number
     * cannot tell them apart. */
    uint64_t draw_arrays;
    uint64_t draw_elements;
    uint64_t last_report_present;
    uint64_t last_report_arrays;
    uint64_t last_report_elements;

    /* When nonzero, log every draw for one frame. See gles_trace_draw. */
    int trace_draws;

    /*
     * Where the frame time goes, behind IT_GLES_PROF. Sampling the clock is
     * itself measurable at this call rate -- tens of thousands of requests a
     * second -- so it is off unless asked for, and the totals are reported
     * alongside the frame rate they explain.
     *
     *   t_call    everything between entering and leaving gles_host_call
     *   t_fetch   pulling guest memory across (vertex/colour/normal arrays,
     *             index lists)
     *   t_err     glGetError, which is the one call in the draw path that
     *             cannot be pipelined -- it drains the driver's queue
     *   t_present waiting for the GPU and writing the frame into guest memory
     */
    uint64_t calls;
    uint64_t t_call, t_fetch, t_err, t_present;
    uint64_t last_report_calls;
    uint64_t last_report_t_call, last_report_t_fetch;
    uint64_t last_report_t_err, last_report_t_present;

    /*
     * Frame-to-frame interval over the reporting burst. A mean of 60 fps is
     * exactly what a stutter looks like when it is averaged: sixty frames
     * containing one 300 ms stall and fifty-nine 3 ms frames still average out
     * near 60. "Starts and stops" is a claim about the tail, so the tail is
     * what gets recorded -- the worst interval and how many were over 33 ms
     * (two panel refreshes) and 100 ms (visible as a hitch).
     */
    uint64_t last_present_ns;
    uint64_t frame_gap_max;
    uint32_t frames_over_33ms;
    uint32_t frames_over_100ms;

    /*
     * What this frame actually drew, by primitive, reset at every present.
     *
     * This exists because a run of draws got attributed to the wrong screen
     * twice: once by reading the tail of a back-to-front sorted list and
     * describing the whole frame from it, and once by using the fog COLOUR as
     * a proxy for which scene was on screen. Both were guesses about scene
     * identity dressed as measurements, and both survived review because the
     * numbers they produced looked plausible.
     *
     * A frame's primitive mix is not a proxy for the scene -- it IS the frame.
     * Printing it whenever the mix CHANGES marks every transition
     * structurally, at one line per transition rather than one per frame, and
     * leaves nothing for the next reader to infer.
     */
    uint32_t f_tris, f_linestrips, f_tristrips, f_other;
    uint32_t last_sig;

    /*
     * Set when a per-draw trace burst is armed, cleared when the frame it
     * covers has been presented and written out. Without this the draw list
     * and the picture come from different instants, which is exactly the gap
     * that let "the obstacles are submitted but never drawn" go unverified
     * through three sessions: a handful of stills from one moment were
     * compared against a draw list from another.
     */
    bool dump_pending;

    /*
     * GL_UNPACK_ALIGNMENT as the guest last set it. Zero means "never set",
     * which is treated as ES 1.1's default of 4 -- not 1. Getting that default
     * backwards is silent: it only shows up as sheared rows on the non-4-byte
     * formats, and only at widths that are not already aligned.
     */
    uint32_t unpack_alignment;
} GLESHost;

/*
 * One global context, not one per guest GC.
 *
 * This is a real limitation and it is visible from the guest: run a test twice
 * and glGenTextures keeps counting up (2, then 3), because the host context
 * outlives the guest process that created the texture. Two guests rendering at
 * once would stomp on each other's state entirely.
 *
 * It is fine for bring-up and wrong for anything real. The fix is to key state
 * off the `ctx` field that every request already carries -- it is the engine's
 * GC handle, which is exactly the right identity -- and that is deliberately
 * deferred until the pixel path itself is trustworthy.
 */
static GLESHost gh;

/*
 * Profiling and strictness switches, read once.
 *
 *   IT_GLES_PROF    account for the frame time (see the t_* fields above)
 *   IT_GLES_STRICT  check glGetError after every pointer call and every draw
 *
 * IT_GLES_STRICT defaults ON, and that is a measurement talking, not caution.
 * glGetError is a synchronisation point -- it drains the driver's command
 * queue, so it cannot be pipelined -- and the draw path calls it up to nine
 * times per draw (twice per bound array, once after the draw). That reads like
 * an obvious bottleneck and it is not: profiled through Cube Runner's title
 * screen, its menus and its gameplay, at 400-500 requests a frame, glGetError
 * accounts for 0.1% of wall time. Removing it would buy nothing, so it stays,
 * and the switch exists only to take it out of the way when profiling
 * something else.
 *
 * The checks earn their keep: a pointer the host had refused left a null array
 * armed and the next draw took QEMU down inside the driver. gles_pointer_ok is
 * now the primary guard -- it rejects the type/size combinations desktop GL
 * does not accept, from tables rather than from the driver's verdict, so the
 * bad call never reaches it -- and these are the backstop for a combination we
 * have not thought of.
 */
static int gles_prof = -1;
static int gles_strict = -1;
/*
 * IT_GLES_SWIZZLE=1 restores the old readback -- RGBA plus a per-pixel byte
 * swap -- and =2 alternates between old and new every 300 frames. Alternating
 * is the only honest way to measure this here: the machine runs several
 * emulators at once and its load moves by a factor of five within a minute, so
 * two boots minutes apart cannot be compared. Interleaved, both paths see the
 * same conditions.
 */
static int gles_swizzle;
static int gles_swizzle_mode;
/*
 * IT_GLES_NO_IOSURFACE=1 forces the readback present even where the IOSurface
 * attachment would work. The two paths produce identical pixels by design, so
 * the only way to tell them apart is to time them against each other on the
 * same device in the same run -- and the same argument as IT_GLES_SWIZZLE
 * applies: a comparison across two boots measures the phone's mood.
 */
static int gles_no_iosurface;

static void gles_read_switches(void)
{
    const char *v;

    if (gles_prof >= 0) {
        return;
    }
    v = getenv("IT_GLES_NO_IOSURFACE");
    gles_no_iosurface = v ? atoi(v) : 0;
    v = getenv("IT_GLES_PROF");
    gles_prof = v ? atoi(v) : 0;
    v = getenv("IT_GLES_STRICT");
    gles_strict = v ? atoi(v) : 1;
    v = getenv("IT_GLES_SWIZZLE");
    gles_swizzle_mode = v ? atoi(v) : 0;
    gles_swizzle = (gles_swizzle_mode == 1);
}

/* Nanoseconds, or 0 when profiling is off -- so the caller pays nothing. */
static inline uint64_t gles_t(void)
{
    return gles_prof ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
}

/* ---------------------------------------------------------------- context */

/*
 * Create the host context and make it current, or say why not.
 *
 * The contract both backends satisfy: on success a fixed-function context is
 * current on THIS thread and the shared FBO setup below can run against it. On
 * failure gles_host_init marks the host failed and every request from then on
 * returns -1, which is the guest's cue to fall back to its software renderer.
 * A missing context is therefore never fatal to the emulator.
 */
#ifdef GLES_HOST_EAGL

static bool gles_platform_context_create(void)
{
    return gles_eagl_context_create();
}

static inline void gles_platform_make_current(void)
{
    gles_eagl_make_current();
}

/*
 * Make the frame buffer's colour attachment an IOSurface, if this OS still
 * allows it. The caller has the texture bound; on success this has taken the
 * place of its glTexImage2D.
 */
static bool gles_platform_attach_iosurface(void)
{
    return gles_eagl_iosurface_bind(GLES_FB_WIDTH, GLES_FB_HEIGHT,
                                    GL_TEXTURE_2D, GL_RGBA, GL_BGRA,
                                    GL_UNSIGNED_BYTE);
}

static inline void *gles_platform_frame_lock(size_t *stride)
{
    return gles_eagl_iosurface_lock(stride);
}

static inline void gles_platform_frame_unlock(void)
{
    gles_eagl_iosurface_unlock();
}

#else /* CGL */

static bool gles_platform_context_create(void)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)16,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    CGLError e;

    e = CGLChoosePixelFormat(attrs, &pix, &npix);
    if (e || !pix) {
        fprintf(stderr, "[gles] CGLChoosePixelFormat failed (%d)\n", e);
        return false;
    }
    e = CGLCreateContext(pix, NULL, &gh.cgl);
    CGLDestroyPixelFormat(pix);
    if (e || !gh.cgl) {
        fprintf(stderr, "[gles] CGLCreateContext failed (%d)\n", e);
        return false;
    }
    CGLSetCurrentContext(gh.cgl);
    return true;
}

/*
 * Every request used to call CGLSetCurrentContext unconditionally. All of them
 * arrive on the one vCPU thread and nothing else on this process's threads
 * touches CGL, so after the first it is always already current -- and the
 * check is a thread-local read against a call into the GL stack.
 */
static inline void gles_platform_make_current(void)
{
    if (CGLGetCurrentContext() != gh.cgl) {
        CGLSetCurrentContext(gh.cgl);
    }
}

/*
 * macOS keeps the readback present, on purpose.
 *
 * CGLTexImageIOSurface2D exists and the same attachment could be built here,
 * but the problem it solves is a tiler's: an on-demand tile resolve into
 * driver staging, which is what makes glReadPixels pathological on a phone and
 * merely unremarkable on an immediate-mode desktop GPU. The macOS path is the
 * one that is measured, boots and renders today, so it keeps the code that was
 * measured. Half-converting it would buy an unmeasured maybe and put the
 * working host at risk.
 */
static bool gles_platform_attach_iosurface(void)
{
    return false;
}

static inline void *gles_platform_frame_lock(size_t *stride)
{
    return NULL;
}

static inline void gles_platform_frame_unlock(void)
{
}

#endif /* GLES_HOST_EAGL */

/* Defined with the compressed-texture decoder below; run once from init. */
static void pvrtc_selfcheck(void);

/*
 * The host framebuffer a guest framebuffer name means. Everything resolves to
 * itself except the drawable -- and framebuffer 0, which on iOS is never the
 * render target (EAGL always renders into an FBO) but is what an app binds to
 * mean "back to the screen".
 */
static GLuint gles_host_fbo(uint32_t name)
{
    if (!name || g_hash_table_contains(gh.fbo_drawable,
                                       GUINT_TO_POINTER(name))) {
        return gh.fbo;
    }
    return name;
}

/*
 * ES renderbuffer formats in desktop GL terms. Most are spelled the same, but
 * GL_RGB565 is ES-only and desktop GL rejects it -- which would leave the
 * framebuffer INCOMPLETE and the app staring at an empty target.
 */
static GLenum gles_rb_format(uint32_t fmt)
{
    switch (fmt) {
    case 0x8D62: return GL_RGB8;    /* GL_RGB565 -- no desktop equivalent */
    case 0x8058: return GL_RGBA8;   /* GL_RGBA8_OES  */
    case 0x8051: return GL_RGB8;    /* GL_RGB8_OES   */
    default:     return fmt;        /* RGBA4, RGB5_A1, DEPTH_COMPONENT16... */
    }
}

static bool gles_host_init(void)
{
    if (gh.inited) {
        return true;
    }
    if (gh.failed) {
        return false;
    }

    if (!gles_platform_context_create()) {
        gh.failed = true;
        return false;
    }

    gh.rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
    gh.fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);

    glGenFramebuffersEXT(1, &gh.fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gh.fbo);

    glGenTextures(1, &gh.tex);
    glBindTexture(GL_TEXTURE_2D, gh.tex);
    /*
     * Storage for the colour attachment, from one of two places. An IOSurface
     * gives the CPU a mapped view of the very memory the GPU renders into, so
     * the present becomes a copy out of it instead of a glReadPixels; ordinary
     * texture storage is the fallback and stays the readback path. Either way
     * this is the same attachment to the same FBO and the guest cannot tell.
     */
    gh.iosurface = !gles_no_iosurface && gles_platform_attach_iosurface();
    if (!gh.iosurface) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLES_FB_WIDTH, GLES_FB_HEIGHT,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, gh.tex, 0);

    glGenRenderbuffersEXT(1, &gh.depth);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, gh.depth);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT16,
                             GLES_FB_WIDTH, GLES_FB_HEIGHT);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT, gh.depth);

    /*
     * An IOSurface attachment that the driver accepts and then cannot render to
     * is the one failure that must not be fatal: it would take the whole HLE
     * down over a fast path. Rebuild the attachment out of ordinary texture
     * storage and check again -- that configuration is the one that has been
     * running all along.
     */
    if (gh.iosurface
        && glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)
           != GL_FRAMEBUFFER_COMPLETE_EXT) {
        fprintf(stderr, "[gles] FBO incomplete with an IOSurface colour "
                "attachment; presenting by readback\n");
        gh.iosurface = false;
        glBindTexture(GL_TEXTURE_2D, gh.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLES_FB_WIDTH, GLES_FB_HEIGHT,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                  GL_TEXTURE_2D, gh.tex, 0);
    }

    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)
        != GL_FRAMEBUFFER_COMPLETE_EXT) {
        fprintf(stderr, "[gles] FBO incomplete\n");
        gh.failed = true;
        return false;
    }

    /*
     * Both matrices start as IDENTITY, because that is what ES 1.1 specifies
     * and therefore what every app was written against.
     *
     * This used to pre-load a viewport-sized ortho "so that a guest which never
     * touches the matrix stack still draws in framebuffer pixel coordinates".
     * That convenience was a trap. glFrustumf/glOrthof MULTIPLY into the
     * current matrix, and an app is entitled to skip glLoadIdentity for its
     * very first projection because the spec promises identity is already
     * there. Super Monkey Ball does exactly that:
     *
     *     glMatrixMode(GL_PROJECTION);
     *     glFrustumf(-0.0866, 0.0866, -0.1299, 0.1299, 0.15, 1000);
     *
     * so its perspective matrix came out as ortho(320x480) x frustum -- every
     * world coordinate scaled down by 160 in x and 240 in y, which collapsed
     * the entire 3D scene into a few slivers radiating from one corner while
     * the 2D HUD (which sets its own ortho each frame) drew perfectly. The
     * measured projection was diag=(0.0108 0.0048 1.0003) against the
     * (1.732 1.1547 -1.0003) the app asked for: off by exactly 2/320 and 2/480.
     *
     * A guest that truly never sets a projection now gets clip coordinates,
     * which is what it would get on the device.
     */
    glViewport(0, 0, GLES_FB_WIDTH, GLES_FB_HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    gh.vertex.client_state   = GL_VERTEX_ARRAY;
    gh.texcoord.client_state = GL_TEXTURE_COORD_ARRAY;
    gh.color.client_state    = GL_COLOR_ARRAY;
    gh.normal.client_state   = GL_NORMAL_ARRAY;
    /* glNormalPointer takes no size; the array is always 3 components. */
    gh.normal.size = 3;

    gh.readback = g_malloc0((size_t)GLES_FB_WIDTH * GLES_FB_HEIGHT * 4);
    gh.inited = true;

    /* Microseconds, once, and it is the only thing standing between a broken
     * PVRTC decoder and a silently wrong picture. See pvrtc_selfcheck. */
    pvrtc_selfcheck();

    fprintf(stderr, "[gles] host GL up: %s / %s, present by %s\n",
            glGetString(GL_VERSION), glGetString(GL_RENDERER),
            gh.iosurface ? "IOSurface" : "readback");
    return true;
}

/* ------------------------------------------------------------ guest arrays */

static uint32_t gles_type_size(uint32_t type)
{
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:  return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT: return 2;
    case GL_FLOAT:          return 4;
    /* GL_FIXED is an ES 1.1 type with no desktop equivalent, so the enum is
     * spelled out. Its 16.16 data would still need converting before a desktop
     * draw could use it; nothing we render yet uses fixed-point arrays. */
    case 0x140C:            return 4;
    default:                return 0;
    }
}

/* GL_FIXED is an ES 1.1 type with no desktop equivalent. */
#define GLES_FIXED 0x140C

/*
 * Does this array's type have to be widened to float before the host will take
 * it?
 *
 * ES 1.1 and desktop GL do NOT accept the same array types, and the difference
 * is not symmetric across the four pointer calls:
 *
 *   glVertexPointer     ES: BYTE SHORT FIXED FLOAT   desktop: SHORT INT FLOAT DOUBLE
 *   glTexCoordPointer   ES: BYTE SHORT FIXED FLOAT   desktop: SHORT INT FLOAT DOUBLE
 *   glNormalPointer     ES: BYTE SHORT FIXED FLOAT   desktop: BYTE SHORT INT FLOAT DOUBLE
 *   glColorPointer      ES: UBYTE FIXED FLOAT        desktop: BYTE UBYTE SHORT ... FLOAT
 *
 * So GL_BYTE positions and texture coordinates are legal ES and illegal
 * desktop, while GL_BYTE normals and UNSIGNED_BYTE colours are legal in both.
 *
 * THIS IS WHY IT MATTERS, and it cost a QEMU crash to find: a rejected
 * glVertexPointer does not fail loudly. It sets GL_INVALID_ENUM and leaves the
 * array's pointer at NULL -- while glEnableClientState(GL_VERTEX_ARRAY) has
 * already succeeded. The next glDrawArrays then walks a null pointer inside the
 * driver and takes the whole emulator down with SIGSEGV in
 * gleRunVertexSubmitImmediate. Cube Runner stores its cube geometry as bytes,
 * which was an entirely sensible thing to do in 2009, and it hit this on its
 * very first draw call.
 *
 * Widening is only correct because these are the types whose values are used
 * unscaled. Normals and colours are NOT widened here: GL_BYTE normals and
 * GL_UNSIGNED_BYTE colours are normalised to [-1,1] and [0,1] by both ES and
 * desktop GL, so passing them through keeps that conversion in the driver where
 * it belongs -- converting them by hand would mean reimplementing the scaling
 * and getting it subtly wrong.
 */
static bool gles_needs_widen(GLenum client_state, uint32_t type)
{
    if (type == GLES_FIXED) {
        return true;            /* no desktop equivalent for any array */
    }
    if (type == GL_BYTE && (client_state == GL_VERTEX_ARRAY ||
                            client_state == GL_TEXTURE_COORD_ARRAY)) {
        return true;
    }
    return false;
}

/*
 * Pull `count` elements of a client array out of guest memory and point the
 * host GL at the copy.
 *
 * A stride of 0 means tightly packed, per GL. Anything else and we still have
 * to copy the whole span including the gaps, because the host GL will walk it
 * with the same stride.
 *
 * Returns true if the array was bound and the caller must disable it again.
 */
static const char *gles_array_name(GLenum client_state)
{
    switch (client_state) {
    case GL_VERTEX_ARRAY:        return "vertex";
    case GL_TEXTURE_COORD_ARRAY: return "texcoord";
    case GL_COLOR_ARRAY:         return "color";
    case GL_NORMAL_ARRAY:        return "normal";
    default:                     return "unknown";
    }
}

/*
 * Would desktop GL accept this pointer call?
 *
 * These are GL 2.1's tables for the four gl*Pointer entry points, and they are
 * spelled out rather than inferred because the four do NOT agree: GL_BYTE is
 * legal for colours and normals and illegal for positions and texture
 * coordinates, and sizes differ per call. The types ES allows and desktop does
 * not are widened before we get here (gles_needs_widen), so anything this
 * rejects is a genuine combination neither API accepts.
 *
 * Asking the driver instead -- call, then glGetError -- is what this replaces;
 * that answer costs a queue drain per array per draw.
 */
static bool gles_pointer_ok(GLenum client_state, uint32_t size, GLenum type)
{
    bool float_like = (type == GL_FLOAT || type == GL_DOUBLE);
    bool wide_int = (type == GL_SHORT || type == GL_INT);

    switch (client_state) {
    case GL_VERTEX_ARRAY:
        return (size >= 2 && size <= 4) && (float_like || wide_int);
    case GL_TEXTURE_COORD_ARRAY:
        return (size >= 1 && size <= 4) && (float_like || wide_int);
    case GL_COLOR_ARRAY:
        return (size == 3 || size == 4) &&
               (float_like || wide_int ||
                type == GL_BYTE || type == GL_UNSIGNED_BYTE ||
                type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT);
    case GL_NORMAL_ARRAY:
        /* glNormalPointer takes no size; normals are always 3-vectors. */
        return float_like || wide_int || type == GL_BYTE;
    default:
        return false;
    }
}

static bool gles_bind_array(CPUState *cpu, GLESArray *a, uint32_t first,
                            uint32_t count)
{
    uint32_t esz, stride;
    size_t need, off;
    const uint8_t *base;
    const void *data;
    GLenum type;

    /* A zero `ptr` is a legitimate offset into a bound buffer -- offset 0 is
     * where geometry usually starts -- so the null-pointer guard only applies
     * to the guest-VA case. */
    if (!a->enabled || (!a->ptr && !a->vbo) || !count) {
        return false;
    }
    esz = gles_type_size(a->type) * a->size;
    if (!esz) {
        return false;
    }
    stride = a->stride ? a->stride : esz;
    /* Last element still only occupies esz bytes, not a full stride. */
    need = (size_t)stride * (count - 1) + esz;
    off = (size_t)stride * first;

    if (a->vbo) {
        /*
         * The pointer was an offset into the buffer bound when gl*Pointer was
         * called, and the bytes are already here -- no guest read at all. This
         * is the only branch in the draw path that VBO support adds; the
         * guest-VA case below is untouched.
         */
        if (a->ptr + off + need > a->vbo->size) {
            static bool warned;

            if (!warned) {
                warned = true;
                fprintf(stderr, "[gles] %s array reads past its buffer "
                        "(offset %u + %zu, buffer %zu bytes); array disabled\n",
                        gles_array_name(a->client_state), a->ptr, off + need,
                        a->vbo->size);
            }
            return false;
        }
        base = a->vbo->data + a->ptr + off;
    } else {
        if (need > a->buf_size) {
            a->buf = g_realloc(a->buf, need);
            a->buf_size = need;
        }
        {
            uint64_t t0 = gles_t();
            int rc = cpu_memory_rw_debug(cpu, a->ptr + (hwaddr)off,
                                         a->buf, need, 0);
            gh.t_fetch += gles_t() - t0;
            if (rc != 0) {
                fprintf(stderr, "[gles] failed to read %zu bytes of array data "
                        "at guest 0x%08x\n", need, a->ptr);
                return false;
            }
        }
        base = a->buf;
    }

    data = base;
    type = a->type;

    /* Convert the types the host would refuse. See gles_needs_widen. */
    if (gles_needs_widen(a->client_state, type)) {
        size_t n = (size_t)count * a->size;
        uint32_t i, c;

        if (n > a->fbuf_count) {
            a->fbuf = g_realloc(a->fbuf, n * sizeof(float));
            a->fbuf_count = n;
        }
        for (i = 0; i < count; i++) {
            const uint8_t *row = base + (size_t)stride * i;
            for (c = 0; c < a->size; c++) {
                float v;
                if (type == GLES_FIXED) {
                    v = (float)((const int32_t *)row)[c] / 65536.0f;
                } else {
                    /* GL_BYTE positions are used unscaled, not normalised. */
                    v = (float)((const int8_t *)row)[c];
                }
                a->fbuf[(size_t)i * a->size + c] = v;
            }
        }
        data = a->fbuf;
        type = GL_FLOAT;
        stride = 0;   /* the converted copy is tightly packed */
    }

    /*
     * Decide up front whether the host will take this pointer, instead of
     * asking it afterwards. Same protection, no synchronisation: see the note
     * on IT_GLES_STRICT.
     */
    if (!gles_pointer_ok(a->client_state, a->size, type)) {
        static uint32_t reported;
        uint32_t key = (a->client_state << 16) ^ (a->size << 8) ^ (type & 0xff);

        if (reported != key) {
            reported = key;
            fprintf(stderr, "[gles] refusing %s pointer (size=%u type=0x%x) -- "
                    "not a combination desktop GL accepts; array disabled for "
                    "this draw\n", gles_array_name(a->client_state),
                    a->size, type);
        }
        return false;
    }

    glEnableClientState(a->client_state);
    if (gles_strict) {
        uint64_t t0 = gles_t();
        while (glGetError() != GL_NO_ERROR) {
            /* Drain, so the check below sees only this call's error. */
        }
        gh.t_err += gles_t() - t0;
    }
    switch (a->client_state) {
    case GL_VERTEX_ARRAY:
        glVertexPointer(a->size, type, stride, data);
        break;
    case GL_TEXTURE_COORD_ARRAY:
        glTexCoordPointer(a->size, type, stride, data);
        break;
    case GL_COLOR_ARRAY:
        glColorPointer(a->size, type, stride, data);
        break;
    case GL_NORMAL_ARRAY:
        /* glNormalPointer has no size argument; normals are always 3-vectors. */
        glNormalPointer(type, stride, data);
        break;
    default:
        glDisableClientState(a->client_state);
        return false;
    }

    /*
     * An array that is enabled but whose pointer the host refused is a loaded
     * gun: the driver dereferences NULL inside the next draw and takes QEMU
     * down with it, with no diagnostic naming the array. gles_pointer_ok above
     * is what now prevents that; this is the belt-and-braces version, kept for
     * bring-up because a combination we have not thought of would otherwise
     * reach the driver.
     */
    if (gles_strict) {
        uint64_t t0 = gles_t();
        GLenum e = glGetError();

        gh.t_err += gles_t() - t0;
        if (e != GL_NO_ERROR) {
            fprintf(stderr, "[gles] host refused %s pointer "
                    "(size=%u type=0x%x stride=%u): GL error 0x%x -- "
                    "array disabled for this draw\n",
                    gles_array_name(a->client_state),
                    a->size, type, stride, e);
            glDisableClientState(a->client_state);
            return false;
        }
    }
    return true;
}

/*
 * Bind every enabled client array for a draw covering elements [first, first+count).
 *
 * Returns a mask of which arrays were bound so the caller can unbind exactly
 * those. Leaving an array enabled across draws would make the next draw walk a
 * scratch buffer that has since been reallocated for a different array.
 */
static uint32_t gles_bind_all_arrays(CPUState *cpu, uint32_t first,
                                     uint32_t count)
{
    GLESArray *arrays[] = { &gh.vertex, &gh.texcoord, &gh.color, &gh.normal };
    uint32_t bound = 0;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(arrays); i++) {
        if (gles_bind_array(cpu, arrays[i], first, count)) {
            bound |= 1u << i;
        }
    }
    return bound;
}

/*
 * Bytes per texel for the ES 1.1 format/type pairs. The packed 16-bit types
 * override the format's component count -- 5_6_5 is GL_RGB but two bytes, not
 * three -- so type is checked second and wins.
 */
static size_t gles_texel_bytes(uint32_t fmt, uint32_t type)
{
    if (type == GL_UNSIGNED_SHORT_5_6_5 ||
        type == GL_UNSIGNED_SHORT_4_4_4_4 ||
        type == GL_UNSIGNED_SHORT_5_5_5_1) {
        return 2;
    }
    switch (fmt) {
    case GL_RGBA:            return 4;
    case GL_RGB:             return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_ALPHA:
    case GL_LUMINANCE:       return 1;
    default:                 return 4;
    }
}

/* GL_UNPACK_ALIGNMENT in force, defaulting to ES 1.1's 4 if the guest never
 * set it. */
static uint32_t gles_unpack(void)
{
    return gh.unpack_alignment ? gh.unpack_alignment : 4;
}

/*
 * How many bytes a w*h image of `bpp` texels occupies in guest memory under the
 * current GL_UNPACK_ALIGNMENT.
 *
 * The last row is deliberately NOT padded. GL only pads *between* rows, and the
 * guest is entitled to allocate exactly this much -- so fetching a padded final
 * row can run off the end of the allocation and into an unmapped page, which
 * fails the read and drops the whole texture. Under-reading shears the image;
 * over-reading loses it entirely.
 */
static size_t gles_image_bytes(uint32_t w, uint32_t h, size_t bpp)
{
    size_t align = gles_unpack();
    size_t row = ((size_t)w * bpp + align - 1) & ~(align - 1);

    if (!w || !h) {
        return 0;
    }
    return row * (h - 1) + (size_t)w * bpp;
}

/* ------------------------------------------------- compressed textures ---- */

/*
 * PVRTC and the paletted formats, decoded on the CPU.
 *
 * PVRTC was THE texture format on the PowerVR MBX, so a large share of the
 * 2008-2010 catalogue uploads nothing else -- and desktop CGL has no PVRTC at
 * all, nor does any GL this ever runs on outside iOS. So the choice is decode
 * it here or render those titles untextured.
 *
 * A WARNING ABOUT `.pvr` FILES, because it cost a wrong assumption once
 * already: a .pvr is a CONTAINER, not a format. Decoding the 52-byte PVR v2
 * headers in one shipping title's bundle found only four of its nine .pvr
 * assets were actually PVRTC; the other five were uncompressed RGBA4444 merely
 * stored in a .pvr. Nothing here may key off a file extension or an asset
 * pipeline's habits -- the only trustworthy signal is the `internalformat`
 * enum the guest passes, and that is what is switched on below.
 */
#define PVRTC_RGB_4BPP    0x8C00
#define PVRTC_RGB_2BPP    0x8C01
#define PVRTC_RGBA_4BPP   0x8C02
#define PVRTC_RGBA_2BPP   0x8C03
#define GLES_PALETTE_FIRST 0x8B90
#define GLES_PALETTE_LAST  0x8B99

static bool gles_is_pvrtc(uint32_t f)
{
    return f >= PVRTC_RGB_4BPP && f <= PVRTC_RGBA_2BPP;
}

static bool gles_is_paletted(uint32_t f)
{
    return f >= GLES_PALETTE_FIRST && f <= GLES_PALETTE_LAST;
}

/*
 * A PVRTC1 block's two endpoint colours, unpacked to 5:5:5:4.
 *
 * The two are NOT symmetric and that asymmetry is the format, not a typo:
 * colour A gets 14 bits (bit 0 is the modulation mode flag) so its blue channel
 * loses a bit, colour B gets 15. Each has an opaque flag that switches its
 * whole layout between RGB and ARGB. Channels are widened by bit replication,
 * which is what the hardware does.
 */
static void pvrtc_endpoints(uint32_t cw, int a[4], int b[4])
{
    if (cw & 0x8000) {                  /* A opaque: RGB 554 */
        a[0] = (cw & 0x7c00) >> 10;
        a[1] = (cw & 0x3e0) >> 5;
        a[2] = cw & 0x1e;
        a[2] |= a[2] >> 4;
        a[3] = 0xf;
    } else {                            /* A transparent: ARGB 3443 */
        a[0] = ((cw & 0xf00) >> 7) | ((cw & 0xf00) >> 11);
        a[1] = ((cw & 0xf0) >> 3) | ((cw & 0xf0) >> 7);
        a[2] = ((cw & 0xe) << 1) | ((cw & 0xe) >> 2);
        a[3] = (cw & 0x7000) >> 11;
    }
    if (cw & 0x80000000u) {             /* B opaque: RGB 555 */
        b[0] = (cw & 0x7c000000) >> 26;
        b[1] = (cw & 0x3e00000) >> 21;
        b[2] = (cw & 0x1f0000) >> 16;
        b[3] = 0xf;
    } else {                            /* B transparent: ARGB 3444 */
        b[0] = ((cw & 0xf000000) >> 23) | ((cw & 0xf000000) >> 27);
        b[1] = ((cw & 0xf00000) >> 19) | ((cw & 0xf00000) >> 23);
        b[2] = ((cw & 0xf0000) >> 15) | ((cw & 0xf0000) >> 19);
        b[3] = (cw & 0x70000000) >> 27;
    }
}

/*
 * Morton order over the block grid -- "twiddling", and the classic PVRTC bug.
 *
 * Blocks are NOT stored row-major. The x and y block indices are bit-interleaved
 * for as many bits as the SMALLER dimension has, and whatever is left of the
 * larger index is appended above. Getting this wrong does not produce a broken
 * image; it produces a plausible one with its 4x4 tiles shuffled, which is easy
 * to look at and not notice.
 */
static uint32_t pvrtc_twiddle(uint32_t bw, uint32_t bh, uint32_t bx, uint32_t by)
{
    uint32_t min_dim = bw < bh ? bw : bh;
    /* The leftover high bits come from the index along the LARGER dimension --
     * the one with bits the interleave could not pair up. Taking the smaller
     * one instead is invisible on a square texture (both indices have the same
     * bit count, so the leftover is zero either way) and shuffles every
     * non-square one, which is the classic way to get this wrong. */
    uint32_t max_val = bw < bh ? by : bx;
    uint32_t twiddled = 0, src = 1, dst = 1;
    int shift = 0;

    while (src < min_dim) {
        if (by & src) {
            twiddled |= dst;
        }
        if (bx & src) {
            twiddled |= dst << 1;
        }
        src <<= 1;
        dst <<= 2;
        shift++;
    }
    return twiddled | ((max_val >> shift) << (2 * shift));
}

/* 5-bit colour and 4-bit alpha, widened to 8 bits from a weighted sum whose
 * denominator is `den`. One rounding step, not two. */
static inline int pvrtc_to8(int num, int den, int bits)
{
    int maxv = (1 << bits) - 1;

    return (num * 255 + den * maxv / 2) / (den * maxv);
}

/*
 * Decode a PVRTC1 image to RGBA8. `bpp` is 2 or 4.
 *
 * The shape of the format: each 8-byte block holds two endpoint colours for a
 * 4x4 (4bpp) or 8x4 (2bpp) region plus per-texel modulation weights. The
 * endpoints are bilinearly interpolated ACROSS blocks -- a texel's colour comes
 * from the four nearest block centres, wrapping at the edges -- and the
 * modulation then picks a point on the line between the interpolated A and B.
 * Block centres sit at (cw/2, ch/2) within each block, which is why every
 * lookup below is offset by half a block.
 */
static void pvrtc_decode(const uint8_t *src, uint32_t w, uint32_t h, int bpp,
                         uint8_t *dst)
{
    const uint32_t cw = (bpp == 2) ? 8 : 4, ch = 4;
    /*
     * At least one block each way. The tail of a mip chain is smaller than a
     * block -- a 2x2 and a 1x1 level are each still one whole 8-byte block --
     * and dropping those levels leaves the chain incomplete, which
     * fixed-function GL renders as solid white. That is exactly what a real
     * title's upload looked like before this clamp existed.
     */
    const uint32_t bw = w / cw ? w / cw : 1, bh = h / ch ? h / ch : 1;
    /* Modulation weight in eighths. The second table is the punch-through
     * mode; 14 is 4 with a flag meaning "and force alpha to zero". */
    static const int mod0[4] = { 0, 3, 5, 8 };
    static const int mod1[4] = { 0, 4, 14, 8 };
    uint32_t px, py;

    if (!bw || !bh) {
        return;
    }
    for (py = 0; py < h; py++) {
        for (px = 0; px < w; px++) {
            /* The four block centres this texel sits between, and its position
             * between them. Both indices wrap: PVRTC textures tile. */
            uint32_t fx = (px + cw - cw / 2) % cw;
            uint32_t fy = (py + ch - ch / 2) % ch;
            uint32_t bx0 = ((px + cw * bw - cw / 2) / cw) % bw;
            uint32_t by0 = ((py + ch * bh - ch / 2) / ch) % bh;
            uint32_t bx1 = (bx0 + 1) % bw, by1 = (by0 + 1) % bh;
            uint32_t wx1 = fx, wx0 = cw - fx;
            uint32_t wy1 = fy, wy0 = ch - fy;
            uint32_t den = cw * ch;
            /* The block that spatially CONTAINS this texel owns its modulation
             * -- a different block from any of the four above, in general. */
            const uint8_t *mb = src + (size_t)pvrtc_twiddle(bw, bh,
                                                            px / cw, py / ch) * 8;
            uint32_t mbits = ldl_le_p(mb), mcolor = ldl_le_p(mb + 4);
            uint32_t lx = px % cw, ly = py % ch;
            int wsum[4] = { wx0 * wy0, wx1 * wy0, wx0 * wy1, wx1 * wy1 };
            uint32_t corner[4] = {
                pvrtc_twiddle(bw, bh, bx0, by0), pvrtc_twiddle(bw, bh, bx1, by0),
                pvrtc_twiddle(bw, bh, bx0, by1), pvrtc_twiddle(bw, bh, bx1, by1),
            };
            int acc_a[4] = { 0, 0, 0, 0 }, acc_b[4] = { 0, 0, 0, 0 };
            int a8[4], b8[4], mod, i, c;
            bool punch = false;
            uint8_t *out = dst + ((size_t)py * w + px) * 4;

            for (i = 0; i < 4; i++) {
                int ea[4], eb[4];

                pvrtc_endpoints(ldl_le_p(src + (size_t)corner[i] * 8 + 4),
                                ea, eb);
                for (c = 0; c < 4; c++) {
                    acc_a[c] += ea[c] * wsum[i];
                    acc_b[c] += eb[c] * wsum[i];
                }
            }
            for (c = 0; c < 4; c++) {
                a8[c] = pvrtc_to8(acc_a[c], den, c == 3 ? 4 : 5);
                b8[c] = pvrtc_to8(acc_b[c], den, c == 3 ? 4 : 5);
            }

            if (bpp == 4) {
                mod = (mcolor & 1) ? mod1[(mbits >> (2 * (4 * ly + lx))) & 3]
                                   : mod0[(mbits >> (2 * (4 * ly + lx))) & 3];
            } else {
                /*
                 * ponytail: 2bpp local-modulation (the mode-1 encodings, where
                 * half the texels carry explicit weights and the rest are
                 * averaged from their neighbours) is NOT decoded -- those
                 * blocks come out as the midpoint of the two endpoints. 2bpp is
                 * absent from every title surveyed here, and the mode-0 path
                 * below is the whole of the format that anything is known to
                 * use. Implement the H/V/checkerboard cases if a title turns up
                 * that needs them; the warning below says when.
                 */
                if (mcolor & 1) {
                    static bool warned;

                    if (!warned) {
                        warned = true;
                        fprintf(stderr, "[gles] PVRTC 2bpp local-modulation "
                                "blocks are approximated (flat A/B midpoint) -- "
                                "this texture will look soft\n");
                    }
                    mod = 4;
                } else {
                    mod = ((mbits >> (8 * ly + lx)) & 1) ? 8 : 0;
                }
            }
            if (mod > 10) {
                punch = true;
                mod -= 10;
            }
            for (c = 0; c < 3; c++) {
                out[c] = (a8[c] * (8 - mod) + b8[c] * mod) / 8;
            }
            out[3] = punch ? 0 : (a8[3] * (8 - mod) + b8[3] * mod) / 8;
        }
    }
}

/*
 * Bytes a PVRTC1 image of this size occupies, or 0 if the size is not one the
 * format can express.
 */
static size_t pvrtc_size(uint32_t w, uint32_t h, int bpp)
{
    uint32_t cw = (bpp == 2) ? 8 : 4;
    uint32_t bw, bh;

    if (!w || !h || (w & (w - 1)) || (h & (h - 1))) {
        return 0;
    }
    /* Levels below one block still occupy a whole block. See pvrtc_decode. */
    bw = w / cw ? w / cw : 1;
    bh = h / 4 ? h / 4 : 1;
    return (size_t)bw * bh * 8;
}

/*
 * The paletted formats: a palette of 16 or 256 entries followed by the index
 * data for EVERY mip level, back to back. Cheap next to PVRTC -- a lookup and a
 * channel widening -- and worth having in the same pass because a title that
 * ships one usually ships no uncompressed fallback.
 */
static bool gles_palette_info(uint32_t fmt, unsigned *idx_bits,
                              unsigned *entry_bytes, uint32_t *type)
{
    if (!gles_is_paletted(fmt)) {
        return false;
    }
    *idx_bits = (fmt <= 0x8B94) ? 4 : 8;
    switch ((fmt - GLES_PALETTE_FIRST) % 5) {
    case 0: *entry_bytes = 3; *type = GL_RGB;  break;   /* RGB8    */
    case 1: *entry_bytes = 4; *type = GL_RGBA; break;   /* RGBA8   */
    case 2: *entry_bytes = 2; *type = GL_UNSIGNED_SHORT_5_6_5;   break;
    case 3: *entry_bytes = 2; *type = GL_UNSIGNED_SHORT_4_4_4_4; break;
    default:*entry_bytes = 2; *type = GL_UNSIGNED_SHORT_5_5_5_1; break;
    }
    return true;
}

static void gles_palette_entry(const uint8_t *e, uint32_t type, uint8_t out[4])
{
    unsigned v;

    switch (type) {
    case GL_RGB:
        out[0] = e[0]; out[1] = e[1]; out[2] = e[2]; out[3] = 0xff;
        return;
    case GL_RGBA:
        out[0] = e[0]; out[1] = e[1]; out[2] = e[2]; out[3] = e[3];
        return;
    default:
        break;
    }
    /* The 16-bit entries are big-endian in the compressed-paletted spec: the
     * data is a byte stream, not host shorts. */
    v = ((unsigned)e[0] << 8) | e[1];
    if (type == GL_UNSIGNED_SHORT_5_6_5) {
        out[0] = (v >> 11) * 255 / 31;
        out[1] = ((v >> 5) & 0x3f) * 255 / 63;
        out[2] = (v & 0x1f) * 255 / 31;
        out[3] = 0xff;
    } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
        out[0] = (v >> 12) * 17;
        out[1] = ((v >> 8) & 0xf) * 17;
        out[2] = ((v >> 4) & 0xf) * 17;
        out[3] = (v & 0xf) * 17;
    } else {                                   /* 5_5_5_1 */
        out[0] = (v >> 11) * 255 / 31;
        out[1] = ((v >> 6) & 0x1f) * 255 / 31;
        out[2] = ((v >> 1) & 0x1f) * 255 / 31;
        out[3] = (v & 1) ? 0xff : 0;
    }
}

/*
 * Decode one mip level of paletted index data to RGBA8. `idx` points at that
 * level's indices; returns how many index bytes it consumed.
 */
static size_t gles_palette_level(const uint8_t *pal, uint32_t type,
                                 const uint8_t *idx, unsigned idx_bits,
                                 unsigned entry_bytes, uint32_t w, uint32_t h,
                                 uint8_t *dst)
{
    size_t n = (size_t)w * h, i;

    for (i = 0; i < n; i++) {
        unsigned e = (idx_bits == 8) ? idx[i]
                                     : ((i & 1) ? (idx[i / 2] & 0xf)
                                                : (idx[i / 2] >> 4));

        gles_palette_entry(pal + (size_t)e * entry_bytes, type, dst + i * 4);
    }
    return (idx_bits == 8) ? n : (n + 1) / 2;
}

/*
 * Say what a decode actually produced, once per distinct format and size.
 *
 * "The texture is wrong" and "the texture never arrived" look identical from
 * the panel -- both render as a flat fill -- and this layer can produce either.
 * The mean channel values separate them at a glance: real art is never flat,
 * and a decoder that has silently degenerated reports 255,255,255 here while
 * the app looks like it is drawing nothing. Bounded to one line per format and
 * size, so a mip chain costs a handful of lines at load time and nothing after.
 */
static void gles_report_decode(uint32_t fmt, uint32_t w, uint32_t h,
                               const uint8_t *rgba)
{
    static struct { uint32_t fmt, w, h; } seen[32];
    static unsigned n_seen;
    uint64_t sum[4] = { 0, 0, 0, 0 };
    size_t n = (size_t)w * h, i;
    unsigned c;

    for (i = 0; i < n_seen; i++) {
        if (seen[i].fmt == fmt && seen[i].w == w && seen[i].h == h) {
            return;
        }
    }
    if (n_seen == ARRAY_SIZE(seen)) {
        return;
    }
    seen[n_seen++] = (typeof(seen[0])){ fmt, w, h };

    for (i = 0; i < n; i++) {
        for (c = 0; c < 4; c++) {
            sum[c] += rgba[i * 4 + c];
        }
    }
    fprintf(stderr, "[gles] decoded 0x%x %ux%u -> mean rgba %llu,%llu,%llu,%llu"
            "%s\n", fmt, w, h,
            (unsigned long long)(sum[0] / n), (unsigned long long)(sum[1] / n),
            (unsigned long long)(sum[2] / n), (unsigned long long)(sum[3] / n),
            (sum[0] / n == 255 && sum[1] / n == 255 && sum[2] / n == 255)
                ? "  (FLAT WHITE -- decode produced nothing)" : "");
}

/*
 * Grow-and-keep staging for the decoded RGBA8.
 */
static uint8_t *gles_decode_buf(size_t n)
{
    if (n > GLES_MAX_TEX_BYTES) {
        return NULL;
    }
    if (n > gh.decbuf_size) {
        gh.decbuf = g_realloc(gh.decbuf, n);
        gh.decbuf_size = n;
    }
    return gh.decbuf;
}

/*
 * Decode a known image and check the pixels, once, at startup.
 *
 * This exists because every failure mode of this decoder is a picture: a
 * transposed twiddle, a swapped endpoint, an off-by-one modulation table all
 * produce something that renders, and none of them produce an error. Without a
 * check that names the expected bytes, a refactor can break the format
 * silently and the only symptom is that one game's art looks wrong -- which is
 * exactly the report nobody can act on.
 *
 * The fixture is a 2x2 block grid (8x8 texels) whose four blocks are flat
 * white, green, red and black. Reading the four block centres back therefore
 * tests the endpoint unpack, the interpolation weights (a centre must land on
 * its own block's colour exactly, with zero bleed) and the twiddle order --
 * green and red are placed so that a row-major block order swaps them.
 */
static void pvrtc_selfcheck(void)
{
    /* Opaque colour A, in the A-opaque layout: bit15 set, RGB 5:5:4. */
    static const struct { uint32_t cw; uint8_t rgb[3]; } blocks[4] = {
        { 0x8000u | (31 << 10) | (31 << 5) | 0x1e, { 255, 255, 255 } },
        { 0x8000u | (0 << 10)  | (31 << 5) | 0x00, {   0, 255,   0 } },
        { 0x8000u | (31 << 10) | (0 << 5)  | 0x00, { 255,   0,   0 } },
        { 0x8000u | (0 << 10)  | (0 << 5)  | 0x00, {   0,   0,   0 } },
    };
    /* Twiddled block order for a 2x2 grid: index = by | (bx << 1). Centres are
     * at texel (4*bx + 2, 4*by + 2). */
    static const struct { uint32_t x, y; unsigned block; } probes[4] = {
        { 2, 2, 0 },  /* bx=0 by=0 -> word 0 */
        { 6, 2, 2 },  /* bx=1 by=0 -> word 2, NOT word 1 */
        { 2, 6, 1 },  /* bx=0 by=1 -> word 1 */
        { 6, 6, 3 },
    };
    uint8_t src[4 * 8], out[8 * 8 * 4];
    unsigned i, c;
    bool ok = true;

    memset(src, 0, sizeof(src));
    for (i = 0; i < 4; i++) {
        /* Modulation all zero -> every texel is colour A. */
        stl_le_p(src + i * 8 + 4, blocks[i].cw);
    }
    /* Words are laid out in twiddled order, so word i is block i by
     * construction: probe[k].block is the word index to expect. */
    pvrtc_decode(src, 8, 8, 4, out);

    for (i = 0; i < 4; i++) {
        const uint8_t *p = out + ((size_t)probes[i].y * 8 + probes[i].x) * 4;
        const uint8_t *want = blocks[probes[i].block].rgb;

        for (c = 0; c < 3; c++) {
            if (p[c] != want[c]) {
                ok = false;
            }
        }
        if (p[3] != 255) {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "[gles] PVRTC SELF-CHECK FAILED at texel (%u,%u): "
                    "got %u,%u,%u,%u want %u,%u,%u,255 -- the decoder is "
                    "broken, textures will be wrong\n",
                    probes[i].x, probes[i].y, p[0], p[1], p[2], p[3],
                    want[0], want[1], want[2]);
            return;
        }
    }

    /*
     * And the twiddle over NON-SQUARE grids, which the fixture above cannot
     * reach. On a square grid both block indices have the same bit count, so
     * taking the leftover high bits from the wrong one is a no-op -- the bug
     * hides completely until a 256x128 atlas turns up and comes back shuffled.
     * A bijection check is the cheap invariant that catches it: every block
     * index must map to a distinct word inside the image.
     */
    {
        static const uint8_t dims[][2] = { { 8, 2 }, { 2, 8 }, { 16, 4 } };
        unsigned d;

        for (d = 0; d < ARRAY_SIZE(dims); d++) {
            uint32_t bw = dims[d][0], bh = dims[d][1], x, y;
            uint64_t seen = 0;

            for (y = 0; y < bh; y++) {
                for (x = 0; x < bw; x++) {
                    uint32_t t = pvrtc_twiddle(bw, bh, x, y);

                    if (t >= bw * bh || (seen & (1ull << t))) {
                        fprintf(stderr, "[gles] PVRTC SELF-CHECK FAILED: "
                                "twiddle %ux%u block (%u,%u) -> word %u, which "
                                "is %s -- non-square textures will be "
                                "shuffled\n", bw, bh, x, y, t,
                                t >= bw * bh ? "out of range" : "already used");
                        return;
                    }
                    seen |= 1ull << t;
                }
            }
        }
    }
}

/*
 * Fetch `n` bytes of guest pixel data into the reusable texture staging buffer.
 * Returns the buffer, or NULL on an out-of-range size or a failed read (the
 * caller skips the upload). Grows the buffer and keeps it, so a per-frame
 * streaming upload does not allocate.
 */
/*
 * `n` zero bytes, for a texture the guest allocated without contents.
 *
 * Grow-and-keep like the other staging buffers, but this one has to be cleared
 * every time: it is handed to GL as image data, and a previous, larger upload
 * would otherwise show through as the tail of the new one.
 */
static const uint8_t *gles_zeroed(size_t n)
{
    if (n > GLES_MAX_TEX_BYTES) {
        fprintf(stderr, "[gles] zero-fill of %zu bytes exceeds cap; dropped\n",
                n);
        return NULL;
    }
    if (n > gh.zerobuf_size) {
        gh.zerobuf = g_realloc(gh.zerobuf, n);
        gh.zerobuf_size = n;
    }
    memset(gh.zerobuf, 0, n);
    return gh.zerobuf;
}

static const uint8_t *gles_fetch_texels(CPUState *cpu, uint32_t pixels,
                                        size_t n, const char *who)
{
    if (n > GLES_MAX_TEX_BYTES) {
        fprintf(stderr, "[gles] %s: %zu-byte upload exceeds cap; dropped\n",
                who, n);
        return NULL;
    }
    if (n > gh.txbuf_size) {
        gh.txbuf = g_realloc(gh.txbuf, n);
        gh.txbuf_size = n;
    }
    if (cpu_memory_rw_debug(cpu, pixels, gh.txbuf, n, 0) != 0) {
        fprintf(stderr, "[gles] %s: cannot read %zu bytes at guest 0x%08x\n",
                who, n, pixels);
        return NULL;
    }
    return gh.txbuf;
}

/*
 * Report a draw the host rejected, once per distinct error.
 *
 * A rejected draw is silent: the frame simply comes out missing whatever that
 * call would have contributed, which is indistinguishable from the app not
 * having drawn it. Cube Runner renders its obstacles with one glDrawElements
 * each, so "no cubes on screen" and "every cube draw returned GL_INVALID_ENUM"
 * look exactly the same from outside.
 */
static void gles_check_draw(const char *what, uint32_t mode, uint32_t count)
{
    static GLenum reported[8];
    static unsigned n_reported;
    GLenum e;
    unsigned i;
    uint64_t t0;

    /* See IT_GLES_STRICT: this is a queue drain, once per draw. */
    if (!gles_strict) {
        return;
    }
    t0 = gles_t();
    e = glGetError();
    gh.t_err += gles_t() - t0;

    if (e == GL_NO_ERROR) {
        return;
    }
    for (i = 0; i < n_reported; i++) {
        if (reported[i] == e) {
            return;
        }
    }
    if (n_reported < ARRAY_SIZE(reported)) {
        reported[n_reported++] = e;
    }
    fprintf(stderr, "[gles] %s(mode=0x%x, count=%u) -> GL error 0x%x\n",
            what, mode, count, e);
}

/*
 * Log one frame's worth of draws, in order, with the state that decides
 * whether each one is visible.
 *
 * Order is the thing a state dump cannot show. Cube Runner's gameplay and its
 * title flythrough have byte-identical GL state at end of frame and submit
 * about the same number of cube draws, yet only the title screen shows any --
 * so the difference has to be in what is drawn when, and against what depth
 * state. The gameplay screen is two flat colour bands, which is what two
 * full-screen quads drawn last would look like.
 */

/*
 * The first few POSITIONS this draw will actually use, decoded out of the
 * buffer we fetched from guest memory.
 *
 * Every previous trace printed the modelview translation, which is where the
 * cube was PUT -- not what it is made of. A cube whose fetched vertices are
 * degenerate collapses to nothing while its translation still reads as a
 * perfectly sensible position, so the placement always looked innocent.
 * Fetching these arrays correctly is our job, and a wrong stride, offset, type
 * or pointer on one code path is exactly the defect this layer can produce and
 * then report success for.
 */
static void gles_trace_vertices(void)
{
    const GLESArray *a = &gh.vertex;
    unsigned i, c, n = 3;
    float lo[4], hi[4];

    if (!a->enabled || !a->size) {
        fprintf(stderr, "[gles]     positions: NO VERTEX ARRAY BOUND\n");
        return;
    }
    /*
     * A VBO-sourced array never fills the guest-VA scratch -- its bytes come
     * straight from the stored buffer -- so printing a->buf here would show
     * whatever the last client-array draw left behind. That is a trace that
     * lies, and this trace exists precisely to settle "did the geometry arrive
     * correctly", so it has to read the source the draw actually used.
     */
    if (a->vbo) {
        if (a->ptr >= a->vbo->size) {
            fprintf(stderr, "[gles]     positions: VBO offset %u past a "
                    "%zu-byte buffer\n", a->ptr, a->vbo->size);
            return;
        }
    } else if (!a->buf) {
        fprintf(stderr, "[gles]     positions: NO VERTEX ARRAY BOUND\n");
        return;
    }
    for (c = 0; c < a->size && c < 4; c++) {
        lo[c] = 1e30f;
        hi[c] = -1e30f;
    }
    fprintf(stderr, "[gles]     positions type=0x%x size=%u stride=%u ptr=0x%08x:",
            a->type, a->size, a->stride, a->ptr);
    for (i = 0; i < n; i++) {
        uint32_t stride = a->stride ? a->stride
                                    : gles_type_size(a->type) * a->size;
        const uint8_t *src = a->vbo ? a->vbo->data + a->ptr : a->buf;
        size_t avail = a->vbo ? a->vbo->size - a->ptr : a->buf_size;
        const uint8_t *row = src + (size_t)stride * i;

        if ((size_t)stride * i + stride > avail) {
            break;
        }
        fprintf(stderr, " (");
        for (c = 0; c < a->size && c < 4; c++) {
            float v;

            switch (a->type) {
            case GL_FLOAT:  v = ((const float *)row)[c];               break;
            case GL_BYTE:   v = ((const int8_t *)row)[c];              break;
            case GL_SHORT:  v = ((const int16_t *)row)[c];             break;
            case GLES_FIXED: v = ((const int32_t *)row)[c] / 65536.0f; break;
            default:        v = 0;                                     break;
            }
            fprintf(stderr, "%s%.3f", c ? " " : "", v);
            if (v < lo[c]) {
                lo[c] = v;
            }
            if (v > hi[c]) {
                hi[c] = v;
            }
        }
        fprintf(stderr, ")");
    }
    /* A cube that fetched correctly spans a real extent on every axis; one
     * that fetched zeros spans nothing and draws nothing. */
    fprintf(stderr, "  extent=(");
    for (c = 0; c < a->size && c < 4; c++) {
        fprintf(stderr, "%s%.3f", c ? " " : "", hi[c] - lo[c]);
    }
    fprintf(stderr, ")\n");
}

static void gles_trace_draw(const char *what, uint32_t mode, uint32_t count)
{
    GLfloat mv[16], cur_col[4], mat_dif[4], line_width = 0;
    GLboolean depth_mask = 0;
    GLint src = 0, dst = 0;

    if (gh.trace_draws <= 0) {
        return;
    }
    gh.trace_draws--;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    /*
     * The state that decides whether a LINE is visible is not the set that
     * decides it for a triangle, and this app splits cleanly along that line:
     * its title flythrough is 100% GL_TRIANGLES and renders, its gameplay
     * obstacles are 100% GL_LINE_STRIP and do not. A zero line width, a zero
     * alpha under an enabled blend, or a colour array that is off for these
     * draws would each produce exactly nothing, and none of the three is
     * visible in the geometry -- which is why the geometry looked innocent for
     * so long.
     */
    glGetFloatv(GL_LINE_WIDTH, &line_width);
    glGetFloatv(GL_CURRENT_COLOR, cur_col);
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, mat_dif);
    glGetIntegerv(GL_BLEND_SRC, &src);
    glGetIntegerv(GL_BLEND_DST, &dst);
    fprintf(stderr, "[gles]   f%" PRIu64 " draw %-14s mode=0x%x count=%-4u depthmask=%d "
            "depthtest=%d xyz=(%.2f %.2f %.2f)\n"
            "[gles]     linewidth=%.2f blend=%d(src=0x%x dst=0x%x) "
            "colour=(%.2f %.2f %.2f %.2f) matdiffuse=(%.2f %.2f %.2f %.2f) "
            "arrays vtx=%u col=%u nrm=%u tex=%u lighting=%d\n",
            gh.presents, what, mode, count, depth_mask,
            glIsEnabled(GL_DEPTH_TEST), mv[12], mv[13], mv[14],
            line_width, glIsEnabled(GL_BLEND), (unsigned)src, (unsigned)dst,
            cur_col[0], cur_col[1], cur_col[2], cur_col[3],
            mat_dif[0], mat_dif[1], mat_dif[2], mat_dif[3],
            gh.vertex.enabled, gh.color.enabled, gh.normal.enabled,
            gh.texcoord.enabled, glIsEnabled(GL_LIGHTING));
    /*
     * The PROJECTION in force for THIS draw, not the one left at end of frame.
     * The end-of-frame state dump always caught the 2D HUD's ortho, so a broken
     * perspective matrix for the 3D pass could never be seen -- and geometry
     * that is transformed to nothing looks exactly like geometry that was never
     * submitted. m[11] tells the two projections apart at a glance: -1 for a
     * frustum, 0 for an ortho.
     */
    {
        float pr[16];
        glGetFloatv(GL_PROJECTION_MATRIX, pr);
        fprintf(stderr, "[gles]     projection diag=(%.4f %.4f %.4f) "
                "m[11]=%.2f m[14]=%.3f viewport-ok\n",
                pr[0], pr[5], pr[10], pr[11], pr[14]);
    }
    gles_trace_vertices();
}

static void gles_unbind_arrays(uint32_t bound)
{
    GLESArray *arrays[] = { &gh.vertex, &gh.texcoord, &gh.color, &gh.normal };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(arrays); i++) {
        if (bound & (1u << i)) {
            glDisableClientState(arrays[i]->client_state);
        }
    }
}

/*
 * Read a small run of floats out of guest memory -- the argument of the *fv
 * setters (glLightfv, glMaterialfv, glFogfv) and of glMultMatrixf.
 *
 * Returns NULL if the guest pointer is unreadable, and the caller drops the
 * call rather than applying whatever was left in the buffer.
 */
static const float *gles_fetch_floats(CPUState *cpu, uint32_t ptr, unsigned n,
                                      float *out)
{
    if (!ptr || !n) {
        return NULL;
    }
    if (cpu_memory_rw_debug(cpu, ptr, (uint8_t *)out, n * sizeof(float), 0)
        != 0) {
        fprintf(stderr, "[gles] cannot read %u floats at guest 0x%08x\n",
                n, ptr);
        return NULL;
    }
    return out;
}

/*
 * How many floats a *fv parameter carries. GL says the count depends on the
 * pname, and reading four where the guest allocated one walks off the end of
 * its buffer, so this is not a detail that can be rounded up to 4.
 */
static unsigned gles_light_nparams(uint32_t pname)
{
    switch (pname) {
    case GL_AMBIENT:                /* 0x1200 */
    case GL_DIFFUSE:                /* 0x1201 */
    case GL_SPECULAR:               /* 0x1202 */
    case GL_POSITION:               /* 0x1203 */
        return 4;
    case GL_SPOT_DIRECTION:         /* 0x1204 */
        return 3;
    default:
        return 1;                   /* the scalar spot/attenuation parameters */
    }
}

static unsigned gles_material_nparams(uint32_t pname)
{
    switch (pname) {
    case GL_AMBIENT:
    case GL_DIFFUSE:
    case GL_SPECULAR:
    case GL_EMISSION:               /* 0x1600 */
    case GL_AMBIENT_AND_DIFFUSE:    /* 0x1602 */
        return 4;
    default:
        return 1;                   /* GL_SHININESS */
    }
}

/* ------------------------------------------------------------------ present */

/*
 * Read the rendered frame back and put it where the panel will scan it out.
 *
 * This is the debug present path, not the shipping one: the real engine renders
 * into a CoreAnimation-allocated IOSurface and CA composites it. Writing
 * straight to w1_framebuffer_base bypasses CA entirely, so whatever CA draws
 * next will overwrite it. It exists because it makes the pixel path verifiable
 * on its own -- guest issues GL, host renders, pixels appear on the panel --
 * without first having to get the whole MBXGLEngine bundle ABI right.
 *
 * GL's origin is bottom-left and the framebuffer's is top-left, hence the row
 * flip. The panel is BGRA (see lcd_refresh_rotated), GL gives us RGBA.
 */
/*
 * Make this frame's pixels readable by the CPU, or say the fast path is not
 * available. Rows run bottom-up and are BGRA, which is what both destinations
 * want; `stride` is the surface's, not the frame's, so callers must use it.
 *
 * THE SYNCHRONISATION, in one place because it is the only part of this that
 * can be silently wrong. Two things stand between the guest and a half-drawn
 * frame:
 *
 *   glFlush   submits the frame's commands. It does not wait -- that is the
 *             whole point -- but a lock can only wait for work the driver has
 *             been handed, so without this there is a window where the GPU has
 *             nothing queued and the lock has nothing to wait for.
 *   the lock  IOSurfaceLock, without kIOSurfaceLockAvoidSync, is defined to
 *             perform "a potentially expensive paging operation (such as
 *             readback from a GPU to system memory)" -- that flag exists to
 *             let a caller REFUSE that wait. Taking the lock is therefore
 *             taking the wait, scoped to this surface rather than to every
 *             queue in the context the way glFinish is.
 *
 * What is gone is the DOUBLE wait the old path took: a glFinish that drained
 * the pipeline, followed by a glReadPixels that resolved the tiles a second
 * time into driver staging. What remains is one wait for one surface's GPU
 * work, which is not negotiable while the guest's present is synchronous --
 * CoreAnimation composites the buffer as soon as the request returns, so
 * handing it pixels the GPU has not finished writing would be a torn frame.
 */
static const uint8_t *gles_frame_lock(size_t *stride)
{
    if (!gh.iosurface) {
        return NULL;
    }
    glFlush();
    return gles_platform_frame_lock(stride);
}

static void gles_present_to_panel(void)
{
    IPodTouchMachineState *nms;
    hwaddr fb;
    const uint8_t *frame;
    size_t fstride = 0;
    int y;

    nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
    if (!nms || !nms->lcd_state) {
        return;
    }
    fb = nms->lcd_state->w1_framebuffer_base;
    if (!fb) {
        fprintf(stderr, "[gles] present: no framebuffer base yet\n");
        return;
    }

    /* The surface is already in the panel's byte order, so the row is a copy
     * and the flip is the only work left. */
    frame = gles_frame_lock(&fstride);
    if (frame) {
        for (y = 0; y < GLES_FB_HEIGHT; y++) {
            const uint8_t *src = frame + (size_t)(GLES_FB_HEIGHT - 1 - y)
                                 * fstride;

            cpu_physical_memory_write(fb + (hwaddr)y * GLES_FB_WIDTH * 4,
                                      src, GLES_FB_WIDTH * 4);
        }
        gles_platform_frame_unlock();
        gh.presents++;
        return;
    }

    /* Read the DRAWABLE, whatever the guest happens to have bound. Now that
     * offscreen framebuffers are real, "the bound FBO" and "the frame we
     * present" are no longer the same thing, and a guest that presents with
     * its render-to-texture target still bound would otherwise have that
     * texture scanned out to the panel. */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gh.fbo);
    glFinish();
    glReadPixels(0, 0, GLES_FB_WIDTH, GLES_FB_HEIGHT, GL_RGBA,
                 GL_UNSIGNED_BYTE, gh.readback);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,
                         gles_host_fbo(gh.bound_framebuffer));

    for (y = 0; y < GLES_FB_HEIGHT; y++) {
        uint8_t row[GLES_FB_WIDTH * 4];
        const uint8_t *src =
            gh.readback + (size_t)(GLES_FB_HEIGHT - 1 - y) * GLES_FB_WIDTH * 4;
        int x;

        for (x = 0; x < GLES_FB_WIDTH; x++) {
            row[x * 4 + 0] = src[x * 4 + 2];  /* B */
            row[x * 4 + 1] = src[x * 4 + 1];  /* G */
            row[x * 4 + 2] = src[x * 4 + 0];  /* R */
            row[x * 4 + 3] = src[x * 4 + 3];  /* A */
        }
        cpu_physical_memory_write(fb + (hwaddr)y * GLES_FB_WIDTH * 4,
                                  row, sizeof(row));
    }
    gh.presents++;
}


/*
 * Write the frame we just read back as a PPM, when its draws were traced.
 *
 * The picture and the draw list then come from the same instant, which is the
 * only way to answer "was this geometry actually invisible" rather than
 * inferring it from a screenshot taken at some other moment.
 */
static void gles_dump_frame(const uint8_t *frame, size_t stride,
                            uint32_t rw, uint32_t rh, bool bgra)
{
    const char *dir = getenv("IT_GLES_DUMP_DIR");
    char path[1024];
    FILE *f;
    uint32_t x, y;

    if (!dir || !gh.dump_pending) {
        return;
    }
    gh.dump_pending = false;
    snprintf(path, sizeof(path), "%s/frame-%06" PRIu64 ".ppm", dir, gh.presents);
    f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", rw, rh);
    for (y = 0; y < rh; y++) {
        /* GL's origin is bottom-left; PPM's is top-left. */
        const uint8_t *src = frame + (size_t)(rh - 1 - y) * stride;

        for (x = 0; x < rw; x++) {
            uint8_t rgb[3];

            if (bgra) {
                rgb[0] = src[x * 4 + 2];
                rgb[1] = src[x * 4 + 1];
                rgb[2] = src[x * 4 + 0];
            } else {
                rgb[0] = src[x * 4 + 0];
                rgb[1] = src[x * 4 + 1];
                rgb[2] = src[x * 4 + 2];
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[gles] wrote traced frame %" PRIu64 " to %s\n",
            gh.presents, path);

    /*
     * And the PANEL, at the same instant.
     *
     * Everything above is what this layer PRODUCED. What the user sees is what
     * CoreAnimation composited into the scanout buffer, and the two have never
     * been compared at the same moment -- the closest anyone got was a GL frame
     * and a screendump seconds apart, which is how "the obstacles are never
     * drawn" survived three sessions. Reading the panel here makes the pair
     * simultaneous by construction.
     */
    {
        IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
        hwaddr fb;
        g_autofree uint8_t *panel = NULL;

        if (!nms || !nms->lcd_state) {
            return;
        }
        fb = nms->lcd_state->scanout_base ? nms->lcd_state->scanout_base
                                          : nms->lcd_state->w1_framebuffer_base;
        if (!fb) {
            return;
        }
        panel = g_malloc((size_t)GLES_FB_WIDTH * GLES_FB_HEIGHT * 4);
        cpu_physical_memory_read(fb, panel,
                                 (size_t)GLES_FB_WIDTH * GLES_FB_HEIGHT * 4);
        snprintf(path, sizeof(path), "%s/panel-%06" PRIu64 ".ppm",
                 dir, gh.presents);
        f = fopen(path, "wb");
        if (!f) {
            return;
        }
        fprintf(f, "P6\n%u %u\n255\n", GLES_FB_WIDTH, GLES_FB_HEIGHT);
        for (y = 0; y < GLES_FB_HEIGHT; y++) {
            const uint8_t *src = panel + (size_t)y * GLES_FB_WIDTH * 4;

            for (x = 0; x < GLES_FB_WIDTH; x++) {
                /* The panel is BGRA (see lcd_refresh_rotated). */
                uint8_t rgb[3] = { src[x * 4 + 2], src[x * 4 + 1],
                                   src[x * 4 + 0] };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        fprintf(stderr, "[gles]   panel scanout 0x%08x written alongside\n",
                (unsigned)fb);
    }
}

/*
 * WHICH GC DREW WHAT, and how each one gets its frame to the screen.
 *
 * The app runs several GCs through this one host context, and only a GC that
 * bound a CA drawable can present into a surface -- the rest fall through to
 * the panel blit, which CoreAnimation then paints over. So "the obstacles are
 * drawn by a context that cannot be seen" is a claim about which ctx issues
 * which primitive, and every request already carries its ctx. Counting it is
 * the whole measurement.
 */
#define GLES_MAX_CTX 8
static struct {
    uint32_t ctx;
    uint64_t tris, linestrips, tristrips, other;
    uint64_t present_surface, present_panel;
} gles_ctx_stats[GLES_MAX_CTX];
static uint32_t gles_cur_ctx;

static unsigned gles_ctx_slot(uint32_t ctx)
{
    unsigned i;

    for (i = 0; i < GLES_MAX_CTX; i++) {
        if (gles_ctx_stats[i].ctx == ctx) {
            return i;
        }
        if (!gles_ctx_stats[i].ctx) {
            gles_ctx_stats[i].ctx = ctx;
            return i;
        }
    }
    return GLES_MAX_CTX - 1;
}

static void gles_report_ctx(void)
{
    unsigned i;

    fprintf(stderr, "[gles] per-GC attribution:\n");
    for (i = 0; i < GLES_MAX_CTX && gles_ctx_stats[i].ctx; i++) {
        fprintf(stderr, "[gles]   ctx=0x%08x  tris=%" PRIu64 " linestrips=%"
                PRIu64 " tristrips=%" PRIu64 " other=%" PRIu64
                "  presents: %" PRIu64 " to a CA surface, %" PRIu64
                " to the panel blit (invisible -- CA paints over it)\n",
                gles_ctx_stats[i].ctx, gles_ctx_stats[i].tris,
                gles_ctx_stats[i].linestrips, gles_ctx_stats[i].tristrips,
                gles_ctx_stats[i].other, gles_ctx_stats[i].present_surface,
                gles_ctx_stats[i].present_panel);
    }
}

/* Count a draw into this frame's primitive mix. */
static void gles_note_primitive(uint32_t mode)
{
    unsigned i = gles_ctx_slot(gles_cur_ctx);

    switch (mode) {
    case GL_TRIANGLES:
        gh.f_tris++;       gles_ctx_stats[i].tris++;       break;
    case GL_LINE_STRIP:
        gh.f_linestrips++; gles_ctx_stats[i].linestrips++; break;
    case GL_TRIANGLE_STRIP:
        gh.f_tristrips++;  gles_ctx_stats[i].tristrips++;  break;
    default:
        gh.f_other++;      gles_ctx_stats[i].other++;      break;
    }
}

/*
 * Report the frame's primitive mix when the SHAPE of it changes -- which
 * primitives appear, not how many -- and reset the counters for the next
 * frame. This is the scene marker; see the fields it reads.
 */
static void gles_note_scene(void)
{
    uint32_t sig = (gh.f_tris       ? 1u : 0) | (gh.f_linestrips ? 2u : 0) |
                   (gh.f_tristrips  ? 4u : 0) | (gh.f_other      ? 8u : 0);

    if (sig != gh.last_sig) {
        fprintf(stderr, "[gles] SCENE CHANGE at frame %" PRIu64
                ": triangles=%u line_strips=%u triangle_strips=%u other=%u\n",
                gh.presents, gh.f_tris, gh.f_linestrips, gh.f_tristrips,
                gh.f_other);
        gh.last_sig = sig;
    }
    gh.f_tris = gh.f_linestrips = gh.f_tristrips = gh.f_other = 0;
}

/* Fold this frame's interval into the tail statistics. See the fields. */
static void gles_note_frame_gap(void)
{
    uint64_t now, gap;

    if (!gles_prof) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (gh.last_present_ns) {
        gap = now - gh.last_present_ns;
        if (gap > gh.frame_gap_max) {
            gh.frame_gap_max = gap;
        }
        if (gap > 33 * 1000000ull) {
            gh.frames_over_33ms++;
        }
        if (gap > 100 * 1000000ull) {
            gh.frames_over_100ms++;
        }
    }
    gh.last_present_ns = now;
}

/*
 * Every 60 presented frames, say how many frames and how much geometry went by
 * and how fast. Both halves matter: the frame rate is the number to report for
 * a port, and the draw split is what distinguishes "the app drew nothing this
 * scene" from "the app drew plenty and none of it was visible".
 */
/*
 * The state that decides whether submitted geometry is actually visible.
 *
 * Printed alongside the frame counter so the same scene can be compared
 * against itself over time, and one scene against another. Cube Runner draws
 * tens of cubes per frame in both its title flythrough and its gameplay, but
 * they only appear in one of them -- so the question is not "is it drawing"
 * but "which piece of state differs", and this prints every candidate at once
 * rather than testing them one reboot at a time.
 */
static void gles_dump_state(void)
{
    GLfloat fog_col[4], mv[16], pr[16], depth_range[2];
    GLfloat fog_start = 0, fog_end = 0, fog_density = 0;
    GLint fog_mode = 0, depth_func = 0, cull_face = 0;

    glGetIntegerv(GL_FOG_MODE, &fog_mode);
    glGetFloatv(GL_FOG_START, &fog_start);
    glGetFloatv(GL_FOG_END, &fog_end);
    glGetFloatv(GL_FOG_DENSITY, &fog_density);
    glGetFloatv(GL_FOG_COLOR, fog_col);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glGetIntegerv(GL_CULL_FACE_MODE, &cull_face);
    glGetFloatv(GL_DEPTH_RANGE, depth_range);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetFloatv(GL_PROJECTION_MATRIX, pr);

    fprintf(stderr,
            "[gles]   enabled: fog=%d depth=%d lighting=%d cull=%d blend=%d "
            "texture2d=%d\n"
            "[gles]   fog: mode=0x%x start=%.3f end=%.3f density=%.4f "
            "colour=(%.2f %.2f %.2f %.2f)\n"
            "[gles]   depth: func=0x%x range=(%.2f %.2f) mask=%d cullmode=0x%x\n"
            "[gles]   modelview  translate=(%.3f %.3f %.3f)  scale=(%.3f %.3f %.3f)\n"
            "[gles]   projection diag=(%.3f %.3f %.3f) m[14]=%.3f\n",
            glIsEnabled(GL_FOG), glIsEnabled(GL_DEPTH_TEST),
            glIsEnabled(GL_LIGHTING), glIsEnabled(GL_CULL_FACE),
            glIsEnabled(GL_BLEND), glIsEnabled(GL_TEXTURE_2D),
            fog_mode, fog_start, fog_end, fog_density,
            fog_col[0], fog_col[1], fog_col[2], fog_col[3],
            depth_func, depth_range[0], depth_range[1],
            (int)glIsEnabled(GL_DEPTH_WRITEMASK), cull_face,
            mv[12], mv[13], mv[14], mv[0], mv[5], mv[10],
            pr[0], pr[5], pr[10], pr[14]);
}

static void gles_report_progress(void)
{
    uint64_t now_ms, dt;
    static int verbose = -1;

    if (verbose < 0) {
        const char *v = getenv("IT_GLES_VERBOSE");
        verbose = v ? atoi(v) : 0;
    }
    if (gh.presents % 60 != 0) {
        return;
    }
    now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    dt = gh.last_report_present ? now_ms - gh.last_report_present : 0;
    fprintf(stderr, "[gles] %" PRIu64 " frames; last 60 in %" PRIu64 " ms "
            "(%.1f fps), draws since last: %" PRIu64 " arrays / %" PRIu64
            " elements\n",
            gh.presents, dt, dt ? 60000.0 / (double)dt : 0.0,
            gh.draw_arrays - gh.last_report_arrays,
            gh.draw_elements - gh.last_report_elements);

    /*
     * The accounting line. Percentages are of WALL time over the same 60
     * frames, not of each other, so what is left over is the guest: if the
     * host side adds up to 20% of the interval then the other 80% is the ARM
     * executing the game, and no amount of work here will help.
     */
    if (gles_swizzle_mode == 2) {
        gles_swizzle = (gh.presents / 300) % 2;
    }
    if (gles_prof && dt) {
        double wall = (double)dt * 1e6;      /* ms -> ns */
        fprintf(stderr, "[gles]   readback path: %s\n",
                gles_swizzle ? "RGBA + per-pixel swizzle (old)"
                             : "BGRA direct (new)");
        fprintf(stderr, "[gles]   frame gaps: worst %.1f ms, %u over 33 ms, "
                "%u over 100 ms\n",
                gh.frame_gap_max / 1e6, gh.frames_over_33ms,
                gh.frames_over_100ms);
        gh.frame_gap_max = 0;
        gh.frames_over_33ms = 0;
        gh.frames_over_100ms = 0;
    }
    if (gles_prof && dt) {
        double wall = (double)dt * 1e6;      /* ms -> ns */
        uint64_t calls = gh.calls - gh.last_report_calls;
        uint64_t tc = gh.t_call - gh.last_report_t_call;
        uint64_t tf = gh.t_fetch - gh.last_report_t_fetch;
        uint64_t te = gh.t_err - gh.last_report_t_err;
        uint64_t tp = gh.t_present - gh.last_report_t_present;

        fprintf(stderr, "[gles]   %" PRIu64 " calls (%.0f/frame, %.1f us each);"
                " host %.1f%% of wall  [fetch %.1f%%  glGetError %.1f%%  "
                "present %.1f%%]\n",
                calls, calls / 60.0,
                calls ? (double)tc / calls / 1000.0 : 0.0,
                100.0 * tc / wall, 100.0 * tf / wall,
                100.0 * te / wall, 100.0 * tp / wall);

        gh.last_report_calls = gh.calls;
        gh.last_report_t_call = gh.t_call;
        gh.last_report_t_fetch = gh.t_fetch;
        gh.last_report_t_err = gh.t_err;
        gh.last_report_t_present = gh.t_present;
    }
    /*
     * The state dump and the per-draw trace are the two tools that answer
     * "the app is drawing and nothing appears". They are off unless asked for,
     * because a per-draw log at 60 fps changes the timing it is reporting on.
     *   IT_GLES_VERBOSE=1  state dump every 60 frames
     *   IT_GLES_VERBOSE=2  and one frame of per-draw trace every 300
     */
    if (verbose >= 1) {
        gles_dump_state();
        /*
         * IT_GLES_TRACE_EVERY: frames between per-draw trace bursts. The
         * default of 300 (5 s) is fine for a scene that persists; a round of
         * Cube Runner can be over inside that, which is how a capture came
         * back with no gameplay in it at all.
         */
        if (verbose >= 2) {
            static int every = -1;

            if (every < 0) {
                const char *e = getenv("IT_GLES_TRACE_EVERY");
                every = e ? atoi(e) : 300;
                if (every < 1) {
                    every = 1;
                }
            }
            if (gh.presents % every == 0) {
                gh.trace_draws = 80;
                gh.dump_pending = true;
            }
        }
    }
    if (verbose >= 1) {
        gles_report_ctx();
    }
    gh.last_report_present = now_ms;
    gh.last_report_arrays = gh.draw_arrays;
    gh.last_report_elements = gh.draw_elements;
}

/*
 * Present into a caller-supplied CPU-addressable buffer -- the shipping path.
 *
 * CoreAnimation allocates an IOSurface, hands it to the engine, and composites
 * it; the engine only ever reads its geometry and writes pixels into it. So all
 * the host needs is the address, the stride and the format. No IOSurface
 * knowledge crosses into QEMU, and nothing here has to stay in step with how CA
 * chose to allocate.
 *
 * Returns 0 on success, -1 if the surface is unusable. Unlike most of this
 * file, that error is real and propagates: a bad surface means the frame went
 * nowhere, and silently returning 0 would make a black screen look like a
 * successful present.
 */
static int gles_present_to_surface(CPUState *cpu, uint32_t base, uint32_t stride,
                                   uint32_t width, uint32_t height,
                                   uint32_t format)
{
    uint32_t y;
    /* Bytes per pixel of the DESTINATION surface. CA picks this from the
     * drawable's format, so every size below follows it rather than assuming
     * the 32-bit case. */
    uint32_t bpp = (format == GLES_SURFACE_RGB565) ? 2 : 4;

    bool bgra;

    if (!base || !width || !height) {
        fprintf(stderr, "[gles] present-surface: bad surface "
                "base=0x%08x %ux%u stride=%u\n", base, width, height, stride);
        return -1;
    }
    if (width > GLES_FB_WIDTH * 4 || height > GLES_FB_HEIGHT * 4) {
        fprintf(stderr, "[gles] present-surface: implausible size %ux%u\n",
                width, height);
        return -1;
    }
    if (stride < width * bpp) {
        fprintf(stderr, "[gles] present-surface: stride %u too small for "
                "width %u at %u bpp\n", stride, width, bpp);
        return -1;
    }

    /* 'BGRA' is what CA uses on this device; accept RGBA too rather than
     * silently producing colour-swapped output for it. */
    bgra = (format != GLES_SURFACE_RGBA32);

    /*
     * Which surface CA gave us for THIS frame.
     *
     * nextBuffer hands out a different surface every frame -- the measured pair
     * alternates forever -- so a renderer that writes one buffer while the
     * compositor scans another produces a picture that is correct but stale,
     * which is indistinguishable from a frozen renderer unless you are
     * watching the addresses. Report the rotation and every new base once.
     */
    if (getenv("IT_GLES_SURFACE_TRACE")) {
        static uint32_t seen[8];
        static unsigned n_seen;
        static uint32_t last_base;
        unsigned i;

        for (i = 0; i < n_seen; i++) {
            if (seen[i] == base) {
                break;
            }
        }
        if (i == n_seen && n_seen < ARRAY_SIZE(seen)) {
            seen[n_seen++] = base;
            fprintf(stderr, "[gles] CA surface #%u: base=0x%08x stride=%u "
                    "%ux%u fmt=0x%08x\n", n_seen, base, stride, width, height,
                    format);
        }
        if (base != last_base) {
            last_base = base;
        } else if ((gh.presents % 120) == 0) {
            fprintf(stderr, "[gles] CA handed the SAME surface 0x%08x twice "
                    "in a row at frame %" PRIu64 "\n", base, gh.presents);
        }
    }

    /* The FBO is GLES_FB_WIDTH x GLES_FB_HEIGHT; take only what fits. */
    {
        uint32_t rw = width < GLES_FB_WIDTH ? width : GLES_FB_WIDTH;
        uint32_t rh = height < GLES_FB_HEIGHT ? height : GLES_FB_HEIGHT;
        const uint8_t *frame = NULL;
        size_t fstride = 0;

        /*
         * The frame, from the render target itself where the host allows it.
         *
         * The IOSurface is BGRA because that is what CoreAnimation wants, so a
         * guest that asked for RGBA has no fast path -- and neither does a run
         * that asked for the old readback in order to time it. Both fall
         * through, and so does any host that never got a surface at all.
         */
        /* The zero-copy path hands back the render target's own BGRA rows, so
         * it can only serve a 32-bit destination; 565 goes via the readback,
         * where GL does the packing. */
        if (bpp == 4 && bgra && !gles_swizzle) {
            frame = gles_frame_lock(&fstride);
        }
        if (frame) {
            for (y = 0; y < rh; y++) {
                /* GL's origin is bottom-left, the surface's is top-left. */
                const uint8_t *src = frame + (size_t)(rh - 1 - y) * fstride;

                if (cpu_memory_rw_debug(cpu, base + (hwaddr)y * stride,
                                        (void *)src, rw * 4, 1) != 0) {
                    fprintf(stderr, "[gles] present-surface: write failed at "
                            "row %u (guest 0x%08x)\n", y, base + y * stride);
                    gles_platform_frame_unlock();
                    return -1;
                }
            }
            gles_dump_frame(frame, fstride, rw, rh, bgra);
            gles_platform_frame_unlock();
            gh.presents++;
            gles_note_scene();
            gles_note_frame_gap();
            gles_report_progress();
            return 0;
        }

        /* Same as the panel present above: read the drawable, not whichever
         * offscreen target the guest left bound. */
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gh.fbo);
        glFinish();

        /*
         * Ask GL for the byte order the destination wants instead of reading
         * RGBA and swapping 150k pixels by hand every frame. GL_BGRA with
         * UNSIGNED_INT_8_8_8_8_REV lands as B,G,R,A in memory on a
         * little-endian host, which is exactly CoreAnimation's layout, so the
         * row copy below becomes a memcpy. The swizzle loop was the largest
         * single item in the frame's host time.
         */
        if (bpp == 2) {
            /* GL packs 565 itself, so there is no conversion loop here: the
             * result already matches CA's little-endian 'L565' layout. */
            glReadPixels(0, 0, rw, rh, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                         gh.readback);
        } else if (bgra && !gles_swizzle) {
            glReadPixels(0, 0, rw, rh, GL_BGRA,
                         GLES_BGRA_READ_TYPE, gh.readback);
        } else {
            glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, gh.readback);
        }
        /*
         * The old path, kept switchable so the two can be compared inside one
         * run. This machine never goes quiet -- eight emulators at once while
         * this was measured -- and a before/after taken from two boots minutes
         * apart measures the host's mood, not the change.
         */
        if (bpp == 4 && bgra && gles_swizzle) {
            uint32_t i, n = rw * rh;

            for (i = 0; i < n; i++) {
                uint8_t *p = gh.readback + (size_t)i * 4;
                uint8_t r = p[0];

                p[0] = p[2];
                p[2] = r;
            }
        }

        for (y = 0; y < rh; y++) {
            /* GL's origin is bottom-left, the surface's is top-left. Only the
             * rw*4 bytes the row actually covers are written, so the staging
             * buffer the swizzle needed is gone with it -- the readback is
             * already in the destination's layout. */
            uint8_t *src = gh.readback + (size_t)(rh - 1 - y) * rw * bpp;

            if (cpu_memory_rw_debug(cpu, base + (hwaddr)y * stride,
                                    src, rw * bpp, 1) != 0) {
                fprintf(stderr, "[gles] present-surface: write failed at row %u "
                        "(guest 0x%08x)\n", y, base + y * stride);
                return -1;
            }
        }
        /* The dump writes 32-bit pixels; a 565 frame is not its format. */
        if (bpp == 4) {
            gles_dump_frame(gh.readback, (size_t)rw * 4, rw, rh, bgra);
        }
    }
    /* Hand the guest's own render target back; the present borrowed it. */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,
                         gles_host_fbo(gh.bound_framebuffer));
    gh.presents++;
    gles_note_scene();
    gles_note_frame_gap();
    gles_report_progress();
    return 0;
}

/* ----------------------------------------------------------------- dispatch */

static float gles_f(uint32_t bits)
{
    union { uint32_t u; float f; } c = { .u = bits };
    return c.f;
}

/* ------------------------------------------------------- buffer objects ---- */

/* ES enum values, same as desktop. */
#define GLES_ARRAY_BUFFER         0x8892
#define GLES_ELEMENT_ARRAY_BUFFER 0x8893
/* A guest buffer big enough to matter is a few MB of geometry; anything past
 * this is a corrupt size, and g_malloc aborts QEMU rather than failing. */
#define GLES_MAX_BUFFER_BYTES ((size_t)64 * 1024 * 1024)

static void gles_buffer_destroy(gpointer p)
{
    GLESBuffer *b = p;

    g_free(b->data);
    g_free(b);
}

/*
 * The buffer with this name, created empty if the guest has not seen it before.
 *
 * ES 1.1 lets an app bind a name it never generated, so "unknown name" is not
 * an error to report -- it is a buffer coming into existence.
 */
static GLESBuffer *gles_buffer_intern(uint32_t name)
{
    GLESBuffer *b;

    if (!name) {
        return NULL;
    }
    if (!gh.buffers) {
        gh.buffers = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                           gles_buffer_destroy);
    }
    b = g_hash_table_lookup(gh.buffers, GUINT_TO_POINTER(name));
    if (!b) {
        b = g_new0(GLESBuffer, 1);
        g_hash_table_insert(gh.buffers, GUINT_TO_POINTER(name), b);
    }
    return b;
}

/* Whichever buffer the guest has bound to this target, or NULL. */
static GLESBuffer *gles_buffer_bound(uint32_t target)
{
    if (target == GLES_ELEMENT_ARRAY_BUFFER) {
        return (GLESBuffer *)gh.element_buffer;
    }
    return (GLESBuffer *)gh.array_buffer;
}

/*
 * Forget every reference to a buffer that is about to be freed.
 *
 * The arrays hold resolved pointers, not names -- that is what keeps the draw
 * path free of lookups -- so a delete has to reach in and clear them, or the
 * next draw walks freed memory. Deleting a bound buffer is legal GL and an
 * engine tearing down a level does exactly this.
 */
static void gles_buffer_forget(const GLESBuffer *b)
{
    GLESArray *arrays[] = { &gh.vertex, &gh.texcoord, &gh.color, &gh.normal };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(arrays); i++) {
        if (arrays[i]->vbo == b) {
            arrays[i]->vbo = NULL;
            arrays[i]->ptr = 0;
        }
    }
    if (gh.array_buffer == b) {
        gh.array_buffer = NULL;
    }
    if (gh.element_buffer == b) {
        gh.element_buffer = NULL;
    }
}


/*
 * The first N matrix-stack operations, with the mode each ran against.
 *
 * A projection that is wrong by a FACTOR rather than by garbage means some
 * earlier matrix survived into it, and only the order of these calls can show
 * which one -- the final matrix cannot.
 */

/*
 * Did that push/pop actually take?
 *
 * Desktop GL only guarantees a PROJECTION stack two deep, and ES guarantees the
 * same -- but an implementation that refuses the push leaves the matrix the app
 * believed it had saved sitting in the live slot. The matching pop then
 * underflows, the app carries on, and every later matrix is silently composed
 * onto the wrong base. Nothing else reports it: the error is raised here and
 * drained by the next draw check long before anyone looks.
 */
static void gles_matrix_stack_check(const char *op)
{
    static unsigned complained;
    GLenum e = glGetError();

    if (e == GL_NO_ERROR || complained >= 4) {
        return;
    }
    complained++;
    {
        GLint mode = 0, depth = 0, maxd = 0;
        glGetIntegerv(GL_MATRIX_MODE, &mode);
        if (mode == GL_PROJECTION) {
            glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &depth);
            glGetIntegerv(GL_MAX_PROJECTION_STACK_DEPTH, &maxd);
        } else {
            glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &depth);
            glGetIntegerv(GL_MAX_MODELVIEW_STACK_DEPTH, &maxd);
        }
        fprintf(stderr, "[gles] %s FAILED: GL error 0x%x, mode=0x%x "
                "depth=%d/%d -- the matrix stack is now out of step with the "
                "guest's\n", op, e, mode, depth, maxd);
    }
}

static void gles_trace_matrix_op(const char *op, uint32_t arg)
{
    static unsigned n;
    GLint mode = 0;

    if (!getenv("IT_GLES_VERBOSE") || n >= 120) {
        return;
    }
    n++;
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    if (arg) {
        fprintf(stderr, "[gles] mtx %-16s arg=0x%x (mode now 0x%x)\n",
                op, arg, mode);
    } else {
        fprintf(stderr, "[gles] mtx %-16s (mode 0x%x)\n", op, mode);
    }
}

static int64_t gles_host_call_1(CPUState *cpu, uint32_t slot, uint32_t ctx,
                                uint32_t argc, const uint32_t *a)
{
    if (!gles_host_init()) {
        return -1;
    }

    switch (slot) {

    /* ---- engine-level operations (not framework dispatch slots) ---- */
    case GLES_OP_PRESENT: {
        uint64_t t0 = gles_t();

        gles_present_to_panel();
        gh.t_present += gles_t() - t0;
        gles_ctx_stats[gles_ctx_slot(ctx)].present_panel++;
        /*
         * Report from HERE too.
         *
         * gles_report_progress was only reachable from the present-to-surface
         * path, so the moment a GC lost its drawable and fell back to this
         * blit the frame counter went silent -- and it went silent in exactly
         * the situation being debugged, which reads as the app slowing down
         * when nothing of the sort has happened. A counter that stops counting
         * when the interesting thing starts is worse than no counter.
         */
        gles_note_scene();
        gles_note_frame_gap();
        gles_report_progress();
        return 0;
    }

    case GLES_OP_PRESENT_SURFACE: {  /* base, stride, w, h, format */
        uint64_t t0 = gles_t();
        int64_t r = gles_present_to_surface(cpu, a[0], a[1], a[2], a[3], a[4]);
        gh.t_present += gles_t() - t0;
        gles_ctx_stats[gles_ctx_slot(ctx)].present_surface++;
        return r;
    }

    /* ---- framework dispatch slots, numbering from GATE1_slotmap.txt ---- */
    case GLES_SLOT_CLEAR_COLOR:                 /* glClearColor(r,g,b,a) */
        glClearColor(gles_f(a[0]), gles_f(a[1]), gles_f(a[2]), gles_f(a[3]));
        return 0;

    case GLES_SLOT_CLEAR:                       /* glClear(mask) */
        /* ES and desktop agree on these bits, so the mask passes through. */
        glClear(a[0]);
        return 0;

    case GLES_SLOT_VIEWPORT:                    /* glViewport(x,y,w,h) */
        glViewport(a[0], a[1], a[2], a[3]);
        return 0;

    case GLES_SLOT_COLOR4F:                     /* glColor4f(r,g,b,a) */
        glColor4f(gles_f(a[0]), gles_f(a[1]), gles_f(a[2]), gles_f(a[3]));
        return 0;

    case GLES_SLOT_ENABLE:
        glEnable(a[0]);
        return 0;

    case GLES_SLOT_DISABLE:
        glDisable(a[0]);
        return 0;

    case GLES_SLOT_ENABLE_CLIENT_STATE:
    case GLES_SLOT_DISABLE_CLIENT_STATE: {
        bool on = (slot == GLES_SLOT_ENABLE_CLIENT_STATE);
        switch (a[0]) {
        case GL_VERTEX_ARRAY:        gh.vertex.enabled = on;   break;
        case GL_TEXTURE_COORD_ARRAY: gh.texcoord.enabled = on; break;
        case GL_COLOR_ARRAY:         gh.color.enabled = on;    break;
        case GL_NORMAL_ARRAY:        gh.normal.enabled = on;   break;
        default: break;
        }
        return 0;
    }

    /*
     * The four client-array pointers. Each captures the ARRAY_BUFFER binding in
     * force right now, because that is when GL decides what the pointer means:
     * with a buffer bound it is an offset into that buffer, without one it is a
     * guest virtual address. Resolving it here rather than at draw time also
     * keeps the draw path's cost at a single null check.
     */
    case GLES_SLOT_VERTEX_POINTER:              /* size,type,stride,ptr */
        gh.vertex.size = a[0];
        gh.vertex.type = a[1];
        gh.vertex.stride = a[2];
        gh.vertex.ptr = a[3];
        gh.vertex.vbo = gh.array_buffer;
        return 0;

    case GLES_SLOT_TEXCOORD_POINTER:
        gh.texcoord.size = a[0];
        gh.texcoord.type = a[1];
        gh.texcoord.stride = a[2];
        gh.texcoord.ptr = a[3];
        gh.texcoord.vbo = gh.array_buffer;
        return 0;

    case GLES_SLOT_COLOR_POINTER:               /* size,type,stride,ptr */
        gh.color.size = a[0];
        gh.color.type = a[1];
        gh.color.stride = a[2];
        gh.color.ptr = a[3];
        gh.color.vbo = gh.array_buffer;
        return 0;

    case GLES_SLOT_NORMAL_POINTER:              /* type,stride,ptr */
        gh.normal.size = 3;                     /* always, per GL */
        gh.normal.type = a[0];
        gh.normal.stride = a[1];
        gh.normal.ptr = a[2];
        gh.normal.vbo = gh.array_buffer;
        return 0;

    case GLES_SLOT_DRAW_ARRAYS: {               /* mode, first, count */
        uint32_t mode = a[0], first = a[1], count = a[2];
        uint32_t bound;

        if (!gh.vertex.enabled) {
            return 0;
        }
        bound = gles_bind_all_arrays(cpu, first, count);
        if (!(bound & 1u)) {                    /* no positions -> no draw */
            /* Unbind the arrays that DID bind: leaving e.g. GL_COLOR_ARRAY
             * enabled on the host, pointing into scratch that a later draw
             * reallocs, is the stale-pointer host crash this file warns about
             * (gles_bind_array's "loaded gun" note) -- reintroduced on the
             * error path if we return without unbinding. */
            gles_unbind_arrays(bound);
            return -1;
        }
        /* We already applied `first` when fetching, so draw from 0. */
        glDrawArrays(mode, 0, count);
        gles_check_draw("glDrawArrays", mode, count);
        gles_trace_draw("glDrawArrays", mode, count);
        gles_unbind_arrays(bound);
        gh.draws++;
        gh.draw_arrays++;
        gles_note_primitive(mode);
        return 0;
    }

    case GLES_SLOT_DRAW_ELEMENTS: {             /* mode, count, type, indices */
        uint32_t mode = a[0], count = a[1], itype = a[2], iptr = a[3];
        uint32_t isz = gles_type_size(itype);
        uint32_t bound, i, maxidx = 0;
        const uint8_t *idx;
        size_t need;

        /* With an ELEMENT_ARRAY_BUFFER bound, `indices` is an offset into it --
         * and offset 0 is the common case, so the null check below only applies
         * without one. */
        if (!gh.vertex.enabled || !count ||
            (!iptr && !gh.element_buffer)) {
            return 0;
        }
        /* ES 1.1 allows only UNSIGNED_BYTE and UNSIGNED_SHORT here. */
        if (itype != GL_UNSIGNED_BYTE && itype != GL_UNSIGNED_SHORT) {
            fprintf(stderr, "[gles] glDrawElements: bad index type 0x%x\n",
                    itype);
            return -1;
        }

        need = (size_t)count * isz;
        if (gh.element_buffer) {
            if (iptr + need > gh.element_buffer->size) {
                fprintf(stderr, "[gles] glDrawElements: %zu index bytes at "
                        "offset %u overrun a %zu-byte buffer\n", need, iptr,
                        gh.element_buffer->size);
                return -1;
            }
            idx = gh.element_buffer->data + iptr;
        } else {
            if (need > gh.ibuf_size) {
                gh.ibuf = g_realloc(gh.ibuf, need);
                gh.ibuf_size = need;
            }
            if (cpu_memory_rw_debug(cpu, iptr, gh.ibuf, need, 0) != 0) {
                fprintf(stderr, "[gles] glDrawElements: cannot read %zu index "
                        "bytes at guest 0x%08x\n", need, iptr);
                return -1;
            }
            idx = gh.ibuf;
        }

        /*
         * Indices are absolute, so the vertex data that has to come across is
         * everything up to the largest one -- there is no `first` to lean on
         * the way glDrawArrays has. Scanning for the maximum costs one pass
         * over the indices and is the only way to know how much of the guest's
         * array is actually referenced; fetching a fixed amount would either
         * truncate the mesh or read guest memory the app never allocated.
         */
        for (i = 0; i < count; i++) {
            /* An index list inside a VBO starts at an arbitrary offset, so the
             * 16-bit read cannot assume alignment the way a cast would. */
            uint32_t v = (isz == 1) ? idx[i] : lduw_le_p(idx + i * 2);
            if (v > maxidx) {
                maxidx = v;
            }
        }

        bound = gles_bind_all_arrays(cpu, 0, maxidx + 1);
        if (!(bound & 1u)) {
            gles_unbind_arrays(bound);           /* see glDrawArrays above */
            return -1;
        }
        glDrawElements(mode, count, itype, idx);
        gles_check_draw("glDrawElements", mode, count);
        gles_trace_draw("glDrawElements", mode, count);
        gles_unbind_arrays(bound);
        gh.draws++;
        gh.draw_elements++;
        gles_note_primitive(mode);
        return 0;
    }

    case GLES_SLOT_GEN_TEXTURES: {              /* n, guest uint* */
        uint32_t n = a[0];
        g_autofree GLuint *ids = NULL;
        if (n > GLES_MAX_NAMES) {
            fprintf(stderr, "[gles] glGenTextures: n=%u exceeds cap\n", n);
            return -1;
        }
        ids = g_new0(GLuint, n ? n : 1);
        glGenTextures(n, ids);
        cpu_memory_rw_debug(cpu, a[1], (uint8_t *)ids, n * sizeof(GLuint), 1);
        return 0;
    }

    case GLES_SLOT_BIND_TEXTURE:                /* target, texture */
        glBindTexture(a[0], a[1]);
        return 0;

    case GLES_SLOT_TEX_PARAMETERI:              /* target, pname, param */
        glTexParameteri(a[0], a[1], a[2]);
        return 0;

    case GLES_SLOT_TEX_IMAGE_2D: {
        /* target, level, internalformat, width, height, border, format, type,
         * pixels -- nine arguments, so they arrive spilled. */
        uint32_t target = a[0], level = a[1], ifmt = a[2];
        uint32_t w = a[3], h = a[4], border = a[5];
        uint32_t fmt = a[6], type = a[7], pixels = a[8];
        size_t bpp = gles_texel_bytes(fmt, type), n;
        const uint8_t *px = NULL;   /* borrowed: points into gh.txbuf */

        n = gles_image_bytes(w, h, bpp);
        if (pixels && n) {
            px = gles_fetch_texels(cpu, pixels, n, "glTexImage2D");
            if (!px) {
                return -1;
            }
        } else if (n) {
            /* A NULL pixel pointer is the "allocate now, fill later" half of
             * the alloc-then-upload idiom, and GL says the contents are
             * UNDEFINED. Undefined is not the same thing on both sides of this
             * shim: the MBX driver handed back cleared pages, while desktop GL
             * hands back whatever was already in that VRAM.
             *
             * So an app that allocates a texture and then fills only the part
             * it actually uses looked perfect on the device and rendered the
             * untouched remainder as STATIC NOISE here. That is precisely what
             * Super Monkey Ball's backgrounds and message panels showed. Zero
             * the storage to match the hardware the guest was written against;
             * it also turns any upload we drop into a visible black region
             * instead of convincing-looking garbage. */
            px = gles_zeroed(n);
            if (!px) {
                return -1;
            }
        }
        /* The staging buffer reproduces the guest's row padding verbatim, so
         * the host must unpack with the guest's alignment, not its own. */
        glPixelStorei(GL_UNPACK_ALIGNMENT, gles_unpack());
        glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px);
        return 0;
    }

    case GLES_SLOT_TEX_SUB_IMAGE_2D: {
        /* target, level, xoffset, yoffset, width, height, format, type,
         * pixels -- nine arguments, spilled, same shape as glTexImage2D.
         * This is the streaming half of the alloc-then-upload idiom
         * (glTexImage2D with NULL, then glTexSubImage2D per frame or per
         * asset); dropping it leaves every such texture incomplete, which
         * fixed-function GL renders as solid white. */
        uint32_t target = a[0], level = a[1], xoff = a[2], yoff = a[3];
        uint32_t w = a[4], h = a[5], fmt = a[6], type = a[7], pixels = a[8];
        size_t bpp = gles_texel_bytes(fmt, type), n;
        const uint8_t *px = NULL;   /* borrowed: points into gh.txbuf */

        n = gles_image_bytes(w, h, bpp);
        if (!pixels || !n) {
            return 0;
        }
        px = gles_fetch_texels(cpu, pixels, n, "glTexSubImage2D");
        if (!px) {
            return -1;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, gles_unpack());
        glTexSubImage2D(target, level, xoff, yoff, w, h, fmt, type, px);
        return 0;
    }

    case GLES_SLOT_COMPRESSED_TEX_IMAGE_2D: {
        /* target, level, internalformat, width, height, border, imageSize,
         * data -- eight arguments, so they arrive spilled. */
        uint32_t target = a[0], level = a[1], ifmt = a[2];
        uint32_t w = a[3], h = a[4], imgsz = a[6], data = a[7];
        const uint8_t *src;

        if (!data || !imgsz) {
            return 0;
        }
        src = gles_fetch_texels(cpu, data, imgsz, "glCompressedTexImage2D");
        if (!src) {
            return -1;
        }
        /* Decoded output is always tightly packed RGBA8, whatever the guest's
         * GL_UNPACK_ALIGNMENT says about its own compressed bytes. */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        if (gles_is_pvrtc(ifmt)) {
            int bpp = (ifmt == PVRTC_RGB_2BPP || ifmt == PVRTC_RGBA_2BPP) ? 2 : 4;
            size_t want = pvrtc_size(w, h, bpp);
            uint8_t *dst;

            if (!want) {
                fprintf(stderr, "[gles] PVRTC %dbpp: %ux%u is not a size the "
                        "format can express; dropped\n", bpp, w, h);
                return -1;
            }
            if (imgsz < want) {
                fprintf(stderr, "[gles] PVRTC %dbpp %ux%u needs %zu bytes but "
                        "only %u were supplied; dropped\n",
                        bpp, w, h, want, imgsz);
                return -1;
            }
            dst = gles_decode_buf((size_t)w * h * 4);
            if (!dst) {
                return -1;
            }
            pvrtc_decode(src, w, h, bpp, dst);
            gles_report_decode(ifmt, w, h, dst);
            glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, dst);
            return 0;
        }

        if (gles_is_paletted(ifmt)) {
            unsigned idx_bits, entry_bytes;
            uint32_t ptype;
            /* A non-positive level means |level|+1 mip levels are packed in
             * after the palette, per OES_compressed_paletted_texture. */
            int32_t slevel = (int32_t)level;
            unsigned nlevels = slevel <= 0 ? (unsigned)(-slevel) + 1 : 1;
            size_t pal_bytes, consumed;
            unsigned lv;

            gles_palette_info(ifmt, &idx_bits, &entry_bytes, &ptype);
            pal_bytes = (size_t)(idx_bits == 4 ? 16 : 256) * entry_bytes;
            if (imgsz < pal_bytes) {
                fprintf(stderr, "[gles] paletted texture 0x%x: %u bytes is not "
                        "even a palette; dropped\n", ifmt, imgsz);
                return -1;
            }
            consumed = pal_bytes;
            for (lv = 0; lv < nlevels; lv++) {
                uint32_t lw = w >> lv, lh = h >> lv;
                size_t idx_bytes;
                uint8_t *dst;

                lw = lw ? lw : 1;
                lh = lh ? lh : 1;
                idx_bytes = (idx_bits == 8) ? (size_t)lw * lh
                                            : ((size_t)lw * lh + 1) / 2;
                if (consumed + idx_bytes > imgsz) {
                    fprintf(stderr, "[gles] paletted texture 0x%x: level %u "
                            "runs past the %u supplied bytes; stopping\n",
                            ifmt, lv, imgsz);
                    break;
                }
                dst = gles_decode_buf((size_t)lw * lh * 4);
                if (!dst) {
                    return -1;
                }
                gles_palette_level(src, ptype, src + consumed, idx_bits,
                                   entry_bytes, lw, lh, dst);
                gles_report_decode(ifmt, lw, lh, dst);
                glTexImage2D(target, lv, GL_RGBA, lw, lh, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, dst);
                consumed += idx_bytes;
            }
            return 0;
        }

        {
            /* Anything else: ETC1, S3TC, a vendor format. Guessing at a layout
             * we have not decoded produces a plausible-looking wrong texture,
             * which is the hardest failure to attribute -- so say so once and
             * leave the texture alone. */
            static uint32_t warned;

            if (warned != ifmt) {
                warned = ifmt;
                fprintf(stderr, "[gles] glCompressedTexImage2D: unhandled "
                        "compressed format 0x%x (%ux%u, %u bytes); dropped\n",
                        ifmt, w, h, imgsz);
            }
            return -1;
        }
    }

    case GLES_SLOT_COMPRESSED_TEX_SUB_IMAGE_2D: {
        /* target, level, xoffset, yoffset, width, height, format, imageSize,
         * data -- nine arguments, spilled.
         *
         * ES 1.1 defines no compressed format that can be sub-imaged: PVRTC
         * needs the whole surface because its endpoints interpolate ACROSS
         * block boundaries, and the paletted formats carry their palette with
         * the base level. So this is a call that has no correct partial
         * implementation, and the honest answer is the one GL itself gives. */
        static bool warned;

        if (!warned) {
            warned = true;
            fprintf(stderr, "[gles] glCompressedTexSubImage2D(fmt=0x%x): no ES "
                    "1.1 compressed format supports sub-image updates; "
                    "dropped\n", a[6]);
        }
        return -1;
    }

    case GLES_SLOT_DELETE_TEXTURES: {           /* n, guest uint* */
        uint32_t n = a[0];
        g_autofree GLuint *ids = NULL;
        if (!n || !a[1]) {
            return 0;
        }
        if (n > GLES_MAX_NAMES) {
            fprintf(stderr, "[gles] glDeleteTextures: n=%u exceeds cap\n", n);
            return -1;
        }
        ids = g_new0(GLuint, n);
        if (cpu_memory_rw_debug(cpu, a[1], (uint8_t *)ids,
                                n * sizeof(GLuint), 0) != 0) {
            return -1;
        }
        glDeleteTextures(n, ids);
        return 0;
    }

    case GLES_SLOT_ALPHA_FUNC:                  /* func, ref(float) */
        glAlphaFunc(a[0], gles_f(a[1]));
        return 0;

    case GLES_SLOT_BIND_BUFFER: {               /* target, buffer */
        GLESBuffer *b = gles_buffer_intern(a[1]);

        if (a[0] == GLES_ELEMENT_ARRAY_BUFFER) {
            gh.element_buffer = b;
        } else {
            gh.array_buffer = b;
        }
        return 0;
    }

    case GLES_SLOT_GEN_BUFFERS: {               /* n, guest uint* */
        uint32_t n = a[0], i;
        g_autofree uint32_t *ids = NULL;

        if (n > GLES_MAX_NAMES) {
            fprintf(stderr, "[gles] glGenBuffers: n=%u exceeds cap\n", n);
            return -1;
        }
        ids = g_new0(uint32_t, n ? n : 1);
        for (i = 0; i < n; i++) {
            ids[i] = ++gh.next_buffer_name;
            gles_buffer_intern(ids[i]);
        }
        if (a[1] && n) {
            cpu_memory_rw_debug(cpu, a[1], (uint8_t *)ids,
                                n * sizeof(uint32_t), 1);
        }
        return 0;
    }

    case GLES_SLOT_DELETE_BUFFERS: {            /* n, guest uint* */
        uint32_t n = a[0], i;
        g_autofree uint32_t *ids = NULL;

        if (!n || !a[1] || !gh.buffers) {
            return 0;
        }
        if (n > GLES_MAX_NAMES) {
            fprintf(stderr, "[gles] glDeleteBuffers: n=%u exceeds cap\n", n);
            return -1;
        }
        ids = g_new0(uint32_t, n);
        if (cpu_memory_rw_debug(cpu, a[1], (uint8_t *)ids,
                                n * sizeof(uint32_t), 0) != 0) {
            return -1;
        }
        for (i = 0; i < n; i++) {
            gpointer key = GUINT_TO_POINTER(ids[i]);
            GLESBuffer *b = ids[i] ? g_hash_table_lookup(gh.buffers, key)
                                   : NULL;

            if (b) {
                gles_buffer_forget(b);
                g_hash_table_remove(gh.buffers, key);
            }
        }
        return 0;
    }

    case GLES_SLOT_BUFFER_DATA: {               /* target, size, data, usage */
        GLESBuffer *b = gles_buffer_bound(a[0]);
        size_t n = a[1];

        /* Usage hints are advisory and there is no host object to hint at. */
        if (!b) {
            fprintf(stderr, "[gles] glBufferData with no buffer bound to "
                    "target 0x%x; dropped\n", a[0]);
            return -1;
        }
        if (n > GLES_MAX_BUFFER_BYTES) {
            fprintf(stderr, "[gles] glBufferData: %zu bytes exceeds cap\n", n);
            return -1;
        }
        b->data = g_realloc(b->data, n ? n : 1);
        b->size = n;
        /* A NULL data pointer means "allocate, contents undefined" -- the
         * alloc-then-glBufferSubData idiom. Zero it so an app that draws
         * before filling gets degenerate geometry rather than whatever the
         * allocator handed back. */
        if (a[2] && n) {
            if (cpu_memory_rw_debug(cpu, a[2], b->data, n, 0) != 0) {
                fprintf(stderr, "[gles] glBufferData: cannot read %zu bytes at "
                        "guest 0x%08x\n", n, a[2]);
                memset(b->data, 0, n);
                return -1;
            }
        } else if (n) {
            memset(b->data, 0, n);
        }
        return 0;
    }

    case GLES_SLOT_BUFFER_SUB_DATA: {           /* target, offset, size, data */
        GLESBuffer *b = gles_buffer_bound(a[0]);
        size_t off = a[1], n = a[2];

        if (!b || !n) {
            return b ? 0 : -1;
        }
        if (off > b->size || n > b->size - off) {
            fprintf(stderr, "[gles] glBufferSubData: %zu bytes at offset %zu "
                    "overruns a %zu-byte buffer; dropped\n", n, off, b->size);
            return -1;
        }
        if (!a[3] ||
            cpu_memory_rw_debug(cpu, a[3], b->data + off, n, 0) != 0) {
            fprintf(stderr, "[gles] glBufferSubData: cannot read %zu bytes at "
                    "guest 0x%08x\n", n, a[3]);
            return -1;
        }
        return 0;
    }

    case GLES_SLOT_GET_INTEGERV: {              /* pname, guest int* */
        uint32_t pname = a[0];
        int32_t v[4] = { 0, 0, 0, 0 };
        unsigned n = 1;

        if (!a[1]) {
            return 0;
        }
        switch (pname) {
        /* FBO state is bookkeeping here, not host GL state -- the host
         * renders into its own FBO regardless (see the OES block below).
         * Answer from the same bookkeeping so save/restore idioms
         * round-trip. */
        case 0x8CA6: /* GL_FRAMEBUFFER_BINDING_OES */
            v[0] = 0;
            break;
        case 0x8CA7: /* GL_RENDERBUFFER_BINDING_OES */
            v[0] = gh.bound_renderbuffer;
            break;
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
            n = 4;
            glGetIntegerv(pname, v);
            break;
        case GL_MAX_VIEWPORT_DIMS:
            n = 2;
            glGetIntegerv(pname, v);
            break;
        default:
            /* Scalar queries (GL_MAX_TEXTURE_SIZE and friends) pass
             * through. An ES-only pname sets GL_INVALID_ENUM on the host
             * and writes nothing, so the guest sees 0 -- the same thing
             * the silent default used to hand it, minus the stack
             * garbage. */
            glGetIntegerv(pname, v);
            break;
        }
        cpu_memory_rw_debug(cpu, a[1], (uint8_t *)v, n * sizeof(v[0]), 1);
        return 0;
    }

    case GLES_SLOT_MATRIX_MODE:
        gles_trace_matrix_op("glMatrixMode", a[0]);
        glMatrixMode(a[0]);
        return 0;

    case GLES_SLOT_LOAD_IDENTITY:
        gles_trace_matrix_op("glLoadIdentity", 0);
        glLoadIdentity();
        return 0;

    case GLES_SLOT_ORTHOF:                      /* l,r,b,t,n,f -- spilled */
        gles_trace_matrix_op("glOrthof", 0);
        glOrtho(gles_f(a[0]), gles_f(a[1]), gles_f(a[2]),
                gles_f(a[3]), gles_f(a[4]), gles_f(a[5]));
        return 0;

    case GLES_SLOT_FRUSTUMF:                    /* l,r,b,t,n,f -- spilled */
        {
            static bool said;
            if (!said && getenv("IT_GLES_VERBOSE")) {
                said = true;
                fprintf(stderr, "[gles] glFrustumf(l=%g r=%g b=%g t=%g "
                        "n=%g f=%g)\n", gles_f(a[0]), gles_f(a[1]),
                        gles_f(a[2]), gles_f(a[3]), gles_f(a[4]), gles_f(a[5]));
            }
        }
        gles_trace_matrix_op("glFrustumf", 0);
        glFrustum(gles_f(a[0]), gles_f(a[1]), gles_f(a[2]),
                  gles_f(a[3]), gles_f(a[4]), gles_f(a[5]));
        return 0;

    /* ---- matrix stack ---- */
    case GLES_SLOT_PUSH_MATRIX:
        gles_trace_matrix_op("glPushMatrix", 0);
        glPushMatrix();
        gles_matrix_stack_check("glPushMatrix");
        return 0;

    case GLES_SLOT_POP_MATRIX:
        gles_trace_matrix_op("glPopMatrix", 0);
        glPopMatrix();
        gles_matrix_stack_check("glPopMatrix");
        return 0;

    case GLES_SLOT_TRANSLATEF:                  /* x,y,z */
        glTranslatef(gles_f(a[0]), gles_f(a[1]), gles_f(a[2]));
        return 0;

    case GLES_SLOT_SCALEF:                      /* x,y,z */
        glScalef(gles_f(a[0]), gles_f(a[1]), gles_f(a[2]));
        return 0;

    case GLES_SLOT_ROTATEF:                     /* angle,x,y,z */
        glRotatef(gles_f(a[0]), gles_f(a[1]), gles_f(a[2]), gles_f(a[3]));
        return 0;

    case GLES_SLOT_MULT_MATRIXF: {              /* const GLfloat m[16] */
        float m[16];
        if (!gles_fetch_floats(cpu, a[0], 16, m)) {
            return -1;
        }
        glMultMatrixf(m);
        return 0;
    }

    case GLES_SLOT_LOAD_MATRIXF: {              /* const GLfloat m[16] */
        float m[16];
        GLint mm = 0;
        if (!gles_fetch_floats(cpu, a[0], 16, m)) {
            return -1;
        }
        /* Report the first PROJECTION load: an app that builds its own
         * perspective matrix never calls glFrustumf at all, so this is the only
         * place its near/far convention becomes visible. */
        glGetIntegerv(GL_MATRIX_MODE, &mm);
        if (mm == GL_PROJECTION) {
            static bool said;
            if (!said && getenv("IT_GLES_VERBOSE")) {
                said = true;
                fprintf(stderr, "[gles] glLoadMatrixf(PROJECTION) "
                        "diag=(%g %g %g) m[11]=%g m[14]=%g\n",
                        m[0], m[5], m[10], m[11], m[14]);
            }
        }
        glLoadMatrixf(m);
        return 0;
    }

    case GLES_SLOT_SCALEX:                      /* x,y,z in 16.16 fixed */
        glScalef((int32_t)a[0] / 65536.0f,
                 (int32_t)a[1] / 65536.0f,
                 (int32_t)a[2] / 65536.0f);
        return 0;

    /* ---- per-fragment state ---- */
    case GLES_SLOT_BLEND_FUNC:                  /* sfactor, dfactor */
        glBlendFunc(a[0], a[1]);
        return 0;

    case GLES_SLOT_DEPTH_MASK:
        /*
         * Measured on Cube Runner: it calls this exactly once, with 0, at
         * startup, and never clears GL_DEPTH_BUFFER_BIT at all -- every one of
         * its glClear calls is GL_COLOR_BUFFER_BIT alone. So the depth buffer
         * keeps its initial value forever and GL_LESS always passes: the game
         * is a pure painter's-order renderer that sorts its own geometry back
         * to front. Worth knowing before blaming the depth attachment for
         * anything: depth is enabled here but carries no information.
         */
        glDepthMask(a[0] ? GL_TRUE : GL_FALSE);
        return 0;

    case GLES_SLOT_CLEAR_DEPTHF:
        /* ES takes a float in [0,1]; desktop GL takes a double. */
        glClearDepth(gles_f(a[0]));
        return 0;

    case GLES_SLOT_SHADE_MODEL:
        glShadeModel(a[0]);
        return 0;

    case GLES_SLOT_PIXEL_STOREI:                /* pname, param */
        /*
         * Recorded rather than forwarded. The value describes how the GUEST's
         * pixels are laid out, and it has to be applied at each upload against
         * the staging buffer -- forwarding it here would set it on a host
         * context whose alignment the intervening calls are free to change.
         * GL_PACK_ALIGNMENT is readback-side and nothing here reads back.
         */
        if (a[0] == GL_UNPACK_ALIGNMENT) {
            gh.unpack_alignment = a[1];
        }
        return 0;

    case GLES_SLOT_SCISSOR:                     /* x, y, width, height */
        glScissor(a[0], a[1], a[2], a[3]);
        return 0;

    case GLES_SLOT_DEPTH_FUNC:                  /* func */
        glDepthFunc(a[0]);
        return 0;

    case GLES_SLOT_FRONT_FACE:                  /* mode */
        glFrontFace(a[0]);
        return 0;

    case GLES_SLOT_TEX_ENVI:                    /* target, pname, param */
        /* Desktop GL has no glTexEnvi -- the integer entry point is spelled
         * glTexEnvf there, and every ES 1.1 pname (GL_TEXTURE_ENV_MODE and the
         * combiner selectors) is an enum that survives the float round trip
         * exactly. */
        glTexEnvf(a[0], a[1], (GLfloat)(GLint)a[2]);
        return 0;

    case GLES_SLOT_ACTIVE_TEXTURE:              /* texture */
        glActiveTexture(a[0]);
        return 0;

    case GLES_SLOT_CLIENT_ACTIVE_TEXTURE:       /* texture */
        glClientActiveTexture(a[0]);
        return 0;

    case GLES_SLOT_LINE_WIDTH:
        glLineWidth(gles_f(a[0]));
        return 0;

    case GLES_SLOT_HINT:                        /* target, mode */
        /*
         * Dropped rather than forwarded. Every hint is by definition allowed to
         * do nothing, but the ES hint targets are not all desktop enums, and a
         * bad one sets GL_INVALID_ENUM -- which the guest can read back through
         * glGetError and interpret as a failure of whatever it called next.
         * Trading a no-op hint for a spurious error is a bad bargain.
         */
        return 0;

    /* ---- lighting and fog ---- */
    case GLES_SLOT_LIGHTFV: {                   /* light, pname, params */
        float p[4];
        unsigned n = gles_light_nparams(a[1]);
        if (!gles_fetch_floats(cpu, a[2], n, p)) {
            return -1;
        }
        glLightfv(a[0], a[1], p);
        return 0;
    }

    case GLES_SLOT_MATERIALFV: {                /* face, pname, params */
        float p[4];
        unsigned n = gles_material_nparams(a[1]);
        if (!gles_fetch_floats(cpu, a[2], n, p)) {
            return -1;
        }
        glMaterialfv(a[0], a[1], p);
        return 0;
    }

    case GLES_SLOT_FOGF:                        /* pname, param */
        glFogf(a[0], gles_f(a[1]));
        return 0;

    case GLES_SLOT_FOGFV: {                     /* pname, params */
        float p[4];
        unsigned n = (a[0] == GL_FOG_COLOR) ? 4 : 1;
        if (!gles_fetch_floats(cpu, a[1], n, p)) {
            return -1;
        }
        glFogfv(a[0], p);
        return 0;
    }

    case GLES_SLOT_FINISH:
        glFinish();
        return 0;

    case GLES_SLOT_FLUSH:
        glFlush();
        return 0;

    case GLES_SLOT_GET_ERROR:
        return glGetError();

    /*
     * OES framebuffer objects, executed rather than answered.
     *
     * These used to be a facade: every call returned 0, glCheckFramebufferStatus
     * always claimed COMPLETE, and every draw landed in gh.fbo no matter what
     * the guest had bound. That is accidentally correct for an app with a
     * single render target -- which is most of them, and why it survived --
     * but it silently destroys any app that renders to more than one. Binding
     * a second framebuffer did nothing, so a scene meant for an offscreen
     * texture was drawn over the drawable instead and the texture stayed
     * empty. Labyrinth's 3D level came out WHITE for exactly this reason: it
     * renders the world to a texture via glFramebufferTexture2DOES, then draws
     * that texture to the screen, and the texture it sampled was never
     * written. Its menus are UIKit, which is why only the game looked broken.
     *
     * So the names are real host names and the calls are real host calls. Only
     * the drawable framebuffer is special-cased -- see gh.fbo_drawable.
     */
    case GLES_SLOT_GEN_RENDERBUFFERS:
    case GLES_SLOT_GEN_FRAMEBUFFERS: {   /* n, guest uint* */
        uint32_t n = a[0];
        g_autofree GLuint *ids = NULL;
        if (n > GLES_MAX_NAMES) {
            fprintf(stderr, "[gles] glGen*: n=%u exceeds cap\n", n);
            return -1;
        }
        ids = g_new0(GLuint, n ? n : 1);
        if (slot == GLES_SLOT_GEN_RENDERBUFFERS) {
            glGenRenderbuffersEXT(n, ids);
        } else {
            glGenFramebuffersEXT(n, ids);
        }
        if (a[1] && n) {
            cpu_memory_rw_debug(cpu, a[1], (uint8_t *)ids,
                                n * sizeof(GLuint), 1);
        }
        return 0;
    }

    case GLES_SLOT_BIND_RENDERBUFFER:    /* target, renderbuffer */
        gh.bound_renderbuffer = a[1];
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, a[1]);
        return 0;

    case GLES_SLOT_BIND_FRAMEBUFFER:     /* target, framebuffer */
        gh.bound_framebuffer = a[1];
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gles_host_fbo(a[1]));
        return 0;

    case GLES_SLOT_RENDERBUFFER_STORAGE: /* target, internalformat, w, h */
        /* Storage through GL means this is NOT the drawable; the drawable gets
         * its own from -renderbufferStorage:fromDrawable:, outside GL. */
        g_hash_table_add(gh.rb_sized,
                         GUINT_TO_POINTER(gh.bound_renderbuffer));
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,
                                 gles_rb_format(a[1]), a[2], a[3]);
        return 0;

    case GLES_SLOT_FB_RENDERBUFFER: {    /* target, attach, rbtarget, rb */
        uint32_t attach = a[1], rb = a[3];
        /*
         * A colour attachment whose renderbuffer never got GL storage is the
         * CA drawable, so this framebuffer is the one that must keep meaning
         * gh.fbo. Its attachments are then left alone entirely -- gh.fbo
         * already carries the colour target we present out of and a matching
         * depth buffer, and letting the guest re-attach over either would
         * break the present path.
         */
        if (attach == GL_COLOR_ATTACHMENT0_EXT && rb
            && !g_hash_table_contains(gh.rb_sized, GUINT_TO_POINTER(rb))) {
            g_hash_table_add(gh.fbo_drawable,
                             GUINT_TO_POINTER(gh.bound_framebuffer));
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gh.fbo);
            return 0;
        }
        if (g_hash_table_contains(gh.fbo_drawable,
                                  GUINT_TO_POINTER(gh.bound_framebuffer))) {
            return 0;
        }
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, attach,
                                     GL_RENDERBUFFER_EXT, rb);
        return 0;
    }

    case GLES_SLOT_FB_TEXTURE_2D:        /* target, attach, textarget, tex, lvl */
        if (g_hash_table_contains(gh.fbo_drawable,
                                  GUINT_TO_POINTER(gh.bound_framebuffer))) {
            return 0;
        }
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, a[1], a[2], a[3], a[4]);
        return 0;

    case GLES_SLOT_DELETE_RENDERBUFFERS:
    case GLES_SLOT_DELETE_FRAMEBUFFERS: {   /* n, guest const uint* */
        uint32_t n = a[0], i;
        g_autofree GLuint *ids = NULL;
        if (n > GLES_MAX_NAMES) {
            fprintf(stderr, "[gles] glDelete*: n=%u exceeds cap\n", n);
            return -1;
        }
        if (!n || !a[1]) {
            return 0;
        }
        ids = g_new0(GLuint, n);
        cpu_memory_rw_debug(cpu, a[1], (uint8_t *)ids, n * sizeof(GLuint), 0);
        if (slot == GLES_SLOT_DELETE_RENDERBUFFERS) {
            for (i = 0; i < n; i++) {
                g_hash_table_remove(gh.rb_sized, GUINT_TO_POINTER(ids[i]));
            }
            glDeleteRenderbuffersEXT(n, ids);
        } else {
            for (i = 0; i < n; i++) {
                /* Never let the guest delete gh.fbo out from under the
                 * present path by deleting the name that stands for it. */
                if (g_hash_table_remove(gh.fbo_drawable,
                                        GUINT_TO_POINTER(ids[i]))) {
                    ids[i] = 0;
                }
            }
            glDeleteFramebuffersEXT(n, ids);
        }
        return 0;
    }

    case GLES_SLOT_CHECK_FB_STATUS:
        /* Report what the host actually thinks. Claiming COMPLETE
         * unconditionally is how an app gets told its render target is fine
         * when nothing was ever attached to it. */
        return glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);

    case GLES_SLOT_GET_RB_PARAMETERIV: { /* target, pname, guest int* */
        GLint v = 0;
        if (g_hash_table_contains(gh.rb_sized,
                                  GUINT_TO_POINTER(gh.bound_renderbuffer))) {
            glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT, a[1], &v);
        } else {
            /* The drawable: CA owns its storage, so the host renderbuffer has
             * none to report and the answer is the panel's own geometry. */
            switch (a[1]) {
            case 0x8D42: v = GLES_FB_WIDTH;  break; /* RENDERBUFFER_WIDTH_OES  */
            case 0x8D43: v = GLES_FB_HEIGHT; break; /* RENDERBUFFER_HEIGHT_OES */
            case 0x8D44: v = 0x8058;         break; /* INTERNAL_FORMAT -> RGBA8 */
            default:     v = 0;              break;
            }
        }
        if (a[2]) {
            cpu_memory_rw_debug(cpu, a[2], (uint8_t *)&v, sizeof(v), 1);
        }
        return 0;
    }

    case GLES_SLOT_GET_FB_ATTACH_PARAM: { /* target, attach, pname, int* */
        uint32_t v = gh.bound_renderbuffer;
        if (a[3]) {
            cpu_memory_rw_debug(cpu, a[3], (uint8_t *)&v, sizeof(v), 1);
        }
        return 0;
    }

    default:
        /* Unimplemented on purpose -- see SCOPE at the top. Returning 0 rather
         * than an error keeps a guest that touches an unhandled state setter
         * running, so the call stream can still be observed end to end.
         *
         * But say so ONCE per slot. Silence here cost real debugging time:
         * Super Monkey Ball's 3D world came through as static noise, and the
         * cause was an entry point the guest called happily and we dropped on
         * the floor without a word. A missing slot must never again be
         * invisible -- an app that renders wrong looks identical to an app
         * that renders right until something says which call went nowhere. */
        {
            static bool warned[1024];
            if (slot < ARRAY_SIZE(warned) && !warned[slot]) {
                warned[slot] = true;
                fprintf(stderr, "[gles] UNHANDLED slot %u (0x%03x) argc=%u -- "
                        "returning 0; the guest will render wrong\n",
                        slot, slot * 4 + 0x10, argc);
            }
        }
        return 0;
    }
}

/*
 * Whether the host will accept GL right now.
 *
 * iOS terminates a process that issues GL commands while it is not foreground,
 * and the vCPU thread has no idea the app was backgrounded -- it will keep
 * servicing guest GL calls into a context the system has already disowned. The
 * app clears this on the way out and sets it on the way back in; the guest sees
 * the same -1 it gets from any other host refusal and falls back to software.
 */
static bool gles_allowed = true;

void gles_host_set_allowed(bool allowed)
{
    gles_allowed = allowed;
}

int64_t gles_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx,
                       uint32_t argc, const uint32_t *a)
{
    uint64_t t0;
    int64_t r;

    if (!gles_allowed) {
        return -1;
    }

    gles_read_switches();
    if (!gles_host_init()) {
        return -1;
    }

    gles_platform_make_current();

    gh.calls++;
    gles_cur_ctx = ctx;
    t0 = gles_t();
    if (gles_strict) {
        /*
         * Attribute GL errors to the call that RAISED them.
         *
         * glGetError reports the first error since it was last called, so a
         * check placed anywhere else blames whichever call happened to look
         * next -- which is how a real GL_INVALID_OPERATION first showed up
         * pinned on an innocent glPushMatrix. Draining immediately before and
         * reading immediately after is the only attribution that holds, and it
         * is what turns "something in this frame is wrong" into a slot number.
         */
        while (glGetError() != GL_NO_ERROR) {
            /* drain whatever was pending from before this call */
        }
        r = gles_host_call_1(cpu, slot, ctx, argc, a);
        {
            GLenum e = glGetError();
            if (e != GL_NO_ERROR) {
                static uint16_t seen[64];
                static unsigned n_seen;
                unsigned i;
                bool known = false;

                for (i = 0; i < n_seen; i++) {
                    if (seen[i] == (uint16_t)slot) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    if (n_seen < ARRAY_SIZE(seen)) {
                        seen[n_seen++] = (uint16_t)slot;
                    }
                    fprintf(stderr, "[gles] slot %u raised GL error 0x%x\n",
                            slot, e);
                }
            }
        }
    } else {
        r = gles_host_call_1(cpu, slot, ctx, argc, a);
    }
    gh.t_call += gles_t() - t0;
    return r;
}

void gles_host_stats(uint64_t *draws, uint64_t *presents)
{
    *draws = gh.draws;
    *presents = gh.presents;
}
