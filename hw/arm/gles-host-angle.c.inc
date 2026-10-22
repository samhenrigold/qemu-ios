/* Experimental EGL lifetime adapter for the existing guest executor.
 * Each guest context has its own native state; the group retains a root only
 * for the shared namespace. No window/swap or second transport is involved.
 * A guest ES 2.0 context is an ANGLE ES 2.0 (or later) context, the rest ES 1.1:
 * ANGLE runs no fixed function in a programmable context, nor shaders in an ES1 one. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef _WIN32
/* libEGL.dll and libGLESv2.dll: the exe's own directory is the first place LoadLibrary looks. */
#define angle_dlopen(path) ((void *)LoadLibraryA(path))
#define angle_dlsym(lib, name) ((void *)GetProcAddress((HMODULE)(lib), name))
#define angle_dlclose(lib) FreeLibrary((HMODULE)(lib))
#define ANGLE_LIB_DIR ""
#define ANGLE_LIB_SUFFIX ".dll"
#define ANGLE_PLATFORM EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE
#else
#include <dlfcn.h>
#define angle_dlopen(path) dlopen(path, RTLD_NOW | RTLD_LOCAL)
#define angle_dlsym dlsym
#define angle_dlclose dlclose
#define ANGLE_LIB_DIR "@rpath/"
#define ANGLE_LIB_SUFFIX ".dylib"
#define ANGLE_PLATFORM EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#endif
#include "gles-host-angle-gl.h"
#define CGLContextObj GLESAngleContextRef
#define CGLSetCurrentContext gles_angle_set_current
#define CGLRetainContext gles_angle_retain
#define CGLReleaseContext gles_angle_release
typedef struct GLESAngleContext {
    EGLContext context;
    EGLSurface surface;
    unsigned refs;
    bool es2;
    GLint row_length;       /* GL_UNPACK_ROW_LENGTH in an ES1 context, which keeps it here (below) */
    /* GL_TEXTURE_RECTANGLE, per unit (below), and the group's rectangle textures' level-0 sizes. */
    GLuint rect[8];
    bool rect_on[8];
    GHashTable *rect_size;
} *CGLContextObj;
static EGLDisplay angle_display = EGL_NO_DISPLAY;
static EGLConfig angle_config;
static __thread CGLContextObj angle_current;

/* libEGL and libGLESv2 beside the emulator (@rpath: the app's Frameworks), else in the ANGLE build it was
 * configured with (GLES_ANGLE_DIR); false, and the bridge stays on CGL, where there is neither. */
