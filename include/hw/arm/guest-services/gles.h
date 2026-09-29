/*
 * OpenGL ES 1.1 high-level emulation -- guest/host request format.
 *
 * The guest side of this is a drop-in replacement for
 * /System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle. The stock
 * bundle talks to the AppleMBX kext through IOKit; ours talks to QEMU instead,
 * so the whole IOKit/MBX rendezvous is bypassed and the host renders with real
 * OpenGL.
 *
 * Every gl* entry point in OpenGLES.framework is the same six-instruction
 * trampoline: it loads the per-thread GC out of TSD key 30, indexes a
 * framework-owned dispatch table with a fixed slot, and tail-calls through it
 * with the GC as arg0. So a single request shape covers all 178 of them: the
 * slot says which call it was, and the arguments follow. Slot numbers are the
 * table's byte offset / 4 -- a numbering the framework already owns, so there
 * is no table of ours that has to be kept in sync with it.
 *
 * WHY THIS STRUCT IS EXACTLY 32 BYTES
 *
 * It sits in qemu_call_t's args union, and that union's size decides where
 * retval lands. contrib/it-kbd-agent hardcodes the layout as
 * "call_number(4) + args(32) + retval(8) + error(8)", and -- worse -- it is
 * *compiled into NAND images that already exist*. Widening the union would
 * shift retval out from under every agent binary already injected into every
 * image, with no build error anywhere to catch it. So this struct is capped at
 * 32 bytes, and general.h static-asserts the total.
 *
 * That cap is why arguments are split: four inline, and anything longer spilled
 * to a guest buffer. glTexImage2D and glCompressedTexSubImage2D take nine
 * scalars, which was never going to fit inline.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */

#ifndef HW_ARM_GUEST_SERVICES_GLES_H
#define HW_ARM_GUEST_SERVICES_GLES_H

#include <stdint.h>

#define QC_GLES_INLINE_ARGS 4

typedef struct __attribute__((packed)) {
    /* Dispatch-table slot, i.e. the framework's table offset / 4. */
    uint32_t slot;
    /* The engine's GC handle -- arg0 of every trampoline. Opaque to the guest.
     * The host does NOT switch context on it: there is one host GL context and
     * its state is global, shared by every guest GC (see the note on the `gh`
     * global in gles-host.c). This is carried only so the host can attribute
     * per-GC statistics -- which GC drew what, and which one presented. */
    uint32_t ctx;
    /* How many 32-bit arguments this call carries, GC excluded. */
    uint32_t argc;
    /* Guest VA of the full argument array, used when argc > QC_GLES_INLINE_ARGS.
     * Zero when the inline array below carries everything. */
    uint32_t spill;
    /* Arguments widened to 32 bits. Floats travel as their bit patterns;
     * pointers are guest virtual addresses the host reads with
     * cpu_memory_rw_debug. */
    uint32_t args[QC_GLES_INLINE_ARGS];
} qc_gles_args_t;

/*
 * Wire ids.
 *
 * Each call names its function with an id from gles-names.h, the table the guest
 * shims are built from too (GLES_ID_glClear and friends). An id below 822 is the
 * 3.1.3 dispatch slot the host has always decoded (the byte offset of the entry
 * point in that firmware's OpenGLES table, divided by four); later firmwares
 * moved the slots and the shim maps its firmware's slot to the id at load, so
 * the wire never carries a firmware's own numbering. The GLES_SLOT_* names below
 * are the ids the host handles, kept for the switch in gles-host.c.
 */
