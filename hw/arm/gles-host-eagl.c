/*
 * OpenGL ES 1.1 high-level emulation -- the iOS half of the host context.
 *
 * gles-host.c is the renderer and it is platform-neutral; this file is the one
 * thing it cannot do for itself, which is ask iOS for a context. On macOS the
 * same two functions are a dozen lines of CGL inside gles-host.c.
 *
 * WHY THIS IS ALLOWED TO BE SO SMALL. The guest is an iPod touch running iOS
 * 3.1.3, so the API the guest issues -- fixed-function GLES 1.1, glMatrixMode
 * and glOrthof and glLightfv -- is still exactly the API OpenGLES.framework
 * exports today (verified against the iPhoneOS 26.5 SDK: 457 entry points, all
 * of the guest's among them). There is no shader translation anywhere in this
 * port. Nor does anything here have to emulate a drawable: the "swap" in this
 * design is a copy into the guest's own CoreAnimation surface (see
 * gles_present_to_surface), so the host context never needs a CAEAGLLayer, a
 * window or presentRenderbuffer. An offscreen FBO in a bare EAGLContext is the
 * entire requirement -- with, since the present has to be cheap on a tiler, an
 * IOSurface for its colour attachment rather than plain texture storage. That
 * second half is the rest of this file.
 *
 * WHY C AND objc_msgSend RATHER THAN AN .m FILE. This is three message sends,
 * and writing them in Objective-C costs more than it looks. meson picks a
 * target's linker driver from the languages of its sources -- and of anything
 * it links -- so a single .m anywhere in qemu-system-arm switches the link from
 * c_LINKER to objc_LINKER. meson then applies objc_link_args, which configure
 * never writes (it writes c_link_args and cpp_link_args only), so every
 * --extra-ldflags the iOS cross build depends on -- the sysroot's -L, its
 * libcrypto.a -- silently disappears and the link dies on the first library it
 * can no longer find. Both ways out of that were tried and neither works from
 * inside hw/arm/meson.build: a separate static_library propagates its language
 * to whatever links it, and so does handing over its extracted objects.
 *
 * The proper fix is one line in configure, teaching it to emit objc_link_args
 * beside c_link_args; with that this file could be an .m and read a little
 * better. It would not do anything different. What it does now is the same
 * three messages the compiler would have emitted, and it is a leaf: nothing
 * else in the tree calls it, so the cost of the idiom is contained here.
 *
 * THREADING. EAGL's current context is thread-local, exactly like CGL's. Every
 * QC_GLES request arrives on the one vCPU thread that traps out of the guest,
 * so the context is created on that thread and stays current on it; no other
 * thread in the process touches GL. gles_eagl_make_current is the same cheap
 * thread-local comparison the CGL path does rather than an unconditional call
 * into the GL stack.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */

#include "qemu/osdep.h"

#include <objc/message.h>
#include <objc/runtime.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

bool gles_eagl_context_create(void);
void gles_eagl_make_current(void);
bool gles_eagl_iosurface_bind(uint32_t width, uint32_t height,
                              unsigned long target,
                              unsigned long internal_format,
                              unsigned long format, unsigned long type);
void *gles_eagl_iosurface_lock(size_t *stride);
void gles_eagl_iosurface_unlock(void);

/* EAGLRenderingAPI, from <OpenGLES/EAGL.h>. ES 1.1 specifically: an ES 2.0
 * context would create happily and then be unable to execute a single one of
 * the guest's calls, because it has no fixed-function pipeline at all. There
 * would be nothing to fall back to it FOR. */
#define EAGL_RENDERING_API_GLES1 1

/* objc_msgSend has no callable prototype on arm64 -- it must be cast to the
 * exact signature of the method being sent, because the ABI for the call is
 * the method's, not a variadic one. */
typedef id (*msg_id_t)(id, SEL);
typedef id (*msg_id_ul_t)(id, SEL, unsigned long);
typedef BOOL (*msg_bool_id_t)(id, SEL, id);
typedef void (*msg_void_t)(id, SEL);
typedef BOOL (*msg_bool_sel_t)(id, SEL, SEL);