static bool gles_angle_load(void)
{
    static int loaded = -1;
    if (loaded >= 0) return loaded;
    void *egl = NULL, *gles = NULL;
    const char *dirs[] = { ANGLE_LIB_DIR,
#ifdef GLES_ANGLE_DIR
        GLES_ANGLE_DIR "/",
#endif
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(dirs) && !(egl && gles); i++) {
        g_autofree char *e = g_strdup_printf("%slibEGL" ANGLE_LIB_SUFFIX, dirs[i]);
        g_autofree char *g = g_strdup_printf("%slibGLESv2" ANGLE_LIB_SUFFIX, dirs[i]);
        if (egl) angle_dlclose(egl);
        if (gles) angle_dlclose(gles);
        egl = angle_dlopen(e);
        gles = egl ? angle_dlopen(g) : NULL;
    }
    loaded = egl && gles;
#define GLES_ANGLE_LOAD(name) \
    if (loaded && !(gles_angle_fn_##name = angle_dlsym(#name[0] == 'e' ? egl : gles, #name))) { \
        fprintf(stderr, "[gles-angle] no %s in ANGLE\n", #name); \
        loaded = 0; \
    }
    GLES_ANGLE_FUNCTIONS(GLES_ANGLE_LOAD)
#undef GLES_ANGLE_LOAD
#ifdef _WIN32
    if (!loaded) fprintf(stderr, "[gles-angle] ANGLE is not available: %s\n", egl && gles ? "entry points" : "libEGL.dll or libGLESv2.dll did not load");
#else
    if (!loaded) fprintf(stderr, "[gles-angle] ANGLE is not available: %s\n", egl && gles ? "entry points" : dlerror());
#endif
    return loaded;
}

static bool gles_angle_create(CGLContextObj share, bool es2, CGLContextObj *out)
{
    if (!gles_angle_load()) return false;
    if (angle_display == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLint attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, ANGLE_PLATFORM, EGL_NONE};
        EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint count;
        if (!get) return false;
        angle_display = get(EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
        if (angle_display == EGL_NO_DISPLAY || !eglInitialize(angle_display, NULL, NULL) ||
            !eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(angle_display, ca, &angle_config, 1, &count) || !count) {
            angle_display = EGL_NO_DISPLAY;
            return false;
        }
    }
    EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, es2 ? 2 : 1, EGL_NONE};
    EGLint surface[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    CGLContextObj c = g_new0(struct GLESAngleContext, 1);
    c->refs = 1;
    c->es2 = es2;
    c->rect_size = share ? g_hash_table_ref(share->rect_size) : g_hash_table_new(g_direct_hash, g_direct_equal);
    c->context = eglCreateContext(angle_display, angle_config, share ? share->context : EGL_NO_CONTEXT, ctx);
    c->surface = eglCreatePbufferSurface(angle_display, angle_config, surface);
    if (c->context == EGL_NO_CONTEXT || c->surface == EGL_NO_SURFACE) {
        if (c->context != EGL_NO_CONTEXT) eglDestroyContext(angle_display, c->context);
        if (c->surface != EGL_NO_SURFACE) eglDestroySurface(angle_display, c->surface);
        g_hash_table_unref(c->rect_size);
        g_free(c);
        return false;
    }
    *out = c;
    return true;
}
static int CGLSetCurrentContext(CGLContextObj c)
{
    if (angle_current == c) return 0;
    if (!eglMakeCurrent(angle_display, c ? c->surface : EGL_NO_SURFACE,
                        c ? c->surface : EGL_NO_SURFACE, c ? c->context : EGL_NO_CONTEXT)) return -1;
    angle_current = c;
    return 0;
}
static CGLContextObj CGLRetainContext(CGLContextObj c) { c->refs++; return c; }
static void CGLReleaseContext(CGLContextObj c)
{
    if (--c->refs) return;
    if (angle_current == c) CGLSetCurrentContext(NULL);
    eglDestroyContext(angle_display, c->context);
    eglDestroySurface(angle_display, c->surface);
    g_hash_table_unref(c->rect_size);
    g_free(c);
}

/* Desktop clip-plane setters receive doubles reconstructed from guest floats. */
static void gles_angle_clip_plane(GLenum plane, const double *value)
{
    GLfloat f[4] = {value[0], value[1], value[2], value[3]};
    glClipPlanef(plane, f);
}
#define glClipPlane gles_angle_clip_plane

/* ES stores an unsized image in its own format (and BGRA texels only in BGRA storage,
 * EXT_texture_format_BGRA8888), where the executor names the desktop's RGBA storage for any of
 * them. Each samples as it did from RGBA storage: RGB and luminance with alpha 1, alpha as (0, 0, 0, a). */
/*
 * ES 1.1 has no GL_UNPACK_ROW_LENGTH, and ANGLE refuses it in an ES1 context (GL_INVALID_ENUM), where the bridge
 * sets it around every upload (gles_unpack_apply, gles_unpack_tight): the error was blamed on the guest's upload,
 * which the bridge then reported failed and uncharged though the host had made it. An ES1 context keeps the row
 * length itself, and an upload with one longer than the image is repacked into tight rows first (the bridge sets
 * GL_UNPACK_ALIGNMENT 1 with a row length, gles_unpack_layout).
 */
static void gles_angle_PixelStorei(GLenum pname, GLint value)
{
    if (pname == GL_UNPACK_ROW_LENGTH && angle_current && !angle_current->es2) {
        angle_current->row_length = value;
        return;
    }
    glPixelStorei(pname, value);
}
#undef glPixelStorei
#define glPixelStorei gles_angle_PixelStorei

/* Bytes in a texel of an ES upload, 0 for anything else. */
static size_t gles_angle_texel_bytes(GLenum fmt, GLenum type)
{
    size_t n = fmt == GL_RGBA || fmt == GL_BGRA_EXT ? 4 : fmt == GL_RGB ? 3 : fmt == GL_LUMINANCE_ALPHA ? 2 :
               fmt == GL_ALPHA || fmt == GL_LUMINANCE ? 1 : 0;
    switch (type) {
    case GL_UNSIGNED_BYTE: return n;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case 0x8D61 /* GL_HALF_FLOAT_OES */: case GL_HALF_FLOAT: return n * 2;
    case GL_FLOAT: return n * 4;
    default: return 0;
    }
}

/* PIXELS as tight rows, in *TIGHT (freed by the caller), where an ES1 context's row length says they aren't. */
static const void *gles_angle_unpack(GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *pixels,
                                     uint8_t **tight)
{
    size_t bpp = gles_angle_texel_bytes(fmt, type);
    GLint rl = angle_current && !angle_current->es2 ? angle_current->row_length : 0;
    *tight = NULL;
    if (!pixels || w <= 0 || h <= 0 || rl <= w || !bpp) return pixels;
    *tight = g_malloc((size_t)w * h * bpp);
    for (GLsizei y = 0; y < h; y++) {
        memcpy(*tight + (size_t)y * w * bpp, (const uint8_t *)pixels + (size_t)y * rl * bpp, (size_t)w * bpp);
    }
    return *tight;
}

static void gles_angle_tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLenum fmt, GLenum type, const void *pixels)
{
    g_autofree uint8_t *tight = NULL;
    if (ifmt == GL_RGBA && (fmt == GL_BGRA_EXT || fmt == GL_RGB || fmt == GL_ALPHA ||
                            fmt == GL_LUMINANCE || fmt == GL_LUMINANCE_ALPHA)) ifmt = fmt;
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, gles_angle_unpack(w, h, fmt, type, pixels, &tight));
}
#undef glTexImage2D
#define glTexImage2D gles_angle_tex_image_2d