#define GLES_FN(n, f, id, argc, fl) GLES_ID_##n = id,
enum {
#include "hw/arm/guest-services/gles-names.h"
};
#undef GLES_FN
/* The id's function name, for the log; "?" for an id no row has. */
static inline const char *gles_id_name(uint32_t id)
{
#define GLES_FN(n, f, id_, argc, fl) case id_: return #n;
    switch (id) {
#include "hw/arm/guest-services/gles-names.h"
    default: return "?";
    }
#undef GLES_FN
}
#define GLES_SLOT_CLEAR_STENCIL                GLES_ID_glClearStencil
#define GLES_SLOT_COLOR4UB                     GLES_ID_glColor4ub
#define GLES_SLOT_CULL_FACE                    GLES_ID_glCullFace
#define GLES_SLOT_NORMAL3F                     GLES_ID_glNormal3f
#define GLES_SLOT_POINT_SIZE                   GLES_ID_glPointSize
#define GLES_SLOT_POLYGON_OFFSET               GLES_ID_glPolygonOffset
#define GLES_SLOT_STENCIL_FUNC                 GLES_ID_glStencilFunc
#define GLES_SLOT_STENCIL_OP                   GLES_ID_glStencilOp
#define GLES_SLOT_MULTI_TEX_COORD4F            GLES_ID_glMultiTexCoord4f
#define GLES_SLOT_SAMPLE_COVERAGE              GLES_ID_glSampleCoverage
#define GLES_SLOT_ALPHA_FUNCX                  GLES_ID_glAlphaFuncx
#define GLES_SLOT_CLEAR_COLORX                 GLES_ID_glClearColorx
#define GLES_SLOT_CLEAR_DEPTHX                 GLES_ID_glClearDepthx
#define GLES_SLOT_COLOR4X                      GLES_ID_glColor4x
#define GLES_SLOT_DEPTH_RANGEF                 GLES_ID_glDepthRangef
#define GLES_SLOT_DEPTH_RANGEX                 GLES_ID_glDepthRangex
#define GLES_SLOT_FRUSTUMX                     GLES_ID_glFrustumx
#define GLES_SLOT_LINE_WIDTHX                  GLES_ID_glLineWidthx
#define GLES_SLOT_NORMAL3X                     GLES_ID_glNormal3x
#define GLES_SLOT_ORTHOX                       GLES_ID_glOrthox
#define GLES_SLOT_POINT_SIZEX                  GLES_ID_glPointSizex
#define GLES_SLOT_POLYGON_OFFSETX              GLES_ID_glPolygonOffsetx
#define GLES_SLOT_ROTATEX                      GLES_ID_glRotatex
#define GLES_SLOT_TRANSLATEX                   GLES_ID_glTranslatex
#define GLES_SLOT_MULTI_TEX_COORD4X            GLES_ID_glMultiTexCoord4x
#define GLES_SLOT_POINT_SIZE_POINTER_OES       GLES_ID_glPointSizePointerOES
/* One-to-one forwards added 2026-09-27; each was a silent stub before. */
#define GLES_SLOT_COPY_TEX_SUB_IMAGE_2D        GLES_ID_glCopyTexSubImage2D
#define GLES_SLOT_IS_ENABLED                   GLES_ID_glIsEnabled
#define GLES_SLOT_IS_TEXTURE                   GLES_ID_glIsTexture
#define GLES_SLOT_LIGHT_MODELF                 GLES_ID_glLightModelf
#define GLES_SLOT_LIGHT_MODELFV                GLES_ID_glLightModelfv
#define GLES_SLOT_LIGHTF                       GLES_ID_glLightf
#define GLES_SLOT_LOGIC_OP                     GLES_ID_glLogicOp
#define GLES_SLOT_BLEND_FUNC_SEPARATE          GLES_ID_glBlendFuncSeparate
#define GLES_SLOT_BLEND_EQUATION               GLES_ID_glBlendEquation
#define GLES_SLOT_BLEND_EQUATION_SEPARATE      GLES_ID_glBlendEquationSeparate
#define GLES_SLOT_POINT_PARAMETERF             GLES_ID_glPointParameterf
#define GLES_SLOT_POINT_PARAMETERFV            GLES_ID_glPointParameterfv
#define GLES_SLOT_CLIP_PLANEF                  GLES_ID_glClipPlanef
/* OES_draw_texture: s/i/x/sv/iv/xv/f/fv, in that order. */
#define GLES_SLOT_DRAW_TEXS_OES                GLES_ID_glDrawTexsOES
#define GLES_SLOT_DRAW_TEXI_OES                GLES_ID_glDrawTexiOES
#define GLES_SLOT_DRAW_TEXX_OES                GLES_ID_glDrawTexxOES
#define GLES_SLOT_DRAW_TEXSV_OES               GLES_ID_glDrawTexsvOES
#define GLES_SLOT_DRAW_TEXIV_OES               GLES_ID_glDrawTexivOES
#define GLES_SLOT_DRAW_TEXXV_OES               GLES_ID_glDrawTexxvOES
#define GLES_SLOT_DRAW_TEXF_OES                GLES_ID_glDrawTexfOES
#define GLES_SLOT_DRAW_TEXFV_OES               GLES_ID_glDrawTexfvOES
#define GLES_SLOT_SAMPLE_COVERAGEX             GLES_ID_glSampleCoveragex
#define GLES_SLOT_GET_BOOLEANV                 GLES_ID_glGetBooleanv
#define GLES_SLOT_GET_POINTERV                 GLES_ID_glGetPointerv
#define GLES_SLOT_LOAD_MATRIXX                 GLES_ID_glLoadMatrixx
#define GLES_SLOT_MULT_MATRIXX                 GLES_ID_glMultMatrixx
#define GLES_SLOT_COLOR_MASK                   GLES_ID_glColorMask
#define GLES_SLOT_STENCIL_MASK                 GLES_ID_glStencilMask
#define GLES_SLOT_IS_RENDERBUFFER              GLES_ID_glIsRenderbuffer
#define GLES_SLOT_IS_FRAMEBUFFER               GLES_ID_glIsFramebuffer
#define GLES_SLOT_GENERATE_MIPMAP              GLES_ID_glGenerateMipmap
#define GLES_SLOT_BIND_TEXTURE                 GLES_ID_glBindTexture    /* 0x0024 */
#define GLES_SLOT_CLEAR                        GLES_ID_glClear   /* 0x0038 */
#define GLES_SLOT_CLEAR_COLOR                  GLES_ID_glClearColor   /* 0x0040 */
#define GLES_SLOT_COLOR4F                      GLES_ID_glColor4f   /* 0x00a4 */
#define GLES_SLOT_DISABLE                      GLES_ID_glDisable   /* 0x010c */
#define GLES_SLOT_DISABLE_CLIENT_STATE         GLES_ID_glDisableClientState   /* 0x0110 */
#define GLES_SLOT_DRAW_ARRAYS                  GLES_ID_glDrawArrays   /* 0x0114 */
#define GLES_SLOT_ENABLE                       GLES_ID_glEnable   /* 0x0130 */
#define GLES_SLOT_ENABLE_CLIENT_STATE          GLES_ID_glEnableClientState   /* 0x0134 */
#define GLES_SLOT_FINISH                       GLES_ID_glFinish   /* 0x0174 */
#define GLES_SLOT_FLUSH                        GLES_ID_glFlush   /* 0x0178 */
#define GLES_SLOT_GEN_TEXTURES                 GLES_ID_glGenTextures   /* 0x0198 */
#define GLES_SLOT_GET_ERROR                    GLES_ID_glGetError  /* 0x01a8 */
#define GLES_SLOT_LOAD_IDENTITY                GLES_ID_glLoadIdentity  /* 0x0284 */
#define GLES_SLOT_MATRIX_MODE                  GLES_ID_glMatrixMode  /* 0x02c8 */
#define GLES_SLOT_TEXCOORD_POINTER             GLES_ID_glTexCoordPointer  /* 0x0494 */
#define GLES_SLOT_TEX_IMAGE_2D                 GLES_ID_glTexImage2D  /* 0x04c4 */
#define GLES_SLOT_GET_LIGHTFV                  GLES_ID_glGetLightfv
#define GLES_SLOT_GET_MATERIALFV               GLES_ID_glGetMaterialfv
#define GLES_SLOT_GET_TEX_ENVFV                GLES_ID_glGetTexEnvfv
#define GLES_SLOT_GET_TEX_ENVIV                GLES_ID_glGetTexEnviv
#define GLES_SLOT_GET_TEX_PARAMETERFV          GLES_ID_glGetTexParameterfv
#define GLES_SLOT_GET_TEX_PARAMETERIV          GLES_ID_glGetTexParameteriv
#define GLES_SLOT_TEX_ENVIV                    GLES_ID_glTexEnviv
#define GLES_SLOT_TEX_PARAMETERF               GLES_ID_glTexParameterf
#define GLES_SLOT_TEX_PARAMETERFV              GLES_ID_glTexParameterfv
#define GLES_SLOT_TEX_PARAMETERIV              GLES_ID_glTexParameteriv
#define GLES_SLOT_TEX_PARAMETERI               GLES_ID_glTexParameteri  /* 0x04d0 */
#define GLES_SLOT_VERTEX_POINTER               GLES_ID_glVertexPointer  /* 0x0548 */
#define GLES_SLOT_VIEWPORT                     GLES_ID_glViewport  /* 0x054c */
#define GLES_SLOT_ORTHOF                       GLES_ID_glOrthof  /* 0x0c6c */

