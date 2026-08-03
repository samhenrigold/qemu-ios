/*
 * OpenGL ES 1.1 high-level emulation -- host-side renderer.
 *
 * QC_GLES requests arrive here from the guest's MBXGLEngine replacement and are
 * executed against a real OpenGL context on the host. The MBX is not involved
 * at any point: the stock engine's only use of IOKit is inside
 * GLESCreateSharegroup, and once the engine is ours there is no reason to keep
 * that rendezvous alive.
 *
 * The host context is legacy-profile CGL, which on modern macOS is still a
 * genuine fixed-function GL 2.1 implementation (measured: "2.1 Metal - 91.7",
 * glMatrixMode/glOrtho/glColor4f/glVertexPointer/glDrawArrays all real). ES 1.1
 * is close enough to that subset that most entry points forward one-to-one,
 * which is the whole reason this approach is viable -- there is no shader
 * translation anywhere in this file.
 *
 * SCOPE. This is deliberately not 178 entry points. It is the smallest set that
 * gets a triangle onto the panel and can be verified end to end; 125 of the
 * remaining entry points are pure state setters that are cheap to add once the
 * pixel path is known good, and adding them before that would be broad
 * untested coverage.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/arm/ipod_touch_2g.h"
#include "hw/arm/guest-services/general.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

/* The panel. iOS keeps its framebuffer in portrait and rotates inside it, so
 * these are the framebuffer's dimensions, not the UI's. */
#define GLES_FB_WIDTH  320
#define GLES_FB_HEIGHT 480

/* Guest vertex/texcoord array state. The guest hands us a pointer into its own
 * address space; nothing is read from it until a draw call, exactly as GL
 * specifies, so the guest is free to refill the buffer between calls. */
typedef struct {
    uint32_t enabled;
    uint32_t size;      /* components per element */
    uint32_t type;      /* GL_FLOAT etc, in ES enum values (same as desktop) */
    uint32_t stride;
    uint32_t ptr;       /* guest VA */
} GLESArray;