/* The framebuffer objects are OES_framebuffer_object in an ES1 context and core in an ES 2.0 one,
 * and ANGLE takes each name only where it belongs. */
static bool gles_angle_es2(void) { return angle_current && angle_current->es2; }
#define gles_fixed_host() (!gles_angle_es2())
static GLboolean gles_angle_IsFramebuffer(GLuint a) { return gles_angle_es2() ? glIsFramebuffer(a) : glIsFramebufferOES(a); }
static GLboolean gles_angle_IsRenderbuffer(GLuint a) { return gles_angle_es2() ? glIsRenderbuffer(a) : glIsRenderbufferOES(a); }
static void gles_angle_GenerateMipmap(GLenum a) { if (gles_angle_es2()) glGenerateMipmap(a); else glGenerateMipmapOES(a); }
static void gles_angle_GenFramebuffers(GLsizei a, GLuint *b) { if (gles_angle_es2()) glGenFramebuffers(a, b); else glGenFramebuffersOES(a, b); }
static void gles_angle_BindFramebuffer(GLenum a, GLuint b) { if (gles_angle_es2()) glBindFramebuffer(a, b); else glBindFramebufferOES(a, b); }
static void gles_angle_FramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e)
{
    if (c == 0x84F5) c = GL_TEXTURE_2D;         /* GL_TEXTURE_RECTANGLE: a 2D texture here (below) */
    if (gles_angle_es2()) glFramebufferTexture2D(a, b, c, d, e); else glFramebufferTexture2DOES(a, b, c, d, e);
}
static void gles_angle_GenRenderbuffers(GLsizei a, GLuint *b) { if (gles_angle_es2()) glGenRenderbuffers(a, b); else glGenRenderbuffersOES(a, b); }
static void gles_angle_BindRenderbuffer(GLenum a, GLuint b) { if (gles_angle_es2()) glBindRenderbuffer(a, b); else glBindRenderbufferOES(a, b); }
static void gles_angle_RenderbufferStorage(GLenum a, GLenum b, GLsizei c, GLsizei d) { if (gles_angle_es2()) glRenderbufferStorage(a, b, c, d); else glRenderbufferStorageOES(a, b, c, d); }
static void gles_angle_FramebufferRenderbuffer(GLenum a, GLenum b, GLenum c, GLuint d) { if (gles_angle_es2()) glFramebufferRenderbuffer(a, b, c, d); else glFramebufferRenderbufferOES(a, b, c, d); }
static GLenum gles_angle_CheckFramebufferStatus(GLenum a) { return gles_angle_es2() ? glCheckFramebufferStatus(a) : glCheckFramebufferStatusOES(a); }
static void gles_angle_DeleteFramebuffers(GLsizei a, const GLuint *b) { if (gles_angle_es2()) glDeleteFramebuffers(a, b); else glDeleteFramebuffersOES(a, b); }
static void gles_angle_DeleteRenderbuffers(GLsizei a, const GLuint *b) { if (gles_angle_es2()) glDeleteRenderbuffers(a, b); else glDeleteRenderbuffersOES(a, b); }
static void gles_angle_GetRenderbufferParameteriv(GLenum a, GLenum b, GLint *c) { if (gles_angle_es2()) glGetRenderbufferParameteriv(a, b, c); else glGetRenderbufferParameterivOES(a, b, c); }
static void gles_angle_GetFramebufferAttachmentParameteriv(GLenum a, GLenum b, GLenum c, GLint *d) { if (gles_angle_es2()) glGetFramebufferAttachmentParameteriv(a, b, c, d); else glGetFramebufferAttachmentParameterivOES(a, b, c, d); }
#undef glIsFramebufferOES
#define glIsFramebufferOES gles_angle_IsFramebuffer
#undef glIsRenderbufferOES
#define glIsRenderbufferOES gles_angle_IsRenderbuffer
#undef glGenerateMipmapOES
#define glGenerateMipmapOES gles_angle_GenerateMipmap
#undef glGenFramebuffersOES
#define glGenFramebuffersOES gles_angle_GenFramebuffers
#undef glBindFramebufferOES
#define glBindFramebufferOES gles_angle_BindFramebuffer
#undef glFramebufferTexture2DOES
#define glFramebufferTexture2DOES gles_angle_FramebufferTexture2D
#undef glGenRenderbuffersOES
#define glGenRenderbuffersOES gles_angle_GenRenderbuffers
#undef glBindRenderbufferOES
#define glBindRenderbufferOES gles_angle_BindRenderbuffer
#undef glRenderbufferStorageOES
#define glRenderbufferStorageOES gles_angle_RenderbufferStorage
#undef glFramebufferRenderbufferOES
#define glFramebufferRenderbufferOES gles_angle_FramebufferRenderbuffer
#undef glCheckFramebufferStatusOES
#define glCheckFramebufferStatusOES gles_angle_CheckFramebufferStatus
#undef glDeleteFramebuffersOES
#define glDeleteFramebuffersOES gles_angle_DeleteFramebuffers
#undef glDeleteRenderbuffersOES
#define glDeleteRenderbuffersOES gles_angle_DeleteRenderbuffers
#undef glGetRenderbufferParameterivOES
#define glGetRenderbufferParameterivOES gles_angle_GetRenderbufferParameteriv
#undef glGetFramebufferAttachmentParameterivOES
#define glGetFramebufferAttachmentParameterivOES gles_angle_GetFramebufferAttachmentParameteriv