/*
 * The fixed-function set a real ES 1.1 game needs on top of the bring-up subset
 * above: matrix stack, lighting, fog, blending, depth, and the remaining client
 * arrays. Every one of these was recovered the same way as the slots above --
 * by disassembling the armv6 slice of the 3.1.3 SDK's OpenGLES and reading the
 * `ldr pc, [ctx, #off]` out of each trampoline -- and the extractor was
 * validated by reproducing all 23 of the already-known slots above exactly
 * before any new number was trusted.
 *
 * The set is not a guess at what games use: it is `nm -u` on Cube Runner,
 * which imports exactly 41 gl* symbols and no others.
 */
#define GLES_SLOT_BLEND_FUNC                   GLES_ID_glBlendFunc    /* 0x002c */
#define GLES_SLOT_COLOR_POINTER                GLES_ID_glColorPointer   /* 0x00dc */
#define GLES_SLOT_DEPTH_MASK                   GLES_ID_glDepthMask   /* 0x0104 */
#define GLES_SLOT_DRAW_ELEMENTS                GLES_ID_glDrawElements   /* 0x011c */
#define GLES_SLOT_FOGF                         GLES_ID_glFogf   /* 0x017c */
#define GLES_SLOT_FOGFV                        GLES_ID_glFogfv   /* 0x0180 */
#define GLES_SLOT_HINT                         GLES_ID_glHint  /* 0x0210 */
#define GLES_SLOT_LIGHTFV                      GLES_ID_glLightfv  /* 0x026c */
#define GLES_SLOT_LINE_WIDTH                   GLES_ID_glLineWidth  /* 0x027c */
#define GLES_SLOT_MATERIALFV                   GLES_ID_glMaterialfv  /* 0x02bc */
#define GLES_SLOT_MATERIALF                    GLES_ID_glMaterialf  /* 0x02b8 */