/*
 * -[EAGLContext texImageIOSurface:target:internalFormat:width:height:
 *               format:type:plane:]
 *
 * Ten arguments including self and _cmd, so the last two land on the stack --
 * which is precisely why the prototype has to match the method's declared types
 * exactly rather than approximately. NSUInteger is unsigned long, the width,
 * height and plane arguments are genuinely uint32_t, and getting either wrong
 * would put `plane` somewhere the callee does not look.
 */
typedef BOOL (*msg_teximage_t)(id, SEL, IOSurfaceRef, unsigned long,
                               unsigned long, uint32_t, uint32_t,
                               unsigned long, unsigned long, uint32_t);

/*
 * Deliberately never released, matching the CGL path: the context lives for
 * the life of the process, and tearing it down from a thread it may not be
 * current on is a worse hazard than the leak.
 */
static id gles_ctx;
static Class gles_eagl_class;

bool gles_eagl_context_create(void)
{
    id fresh;

    if (gles_ctx) {
        return true;
    }

    /* Not dlopen'd or weak-linked: the machine is linked against
     * OpenGLES.framework, so if the class is missing the process would not
     * have started. The check is for the day Apple withdraws it. */
    gles_eagl_class = objc_getClass("EAGLContext");
    if (!gles_eagl_class) {
        fprintf(stderr, "[gles] no EAGLContext class; the guest will fall "
                "back to software GL\n");
        return false;
    }

    /* [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES1] */
    fresh = ((msg_id_t)objc_msgSend)((id)gles_eagl_class,
                                     sel_registerName("alloc"));
    gles_ctx = fresh ?
        ((msg_id_ul_t)objc_msgSend)(fresh, sel_registerName("initWithAPI:"),
                                    EAGL_RENDERING_API_GLES1) : NULL;
    if (!gles_ctx) {
        /* ES 1.1 has been deprecated since iOS 12 and initWithAPI: answers nil
         * rather than failing loudly when an API level is not supported. */
        fprintf(stderr, "[gles] EAGL ES 1.1 context unavailable on this OS; "
                "the guest will fall back to software GL\n");
        return false;
    }

    /* [EAGLContext setCurrentContext:ctx] */
    if (!((msg_bool_id_t)objc_msgSend)((id)gles_eagl_class,
                                       sel_registerName("setCurrentContext:"),
                                       gles_ctx)) {
        fprintf(stderr, "[gles] EAGL setCurrentContext failed\n");
        ((msg_void_t)objc_msgSend)(gles_ctx, sel_registerName("release"));
        gles_ctx = NULL;
        return false;
    }

    return true;
}

void gles_eagl_make_current(void)
{
    id cur = ((msg_id_t)objc_msgSend)((id)gles_eagl_class,
                                      sel_registerName("currentContext"));

    if (cur != gles_ctx) {
        ((msg_bool_id_t)objc_msgSend)((id)gles_eagl_class,
                                      sel_registerName("setCurrentContext:"),
                                      gles_ctx);
    }
}

/* ---------------------------------------------------------------- IOSurface */

/*
 * The render target the CPU can read without asking GL for it.
 *
 * The frame has to reach guest memory every present -- the guest's
 * CoreAnimation owns the destination and the request is synchronous -- so the
 * pixels cross the CPU/GPU line either way. What is negotiable is HOW. A
 * glReadPixels out of an ordinary texture attachment is the worst available
 * answer on a tile-based GPU: it forces the tiler to resolve on demand, into
 * driver-owned staging the CPU then copies again, with a full pipeline drain in
 * front of it. An IOSurface attachment is the same render target with system
 * memory as its backing store, so once the GPU is done the pixels are simply
 * THERE and the present is a memcpy out of a mapped page.
 *
 * ORDERING. IOSurfaceLock without kIOSurfaceLockAvoidSync is the synchronising
 * read: that flag exists to let a caller BAIL OUT of "a potentially expensive
 * paging operation (such as readback from a GPU to system memory)", which is
 * Apple documenting that the default lock performs exactly that wait. So the
 * lock is the guarantee, and it is a better one than glFinish -- it waits for
 * the work that touches THIS surface rather than draining every queue in the
 * context. The caller still owes it a glFlush first, because a lock can only
 * wait for commands the driver has actually been given.
 */
