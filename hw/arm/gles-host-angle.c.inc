/* Experimental EGL lifetime adapter for the existing guest executor.
 * Each guest context has its own native state; the group retains a root only
 * for the shared namespace. No window/swap or second transport is involved.
 * A guest ES 2.0 context is an ANGLE ES 2.0 (or later) context, the rest ES 1.1:
 * ANGLE runs no fixed function in a programmable context, nor shaders in an ES1 one. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define CGLContextObj GLESAngleContextRef
#define CGLSetCurrentContext gles_angle_set_current
#define CGLRetainContext gles_angle_retain
#define CGLReleaseContext gles_angle_release
typedef struct GLESAngleContext {
    EGLContext context;
    EGLSurface surface;
    unsigned refs;
    bool es2;
} *CGLContextObj;
static EGLDisplay angle_display = EGL_NO_DISPLAY;
static EGLConfig angle_config;
static __thread CGLContextObj angle_current;

static bool gles_angle_create(CGLContextObj share, bool es2, CGLContextObj *out)
{
    if (angle_display == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLint attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
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
    c->context = eglCreateContext(angle_display, angle_config, share ? share->context : EGL_NO_CONTEXT, ctx);
    c->surface = eglCreatePbufferSurface(angle_display, angle_config, surface);
    if (c->context == EGL_NO_CONTEXT || c->surface == EGL_NO_SURFACE) {
        if (c->context != EGL_NO_CONTEXT) eglDestroyContext(angle_display, c->context);
        if (c->surface != EGL_NO_SURFACE) eglDestroySurface(angle_display, c->surface);
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
static void gles_angle_tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                                    GLint border, GLenum fmt, GLenum type, const void *pixels)
{
    if (ifmt == GL_RGBA && (fmt == GL_BGRA_EXT || fmt == GL_RGB || fmt == GL_ALPHA ||
                            fmt == GL_LUMINANCE || fmt == GL_LUMINANCE_ALPHA)) ifmt = fmt;
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, pixels);
}
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
static void gles_angle_FramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e) { if (gles_angle_es2()) glFramebufferTexture2D(a, b, c, d, e); else glFramebufferTexture2DOES(a, b, c, d, e); }
static void gles_angle_GenRenderbuffers(GLsizei a, GLuint *b) { if (gles_angle_es2()) glGenRenderbuffers(a, b); else glGenRenderbuffersOES(a, b); }
static void gles_angle_BindRenderbuffer(GLenum a, GLuint b) { if (gles_angle_es2()) glBindRenderbuffer(a, b); else glBindRenderbufferOES(a, b); }
static void gles_angle_RenderbufferStorage(GLenum a, GLenum b, GLsizei c, GLsizei d) { if (gles_angle_es2()) glRenderbufferStorage(a, b, c, d); else glRenderbufferStorageOES(a, b, c, d); }
static void gles_angle_FramebufferRenderbuffer(GLenum a, GLenum b, GLenum c, GLuint d) { if (gles_angle_es2()) glFramebufferRenderbuffer(a, b, c, d); else glFramebufferRenderbufferOES(a, b, c, d); }
static GLenum gles_angle_CheckFramebufferStatus(GLenum a) { return gles_angle_es2() ? glCheckFramebufferStatus(a) : glCheckFramebufferStatusOES(a); }
static void gles_angle_DeleteFramebuffers(GLsizei a, const GLuint *b) { if (gles_angle_es2()) glDeleteFramebuffers(a, b); else glDeleteFramebuffersOES(a, b); }
static void gles_angle_DeleteRenderbuffers(GLsizei a, const GLuint *b) { if (gles_angle_es2()) glDeleteRenderbuffers(a, b); else glDeleteRenderbuffersOES(a, b); }
static void gles_angle_GetRenderbufferParameteriv(GLenum a, GLenum b, GLint *c) { if (gles_angle_es2()) glGetRenderbufferParameteriv(a, b, c); else glGetRenderbufferParameterivOES(a, b, c); }
static void gles_angle_GetFramebufferAttachmentParameteriv(GLenum a, GLenum b, GLenum c, GLint *d) { if (gles_angle_es2()) glGetFramebufferAttachmentParameteriv(a, b, c, d); else glGetFramebufferAttachmentParameterivOES(a, b, c, d); }
#define glIsFramebufferOES gles_angle_IsFramebuffer
#define glIsRenderbufferOES gles_angle_IsRenderbuffer
#define glGenerateMipmapOES gles_angle_GenerateMipmap
#define glGenFramebuffersOES gles_angle_GenFramebuffers
#define glBindFramebufferOES gles_angle_BindFramebuffer
#define glFramebufferTexture2DOES gles_angle_FramebufferTexture2D
#define glGenRenderbuffersOES gles_angle_GenRenderbuffers
#define glBindRenderbufferOES gles_angle_BindRenderbuffer
#define glRenderbufferStorageOES gles_angle_RenderbufferStorage
#define glFramebufferRenderbufferOES gles_angle_FramebufferRenderbuffer
#define glCheckFramebufferStatusOES gles_angle_CheckFramebufferStatus
#define glDeleteFramebuffersOES gles_angle_DeleteFramebuffers
#define glDeleteRenderbuffersOES gles_angle_DeleteRenderbuffers
#define glGetRenderbufferParameterivOES gles_angle_GetRenderbufferParameteriv
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