/*
 * Calls the shim used to drop on the floor. Each one was silently unimplemented
 * on BOTH sides, which is worse than it sounds: the host's unhandled-slot
 * warning cannot fire for a call the shim never forwards, so these were
 * invisible from the QEMU log no matter how verbose it was set.
 *
 * glCopyTexImage2D is the one that mattered. Labyrinth builds its render target
 * by copying the framebuffer into a texture, so dropping it left that texture
 * with NO storage -- and once framebuffer objects became real, an FBO with a
 * storage-less texture attached reports INCOMPLETE, which the app treats as
 * fatal. It is the reason Labyrinth went from a white level to a crash on Play.
 */
#define GLES_SLOT_COPY_TEX_IMAGE_2D            GLES_ID_glCopyTexImage2D  /* 0x00d8 */
#define GLES_SLOT_GET_FLOATV                   GLES_ID_glGetFloatv  /* 0x019c */
#define GLES_SLOT_READ_PIXELS                  GLES_ID_glReadPixels  /* 0x03b4 */
#define GLES_SLOT_TEX_ENVF                     GLES_ID_glTexEnvf  /* 0x0498 */
#define GLES_SLOT_TEX_ENVFV                    GLES_ID_glTexEnvfv  /* 0x049c */
#define GLES_SLOT_TEX_PARAMETERX               GLES_ID_glTexParameterx  /* 0x0c7c */
#define GLES_SLOT_MULT_MATRIXF                 GLES_ID_glMultMatrixf  /* 0x02d0 */
#define GLES_SLOT_NORMAL_POINTER               GLES_ID_glNormalPointer  /* 0x0300 */
#define GLES_SLOT_POP_MATRIX                   GLES_ID_glPopMatrix  /* 0x0344 */
#define GLES_SLOT_PUSH_MATRIX                  GLES_ID_glPushMatrix  /* 0x0358 */
#define GLES_SLOT_ROTATEF                      GLES_ID_glRotatef  /* 0x03f0 */
#define GLES_SLOT_SCALEF                       GLES_ID_glScalef  /* 0x03f8 */
#define GLES_SLOT_SHADE_MODEL                  GLES_ID_glShadeModel  /* 0x0404 */
#define GLES_SLOT_TRANSLATEF                   GLES_ID_glTranslatef  /* 0x04e4 */
#define GLES_SLOT_CLEAR_DEPTHF                 GLES_ID_glClearDepthf  /* 0x0bfc */
#define GLES_SLOT_FRUSTUMF                     GLES_ID_glFrustumf  /* 0x0c20 */