static IOSurfaceRef gles_surf;

static void gles_dict_set_int(CFMutableDictionaryRef d, CFStringRef key, int v)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);

    if (n) {
        CFDictionarySetValue(d, key, n);
        CFRelease(n);
    }
}

bool gles_eagl_iosurface_bind(uint32_t width, uint32_t height,
                              unsigned long target,
                              unsigned long internal_format,
                              unsigned long format, unsigned long type)
{
    CFMutableDictionaryRef props;
    SEL sel = sel_registerName("texImageIOSurface:target:internalFormat:"
                               "width:height:format:type:plane:");

    if (!gles_ctx) {
        return false;
    }
    if (gles_surf) {
        /* Bound once, for the life of the process, like the context. */
        return true;
    }

    /*
     * texImageIOSurface: has been deprecated since iOS 12 and the whole
     * framework is on borrowed time, so ask before sending rather than trap on
     * the day it goes. gles-host.c keeps the glReadPixels path for exactly this
     * case: slow pixels beat no pixels.
     */
    if (!((msg_bool_sel_t)objc_msgSend)(gles_ctx,
                                        sel_registerName("respondsToSelector:"),
                                        sel)) {
        fprintf(stderr, "[gles] EAGL has no texImageIOSurface:; presenting by "
                "readback\n");
        return false;
    }

    props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
    if (!props) {
        return false;
    }
    gles_dict_set_int(props, kIOSurfaceWidth, (int)width);
    gles_dict_set_int(props, kIOSurfaceHeight, (int)height);
    gles_dict_set_int(props, kIOSurfaceBytesPerElement, 4);
    /* 'BGRA' -- the byte order CoreAnimation uses in the guest, so the present
     * is a copy and never a swizzle. Deliberately no kIOSurfaceBytesPerRow:
     * IOSurface picks the alignment the GPU wants and the caller reads the
     * result back out of IOSurfaceGetBytesPerRow. */
    gles_dict_set_int(props, kIOSurfacePixelFormat, 0x42475241);

    gles_surf = IOSurfaceCreate(props);
    CFRelease(props);
    if (!gles_surf) {
        fprintf(stderr, "[gles] IOSurfaceCreate failed; presenting by "
                "readback\n");
        return false;
    }

    /* Binds to whichever texture the caller left bound on `target`, exactly as
     * glTexImage2D would -- this REPLACES that call, it does not accompany it. */
    if (!((msg_teximage_t)objc_msgSend)(gles_ctx, sel, gles_surf, target,
                                        internal_format, width, height,
                                        format, type, 0)) {
        fprintf(stderr, "[gles] texImageIOSurface rejected the surface; "
                "presenting by readback\n");
        CFRelease(gles_surf);
        gles_surf = NULL;
        return false;
    }

    return true;
}

/*
 * Returns the frame's first byte, or NULL if the surface cannot be read -- in
 * which case the caller must fall back for this frame rather than present
 * whatever was there before. Rows run bottom-up, GL's way round.
 */
void *gles_eagl_iosurface_lock(size_t *stride)
{
    void *base;

    if (!gles_surf) {
        return NULL;
    }
    if (IOSurfaceLock(gles_surf, kIOSurfaceLockReadOnly, NULL) != 0) {
        return NULL;
    }
    base = IOSurfaceGetBaseAddress(gles_surf);
    if (!base) {
        IOSurfaceUnlock(gles_surf, kIOSurfaceLockReadOnly, NULL);
        return NULL;
    }
    *stride = IOSurfaceGetBytesPerRow(gles_surf);
    return base;
}

void gles_eagl_iosurface_unlock(void)
{
    if (gles_surf) {
        /* Symmetrical with the lock's flags; the header calls anything else
         * undefined behaviour. */
        IOSurfaceUnlock(gles_surf, kIOSurfaceLockReadOnly, NULL);
    }
}