static void gles_angle_get_clip_plane(GLenum plane, double *value)
{
    GLfloat f[4];
    glGetClipPlanef(plane, f);
    for (unsigned i = 0; i < 4; i++) value[i] = f[i];
}
#define glGetClipPlane gles_angle_get_clip_plane

/* APPLE_fence is NV_fence with every fence set to "all completed". An object other than a fence
 * is never pending: the executor copies the guest's texels and arrays into the host at each call,
 * so the guest may always reuse the memory it named. */
#define GL_FENCE_APPLE 0x8A0B
#define glGenFencesAPPLE glGenFencesNV
#define glDeleteFencesAPPLE glDeleteFencesNV
#define glSetFenceAPPLE(fence) glSetFenceNV(fence, GL_ALL_COMPLETED_NV)
#define glIsFenceAPPLE glIsFenceNV
#define glTestFenceAPPLE glTestFenceNV
#define glFinishFenceAPPLE glFinishFenceNV
static GLboolean glTestObjectAPPLE(GLenum object, GLuint name)
{
    return object == GL_FENCE_APPLE ? glTestFenceNV(name) : GL_TRUE;
}
static void glFinishObjectAPPLE(GLenum object, GLint name)
{
    if (object == GL_FENCE_APPLE) glFinishFenceNV(name);
}

/*
 * GL_TEXTURE_RECTANGLE (APPLE/ARB_texture_rectangle), which 2.x and 3.x CoreAnimation composites with
 * (contrib/it-gles/gles1x.c offers it) and the executor binds surfaces at, and which ES has no target for.
 * A rectangle texture is a 2D one: its binding and enable are the context's own, per unit, and an
 * operation on the target runs on that texture bound at 2D for the call. Defaults are a rectangle's
 * (linear, clamped, no mipmaps). A draw samples each unit's rectangle where it is on, as desktop GL
 * prefers it to 2D, with the texture matrix scaled by 1/width and 1/height, since rectangle coordinates
 * are texels: the matrix and the unit's 2D binding and enable are put back after.
 */