/*
 * The remainder of Super Monkey Ball's import set (`nm -u` lists 49 gl*
 * symbols; these seven were the only ones missing). Same provenance as above:
 * read out of the 3.1.3 SDK trampolines, extractor re-validated against the
 * known slots first. glLoadMatrixf and glTexSubImage2D are the load-bearing
 * two -- a console-engine port sets matrices wholesale and streams textures
 * into pre-allocated storage, and with both silently dropped every draw lands
 * off-screen or samples an incomplete (white) texture.
 */
#define GLES_SLOT_ALPHA_FUNC                   GLES_ID_glAlphaFunc    /* 0x0014 */
#define GLES_SLOT_DELETE_TEXTURES              GLES_ID_glDeleteTextures   /* 0x00fc */
#define GLES_SLOT_GET_INTEGERV                 GLES_ID_glGetIntegerv  /* 0x01b0 */
#define GLES_SLOT_LOAD_MATRIXF                 GLES_ID_glLoadMatrixf  /* 0x028c */
#define GLES_SLOT_TEX_SUB_IMAGE_2D             GLES_ID_glTexSubImage2D  /* 0x04dc */
#define GLES_SLOT_BIND_BUFFER                  GLES_ID_glBindBuffer  /* 0x0a18 */
#define GLES_SLOT_SCALEX                       GLES_ID_glScalex  /* 0x0c80 */

/*
 * Cheap state setters a survey of 20 real App Store apps found imported but
 * unimplemented. Same provenance as everything above -- read out of the 3.1.3
 * SDK trampolines, with the extractor re-validated against seven already-known
 * slots (7, 1, 61, 253, 301, 307, 335) before any new number was trusted.
 *
 * glPixelStorei is the one with teeth. ES 1.1's GL_UNPACK_ALIGNMENT defaults to
 * 4, so a guest uploading GL_RGB, GL_LUMINANCE, GL_ALPHA or GL_LUMINANCE_ALPHA
 * at a width whose row is not a multiple of four bytes pads every row -- and
 * the upload path here used to size the fetch as w*h*bpp, with no padding at
 * all. That under-reads, and each row after the first is sheared by a
 * progressively larger offset. Honouring the alignment is not about supporting
 * the call; it is a correctness fix to uploads that were already wrong.
 */