typedef struct {
    bool inited;
    bool failed;
    CGLContextObj cgl;
    GLuint fbo, tex, depth;

    GLESArray vertex;
    GLESArray texcoord;
    GLESArray color;

    /* Scratch for pulling guest arrays across. Grown as needed, never shrunk. */
    uint8_t *vbuf;
    size_t vbuf_size;
    uint8_t *tbuf;
    size_t tbuf_size;

    uint8_t *readback;  /* GLES_FB_WIDTH * GLES_FB_HEIGHT * 4 */

    uint64_t draws;
    uint64_t presents;
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

/* ---------------------------------------------------------------- context */

static bool gles_host_init(void)
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

    if (gh.inited) {
        return true;
    }
    if (gh.failed) {
        return false;
    }

    e = CGLChoosePixelFormat(attrs, &pix, &npix);
    if (e || !pix) {
        fprintf(stderr, "[gles] CGLChoosePixelFormat failed (%d)\n", e);
        gh.failed = true;
        return false;
    }
    e = CGLCreateContext(pix, NULL, &gh.cgl);
    CGLDestroyPixelFormat(pix);
    if (e || !gh.cgl) {
        fprintf(stderr, "[gles] CGLCreateContext failed (%d)\n", e);
        gh.failed = true;
        return false;
    }
    CGLSetCurrentContext(gh.cgl);

    glGenFramebuffersEXT(1, &gh.fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, gh.fbo);

    glGenTextures(1, &gh.tex);
    glBindTexture(GL_TEXTURE_2D, gh.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLES_FB_WIDTH, GLES_FB_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
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

    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)
        != GL_FRAMEBUFFER_COMPLETE_EXT) {
        fprintf(stderr, "[gles] FBO incomplete\n");
        gh.failed = true;
        return false;
    }

    /* ES 1.1's default matrices are identity with a viewport-sized ortho set up
     * by the app; we only pre-load something sane so that a guest which never
     * touches the matrix stack still draws in framebuffer pixel coordinates. */
    glViewport(0, 0, GLES_FB_WIDTH, GLES_FB_HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, GLES_FB_WIDTH, 0, GLES_FB_HEIGHT, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    gh.readback = g_malloc0((size_t)GLES_FB_WIDTH * GLES_FB_HEIGHT * 4);
    gh.inited = true;

    fprintf(stderr, "[gles] host GL up: %s / %s\n",
            glGetString(GL_VERSION), glGetString(GL_RENDERER));
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

/*
 * Pull `count` elements of a client array out of guest memory.
 *
 * A stride of 0 means tightly packed, per GL. Anything else and we still have
 * to copy the whole span including the gaps, because the host GL will walk it
 * with the same stride.
 */
static uint8_t *gles_fetch_array(CPUState *cpu, GLESArray *a, uint32_t first,
                                 uint32_t count, uint8_t **buf, size_t *cap)
{
    uint32_t esz = gles_type_size(a->type) * a->size;
    uint32_t stride = a->stride ? a->stride : esz;
    size_t need;

    if (!esz || !count) {
        return NULL;
    }
    /* Last element still only occupies esz bytes, not a full stride. */
    need = (size_t)stride * (count - 1) + esz;

    if (need > *cap) {
        *buf = g_realloc(*buf, need);
        *cap = need;
    }
    if (cpu_memory_rw_debug(cpu, a->ptr + (hwaddr)stride * first,
                            *buf, need, 0) != 0) {
        fprintf(stderr, "[gles] failed to read %zu bytes of vertex data at "
                "guest 0x%08x\n", need, a->ptr);
        return NULL;
    }
    return *buf;
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
static void gles_present_to_panel(void)
{
    IPodTouchMachineState *nms;
    hwaddr fb;
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

    glFinish();
    glReadPixels(0, 0, GLES_FB_WIDTH, GLES_FB_HEIGHT, GL_RGBA,
                 GL_UNSIGNED_BYTE, gh.readback);

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

/* ----------------------------------------------------------------- dispatch */

static float gles_f(uint32_t bits)
{
    union { uint32_t u; float f; } c = { .u = bits };
    return c.f;
}

int64_t gles_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx,
                       uint32_t argc, const uint32_t *a)
{
    if (!gles_host_init()) {
        return -1;
    }
    CGLSetCurrentContext(gh.cgl);

    switch (slot) {

    /* ---- engine-level operations (not framework dispatch slots) ---- */
    case GLES_OP_PRESENT:
        gles_present_to_panel();
        return 0;

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
        default: break;
        }
        return 0;
    }

    case GLES_SLOT_VERTEX_POINTER:              /* size,type,stride,ptr */
        gh.vertex.size = a[0];
        gh.vertex.type = a[1];
        gh.vertex.stride = a[2];
        gh.vertex.ptr = a[3];
        return 0;

    case GLES_SLOT_TEXCOORD_POINTER:
        gh.texcoord.size = a[0];
        gh.texcoord.type = a[1];
        gh.texcoord.stride = a[2];
        gh.texcoord.ptr = a[3];
        return 0;

    case GLES_SLOT_DRAW_ARRAYS: {               /* mode, first, count */
        uint32_t mode = a[0], first = a[1], count = a[2];
        uint8_t *vp, *tp = NULL;

        if (!gh.vertex.enabled) {
            return 0;
        }
        vp = gles_fetch_array(cpu, &gh.vertex, first, count,
                              &gh.vbuf, &gh.vbuf_size);
        if (!vp) {
            return -1;
        }
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(gh.vertex.size, gh.vertex.type, gh.vertex.stride, vp);

        if (gh.texcoord.enabled && gh.texcoord.ptr) {
            tp = gles_fetch_array(cpu, &gh.texcoord, first, count,
                                  &gh.tbuf, &gh.tbuf_size);
            if (tp) {
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glTexCoordPointer(gh.texcoord.size, gh.texcoord.type,
                                  gh.texcoord.stride, tp);
            }
        }

        /* We already applied `first` when fetching, so draw from 0. */
        glDrawArrays(mode, 0, count);

        glDisableClientState(GL_VERTEX_ARRAY);
        if (tp) {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        gh.draws++;
        return 0;
    }

    case GLES_SLOT_GEN_TEXTURES: {              /* n, guest uint* */
        uint32_t n = a[0];
        g_autofree GLuint *ids = g_new0(GLuint, n ? n : 1);
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
        size_t bpp, n;
        g_autofree uint8_t *px = NULL;

        switch (fmt) {
        case GL_RGBA:            bpp = 4; break;
        case GL_RGB:             bpp = 3; break;
        case GL_LUMINANCE_ALPHA: bpp = 2; break;
        case GL_ALPHA:
        case GL_LUMINANCE:       bpp = 1; break;
        default:                 bpp = 4; break;
        }
        if (type == GL_UNSIGNED_SHORT_5_6_5 ||
            type == GL_UNSIGNED_SHORT_4_4_4_4 ||
            type == GL_UNSIGNED_SHORT_5_5_5_1) {
            bpp = 2;
        }

        n = (size_t)w * h * bpp;
        if (pixels && n) {
            px = g_malloc(n);
            if (cpu_memory_rw_debug(cpu, pixels, px, n, 0) != 0) {
                fprintf(stderr, "[gles] glTexImage2D: cannot read %zu bytes "
                        "at guest 0x%08x\n", n, pixels);
                return -1;
            }
        }
        glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px);
        return 0;
    }

    case GLES_SLOT_MATRIX_MODE:
        glMatrixMode(a[0]);
        return 0;

    case GLES_SLOT_LOAD_IDENTITY:
        glLoadIdentity();
        return 0;

    case GLES_SLOT_ORTHOF:                      /* l,r,b,t,n,f -- spilled */
        glOrtho(gles_f(a[0]), gles_f(a[1]), gles_f(a[2]),
                gles_f(a[3]), gles_f(a[4]), gles_f(a[5]));
        return 0;

    case GLES_SLOT_FINISH:
        glFinish();
        return 0;

    case GLES_SLOT_FLUSH:
        glFlush();
        return 0;

    case GLES_SLOT_GET_ERROR:
        return glGetError();

    default:
        /* Unimplemented on purpose -- see SCOPE at the top. Returning 0 rather
         * than an error keeps a guest that touches an unhandled state setter
         * running, so the call stream can still be observed end to end. */
        return 0;
    }
}

void gles_host_stats(uint64_t *draws, uint64_t *presents)
{
    *draws = gh.draws;
    *presents = gh.presents;
}