#define GL_TEXTURE_RECTANGLE_ARB 0x84F5
#define GL_TEXTURE_BINDING_RECTANGLE_ARB 0x84F6
#define GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB 0x84F8
static unsigned gles_angle_unit(void)
{
    GLint unit = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    return (unsigned)(unit - GL_TEXTURE0) % G_N_ELEMENTS(angle_current->rect);
}
/* The current unit's rectangle texture at GL_TEXTURE_2D; the 2D binding it replaced. */
static GLint gles_angle_rect_begin(void)
{
    GLint old = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    glBindTexture(GL_TEXTURE_2D, angle_current->rect[gles_angle_unit()]);
    return old;
}
static void gles_angle_BindTexture(GLenum target, GLuint name)
{
    if (target != GL_TEXTURE_RECTANGLE_ARB || !angle_current) {
        glBindTexture(target, name);
        return;
    }
    bool created = name && !glIsTexture(name);
    angle_current->rect[gles_angle_unit()] = name;
    if (created) {
        GLint old = gles_angle_rect_begin();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, old);
    }
}
static void gles_angle_DeleteTextures(GLsizei n, const GLuint *names)
{
    for (GLsizei i = 0; angle_current && i < n; i++) {
        for (unsigned u = 0; u < G_N_ELEMENTS(angle_current->rect); u++) {
            if (angle_current->rect[u] == names[i]) angle_current->rect[u] = 0;
        }
        g_hash_table_remove(angle_current->rect_size, GUINT_TO_POINTER(names[i]));
    }
    glDeleteTextures(n, names);
}
static void gles_angle_Enable(GLenum cap)
{
    if (cap == GL_TEXTURE_RECTANGLE_ARB && angle_current) angle_current->rect_on[gles_angle_unit()] = true;
    else glEnable(cap);
}
static void gles_angle_Disable(GLenum cap)
{
    if (cap == GL_TEXTURE_RECTANGLE_ARB && angle_current) angle_current->rect_on[gles_angle_unit()] = false;
    else glDisable(cap);
}
static GLboolean gles_angle_IsEnabled(GLenum cap)
{
    if (cap == GL_TEXTURE_RECTANGLE_ARB && angle_current) return angle_current->rect_on[gles_angle_unit()];
    return glIsEnabled(cap);
}
/* The rectangle queries, or false for any other name. */
static bool gles_angle_rect_get(GLenum pname, GLint *v)
{
    if (!angle_current) return false;
    switch (pname) {
    case GL_TEXTURE_RECTANGLE_ARB: *v = angle_current->rect_on[gles_angle_unit()]; return true;
    case GL_TEXTURE_BINDING_RECTANGLE_ARB: *v = angle_current->rect[gles_angle_unit()]; return true;
    case GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB: glGetIntegerv(GL_MAX_TEXTURE_SIZE, v); return true;
    default: return false;
    }
}
static void gles_angle_GetIntegerv(GLenum pname, GLint *v)
{
    if (!gles_angle_rect_get(pname, v)) glGetIntegerv(pname, v);
}
static void gles_angle_GetFloatv(GLenum pname, GLfloat *v)
{
    GLint i;
    if (gles_angle_rect_get(pname, &i)) *v = i;
    else glGetFloatv(pname, v);
}
static void gles_angle_GetBooleanv(GLenum pname, GLboolean *v)
{
    GLint i;
    if (gles_angle_rect_get(pname, &i)) *v = i != 0;
    else glGetBooleanv(pname, v);
}
/* CALL with the rectangle target at 2D, or as it is. */
#define GLES_ANGLE_ON_RECT(target, call) do {                       \
        if ((target) == GL_TEXTURE_RECTANGLE_ARB && angle_current) {  \
            GLint old_ = gles_angle_rect_begin();                     \
            GLenum t_ = GL_TEXTURE_2D;                                \
            call;                                                     \
            glBindTexture(GL_TEXTURE_2D, old_);                       \
        } else {                                                      \
            GLenum t_ = (target);                                     \
            call;                                                     \
        }                                                             \
    } while (0)