#define GLES_SLOT_DEPTH_FUNC                   GLES_ID_glDepthFunc   /* 0x0100 */
#define GLES_SLOT_FRONT_FACE                   GLES_ID_glFrontFace   /* 0x018c */
#define GLES_SLOT_PIXEL_STOREI                 GLES_ID_glPixelStorei  /* 0x031c */
#define GLES_SLOT_SCISSOR                      GLES_ID_glScissor  /* 0x03fc */
#define GLES_SLOT_TEX_ENVI                     GLES_ID_glTexEnvi  /* 0x04a0 */
#define GLES_SLOT_CLIENT_ACTIVE_TEXTURE        GLES_ID_glClientActiveTexture  /* 0x0558 */
#define GLES_SLOT_ACTIVE_TEXTURE               GLES_ID_glActiveTexture  /* 0x055c */

/*
 * Compressed textures and buffer objects.
 *
 * Read out of the 3.1.3 SDK trampolines like everything above, and
 * cross-checked against contrib/it-gles/slotmap.txt; the extractor was
 * re-validated here by re-deriving GLES_SLOT_BIND_BUFFER (642, `ldr r12,
 * [r3, #0xa18]`) before any neighbouring number was trusted.
 *
 * glCompressedTexImage2D is what a PowerVR-era title uploads its art with --
 * PVRTC was THE texture format on the MBX -- and it takes eight scalars, so it
 * spills exactly like glTexImage2D. The host has no PVRTC, so it decodes to
 * RGBA8 on the CPU; see gles-host.c.
 *
 * The buffer-object four close the asymmetry glBindBuffer was left in: the bind
 * was accepted with no way to ever put data behind it, so any engine keeping
 * geometry in a VBO drew from memory the host never received.
 */
#define GLES_SLOT_COMPRESSED_TEX_IMAGE_2D      GLES_ID_glCompressedTexImage2D  /* 0x0600 */
#define GLES_SLOT_COMPRESSED_TEX_SUB_IMAGE_2D  GLES_ID_glCompressedTexSubImage2D  /* 0x060c */
#define GLES_SLOT_DELETE_BUFFERS               GLES_ID_glDeleteBuffers  /* 0x0a1c */
#define GLES_SLOT_GEN_BUFFERS                  GLES_ID_glGenBuffers  /* 0x0a20 */
#define GLES_SLOT_BUFFER_DATA                  GLES_ID_glBufferData  /* 0x0a28 */
#define GLES_SLOT_BUFFER_SUB_DATA              GLES_ID_glBufferSubData  /* 0x0a2c */

/* OES framebuffer-object entry points. EAGL uses these itself inside
 * -renderbufferStorage:fromDrawable:, so a real CAEAGLLayer client cannot get
 * off the ground without them -- they are not optional coverage. The non-OES
 * and OES spellings share a slot (the framework aliases them). */
