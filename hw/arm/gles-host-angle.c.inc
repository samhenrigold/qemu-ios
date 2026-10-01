/* Experimental ES1 EGL lifetime adapter for the existing guest executor.
 * Each guest context has its own native state; the group retains a root only
 * for the shared namespace. No window/swap or second transport is involved. */
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
} *CGLContextObj;
static EGLDisplay angle_display = EGL_NO_DISPLAY;
static EGLConfig angle_config;
static __thread CGLContextObj angle_current;

static bool gles_angle_create(CGLContextObj share, CGLContextObj *out)
{
    if (angle_display == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLint attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
        EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
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
    EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
    EGLint surface[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    CGLContextObj c = g_new0(struct GLESAngleContext, 1);
    c->refs = 1;
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