static void gles_angle_rect_sized(GLenum target, GLint level, GLsizei w, GLsizei h)
{
    if (target == GL_TEXTURE_RECTANGLE_ARB && !level && angle_current) {
        GLuint name = angle_current->rect[gles_angle_unit()];
        g_hash_table_insert(angle_current->rect_size, GUINT_TO_POINTER(name),
                            GUINT_TO_POINTER((unsigned)w << 16 | (unsigned)h));
    }
}
static void gles_angle_TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                                  GLint border, GLenum fmt, GLenum type, const void *pixels)
{
    gles_angle_rect_sized(target, level, w, h);
    GLES_ANGLE_ON_RECT(target, gles_angle_tex_image_2d(t_, level, ifmt, w, h, border, fmt, type, pixels));
}
static void gles_angle_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                     GLenum fmt, GLenum type, const void *pixels)
{
    g_autofree uint8_t *tight = NULL;
    const void *px = gles_angle_unpack(w, h, fmt, type, pixels, &tight);
    GLES_ANGLE_ON_RECT(target, glTexSubImage2D(t_, level, x, y, w, h, fmt, type, px));
}
static void gles_angle_CopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y,
                                      GLsizei w, GLsizei h, GLint border)
{
    gles_angle_rect_sized(target, level, w, h);
    GLES_ANGLE_ON_RECT(target, glCopyTexImage2D(t_, level, ifmt, x, y, w, h, border));
}
static void gles_angle_CopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y,
                                         GLsizei w, GLsizei h)
{
    GLES_ANGLE_ON_RECT(target, glCopyTexSubImage2D(t_, level, xo, yo, x, y, w, h));
}
/* GL_TEXTURE_MAX_LEVEL, which gles-host.c caps at the highest level uploaded so that desktop GL, which samples an
 * incomplete texture as black, treats it as complete. ES 1.1 has none (ANGLE: GL_INVALID_ENUM, blamed on the upload
 * or glGenerateMipmap that set it), and needs none: an ES1 context disables an incomplete texture's unit, as the
 * devices did. There it is dropped and reads as GL's default, 1000 (a guest's own APPLE_texture_max_level in an ES1
 * context with it: kept per texture would be the next step, should an app need it). */
static bool gles_angle_es1_max_level(GLenum p) { return p == GL_TEXTURE_MAX_LEVEL && angle_current && !angle_current->es2; }
static void gles_angle_TexParameteri(GLenum target, GLenum p, GLint v)
{
    if (!gles_angle_es1_max_level(p)) GLES_ANGLE_ON_RECT(target, glTexParameteri(t_, p, v));
}
static void gles_angle_TexParameterf(GLenum target, GLenum p, GLfloat v)
{
    if (!gles_angle_es1_max_level(p)) GLES_ANGLE_ON_RECT(target, glTexParameterf(t_, p, v));
}
static void gles_angle_TexParameteriv(GLenum target, GLenum p, const GLint *v)
{
    if (!gles_angle_es1_max_level(p)) GLES_ANGLE_ON_RECT(target, glTexParameteriv(t_, p, v));
}
static void gles_angle_TexParameterfv(GLenum target, GLenum p, const GLfloat *v)
{
    if (!gles_angle_es1_max_level(p)) GLES_ANGLE_ON_RECT(target, glTexParameterfv(t_, p, v));
}
static void gles_angle_GetTexParameteriv(GLenum target, GLenum p, GLint *v)
{
    if (gles_angle_es1_max_level(p)) *v = 1000;
    else GLES_ANGLE_ON_RECT(target, glGetTexParameteriv(t_, p, v));
}
static void gles_angle_GetTexParameterfv(GLenum target, GLenum p, GLfloat *v)
{
    if (gles_angle_es1_max_level(p)) *v = 1000;
    else GLES_ANGLE_ON_RECT(target, glGetTexParameterfv(t_, p, v));
}