#define GLES_SLOT_BIND_RENDERBUFFER            GLES_ID_glBindRenderbuffer  /* 0x0a78 */
#define GLES_SLOT_DELETE_RENDERBUFFERS         GLES_ID_glDeleteRenderbuffers  /* 0x0a7c */
#define GLES_SLOT_GEN_RENDERBUFFERS            GLES_ID_glGenRenderbuffers  /* 0x0a80 */
#define GLES_SLOT_RENDERBUFFER_STORAGE         GLES_ID_glRenderbufferStorage  /* 0x0a84 */
#define GLES_SLOT_GET_RB_PARAMETERIV           GLES_ID_glGetRenderbufferParameteriv  /* 0x0a88 */
#define GLES_SLOT_BIND_FRAMEBUFFER             GLES_ID_glBindFramebuffer  /* 0x0a90 */
#define GLES_SLOT_DELETE_FRAMEBUFFERS          GLES_ID_glDeleteFramebuffers  /* 0x0a94 */
#define GLES_SLOT_GEN_FRAMEBUFFERS             GLES_ID_glGenFramebuffers  /* 0x0a98 */
#define GLES_SLOT_CHECK_FB_STATUS              GLES_ID_glCheckFramebufferStatus  /* 0x0a9c */
#define GLES_SLOT_FB_TEXTURE_2D                GLES_ID_glFramebufferTexture2D  /* 0x0aa4 */
#define GLES_SLOT_FB_RENDERBUFFER              GLES_ID_glFramebufferRenderbuffer  /* 0x0aac */
#define GLES_SLOT_GET_FB_ATTACH_PARAM          GLES_ID_glGetFramebufferAttachmentParameteriv  /* 0x0ab0 */

/* Ids stop at GLES_ID_MAX (< 0x1000). Engine-level operations that are not
 * GL functions at all live above them. */
#define GLES_OP_BASE                    0x1000
/* Debug present: blit straight to where the LCD scans out. Bypasses
 * CoreAnimation, so whatever CA draws next overwrites it. */
#define GLES_OP_PRESENT                 (GLES_OP_BASE + 0)
/*
 * Real present: write the frame into a caller-supplied CPU-addressable buffer.
 *
 * args: base (guest VA), stride in bytes, width, height, format
 *
 * This is the shape CoreAnimation's IOSurface has. The engine is a pure
 * consumer of that surface -- all ten of the stock bundle's IOSurface imports
 * are read-side (Get*, Lock/Unlock; there is no IOSurfaceCreate anywhere in
 * it) -- so the host never needs to know what an IOSurface is. It is handed an
 * address, a stride and a format, and it writes pixels.
 */
#define GLES_OP_PRESENT_SURFACE         (GLES_OP_BASE + 1)

/*
 * The shim's own log, forwarded to the host.
 *
 * args: guest pointer to bytes, length
 *
 * The shim writes to fd 2, which for an app launched by SpringBoard goes
 * nowhere anybody can read. That invisibility has cost real debugging time more
 * than once: whether CoreAnimation ACCEPTED a surface, and whether it accepted
 * the frames presented into it, are both decided guest-side and were only ever
 * reported there. The host cannot infer either.
 */
#define GLES_OP_LOG                     (GLES_OP_BASE + 2)
/* target, base, stride, width, height, FourCC, UV base, UV stride[, IOSurface ID].
 * A zero base detaches the currently bound texture from guest memory.
 * GLES_SURFACE_WINDOW_ORDER in the target: the memory's first row is the texture's LAST (an
 * EGL pixmap's order, 1.x/2.x's render targets: row 0 is the top of the screen, GL's y=0 the
 * bottom), so the upload and the write-back each reverse the rows. An older host refuses it
 * as a target it does not know. The ID (when sent, and not 0) keys the host's record of the
 * surface's pages. */
#define GLES_OP_BIND_SURFACE            (GLES_OP_BASE + 3)
#define GLES_SURFACE_WINDOW_ORDER       0x80000000u
/* Native context lifecycle. New operations return opaque positive handles;
 * a zero sharegroup handle means the backend only supports legacy contexts. */
#define GLES_OP_NEW_SHAREGROUP          (GLES_OP_BASE + 4)
#define GLES_OP_DELETE_SHAREGROUP       (GLES_OP_BASE + 5)
#define GLES_OP_NEW_CONTEXT             (GLES_OP_BASE + 6)
#define GLES_OP_DELETE_CONTEXT          (GLES_OP_BASE + 7)
/* Accepted CA drawable width and height, before GL size queries/draws.
 * Old shims retain the panel-sized default. The request ABI is unchanged. */
#define GLES_OP_DRAWABLE_STORAGE        (GLES_OP_BASE + 8)

/*
 * A command buffer of calls the guest queued (contrib/ipad1-gles/glishim.c):
 * args[0] = its guest VA, args[1] = its length in 32-bit words. Each record is
 * [slot | argc << 16, argc args], run in order as if it had trapped alone.
 * Only calls that return nothing and pass no guest pointer are queued.
 */
#define GLES_OP_BATCH                   (GLES_OP_BASE + 9)
#define GLES_BATCH_MAX_WORDS            4096

/* Surface pixel formats, as IOSurfaceGetPixelFormat reports them (FourCC). */
#define GLES_SURFACE_BGRA32             0x42475241  /* 'BGRA' */
#define GLES_SURFACE_RGBA32             0x52474241  /* 'RGBA' */
/* 16 bits per pixel, not 32. A CAEAGLLayer that asked for
 * kEAGLColorFormatRGB565 gets this, and every size in the present path has to
 * follow the format rather than assume four bytes. */
#define GLES_SURFACE_RGB555 0x4c353535 /* L555: opaque little-endian RGB555 */
#define GLES_SURFACE_RGB565             0x4c353635  /* 'L565' */
/* 8 bits of alpha per pixel: CoreAnimation's shadow masks (a popover's, a layer's shadowPath). */
#define GLES_SURFACE_A8                 0x41303038  /* 'A008' */
/* The rest of what QuartzCore and IOSurface name (immediates in every firmware from 5F138
 * to 8C148): 8-bit luminance, the two packed 16-bit RGBA orders and the 32-bit orders. */
#define GLES_SURFACE_L8                 0x4c303038  /* 'L008' */
#define GLES_SURFACE_RGBA4444           0x34343434  /* '4444' */
#define GLES_SURFACE_RGBA5551           0x31353535  /* '1555' */
#define GLES_SURFACE_ARGB32             0x41524742  /* 'ARGB' */
#define GLES_SURFACE_ABGR32             0x41424752  /* 'ABGR' */

#ifndef OUT_OF_TREE_BUILD
int64_t qc_handle_gles(CPUState *cpu, qc_gles_args_t *a);
/* Guest-pointer access for the GL host: faults untouched pages in (guest-gles.c). */
int gles_guest_rw(CPUState *cpu, vaddr va, void *buf, size_t len, bool write);
bool gles_guest_fault_pending(void);
void qc_gles_dump_stats(void);
int64_t gles_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx,
                       uint32_t argc, const uint32_t *args);
void gles_host_stats(uint64_t *draws, uint64_t *presents);
void gles_host_reset(void);
int gles_host_context_count(void);
/* Every refusal the bridge makes, by name ("surface:A008", "teximage:0x1906/0x8033",
 * "slot:807"), counted per call and logged once; a guest shim reports its own through
 * GLES_OP_LOG as "[gles-reject] NAME COUNT". True the first time a name is seen, so the
 * caller can add detail to the log. */
bool gles_host_refuse(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
/* "NAME\tCOUNT\n" per name, sorted: the machine's gles-rejects property. Caller frees. */
char *gles_host_rejects(void);
/* gles-debug=on: a refused texture or surface samples magenta and a refused draw paints
 * the viewport magenta, so a screenshot shows the gap; off, the least-bad fallback stays. */
void gles_host_set_debug(bool on);
bool gles_host_debug(void);
/* The newest write generation the bridge recorded on RAM [addr, addr + len): it clears QEMU's
 * VGA dirty bits of every surface page it checks (its own write-backs included), so a display
 * model that scans such pages out through the dirty log compares this too, or misses frames. */
uint64_t gles_host_ram_gen(uint64_t addr, uint64_t len);
#endif

#endif /* HW_ARM_GUEST_SERVICES_GLES_H */