/* Each unit's rectangle, where on, at 2D for a draw (see above); the units swapped, as a mask. */
static unsigned gles_angle_rect_draw_begin(GLint old[], GLboolean was_on[], GLfloat matrix[][16])
{
    unsigned swapped = 0;
    GLint active = GL_TEXTURE0, mode = GL_MODELVIEW;
    if (!angle_current || angle_current->es2) return 0;
    for (unsigned u = 0; u < G_N_ELEMENTS(angle_current->rect); u++) {
        GLuint name = angle_current->rect[u];
        if (!angle_current->rect_on[u] || !name) continue;
        unsigned size = GPOINTER_TO_UINT(g_hash_table_lookup(angle_current->rect_size, GUINT_TO_POINTER(name)));
        GLfloat w = MAX(size >> 16, 1), h = MAX(size & 0xffff, 1), m[16];
        if (!swapped) {
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            glGetIntegerv(GL_MATRIX_MODE, &mode);
            glMatrixMode(GL_TEXTURE);
        }
        swapped |= 1u << u;
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old[u]);
        was_on[u] = glIsEnabled(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, name);
        glEnable(GL_TEXTURE_2D);
        glGetFloatv(GL_TEXTURE_MATRIX, matrix[u]);
        for (unsigned c = 0; c < 4; c++) {          /* diag(1/w, 1/h, 1, 1) times the guest's matrix */
            m[c * 4 + 0] = matrix[u][c * 4 + 0] / w;
            m[c * 4 + 1] = matrix[u][c * 4 + 1] / h;
            m[c * 4 + 2] = matrix[u][c * 4 + 2];
            m[c * 4 + 3] = matrix[u][c * 4 + 3];
        }
        glLoadMatrixf(m);
    }
    if (swapped) {
        glActiveTexture(active);
        glMatrixMode(mode);
    }
    return swapped;
}
static void gles_angle_rect_draw_end(unsigned swapped, const GLint old[], const GLboolean was_on[],
                                     GLfloat matrix[][16])
{
    GLint active = GL_TEXTURE0, mode = GL_MODELVIEW;
    if (!swapped) return;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glMatrixMode(GL_TEXTURE);
    for (unsigned u = 0; u < G_N_ELEMENTS(angle_current->rect); u++) {
        if (!(swapped & (1u << u))) continue;
        glActiveTexture(GL_TEXTURE0 + u);
        glLoadMatrixf(matrix[u]);
        glBindTexture(GL_TEXTURE_2D, old[u]);
        if (!was_on[u]) glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(active);
    glMatrixMode(mode);
}
#define GLES_ANGLE_RECT_DRAW(call) do {                                         \
        GLint old_[8];                                                          \
        GLboolean on_[8];                                                       \
        GLfloat m_[8][16];                                                      \
        unsigned swapped_ = gles_angle_rect_draw_begin(old_, on_, m_);          \
        call;                                                                   \
        gles_angle_rect_draw_end(swapped_, old_, on_, m_);                      \
    } while (0)
static void gles_angle_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    GLES_ANGLE_RECT_DRAW(glDrawArrays(mode, first, count));
}
static void gles_angle_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    GLES_ANGLE_RECT_DRAW(glDrawElements(mode, count, type, indices));
}
#undef glBindTexture
#define glBindTexture gles_angle_BindTexture
#undef glDeleteTextures
#define glDeleteTextures gles_angle_DeleteTextures
#undef glEnable
#define glEnable gles_angle_Enable
#undef glDisable
#define glDisable gles_angle_Disable
#undef glIsEnabled
#define glIsEnabled gles_angle_IsEnabled
#undef glGetIntegerv
#define glGetIntegerv gles_angle_GetIntegerv
#undef glGetFloatv
#define glGetFloatv gles_angle_GetFloatv
#undef glGetBooleanv
#define glGetBooleanv gles_angle_GetBooleanv
#undef glTexImage2D
#define glTexImage2D gles_angle_TexImage2D
#undef glTexSubImage2D
#define glTexSubImage2D gles_angle_TexSubImage2D
#undef glCopyTexImage2D
#define glCopyTexImage2D gles_angle_CopyTexImage2D
#undef glCopyTexSubImage2D
#define glCopyTexSubImage2D gles_angle_CopyTexSubImage2D
#undef glTexParameteri
#define glTexParameteri gles_angle_TexParameteri
#undef glTexParameterf
#define glTexParameterf gles_angle_TexParameterf
#undef glTexParameteriv
#define glTexParameteriv gles_angle_TexParameteriv
#undef glTexParameterfv
#define glTexParameterfv gles_angle_TexParameterfv
#undef glGetTexParameteriv
#define glGetTexParameteriv gles_angle_GetTexParameteriv
#undef glGetTexParameterfv
#define glGetTexParameterfv gles_angle_GetTexParameterfv
#undef glDrawArrays
#define glDrawArrays gles_angle_DrawArrays
#undef glDrawElements
#define glDrawElements gles_angle_DrawElements
