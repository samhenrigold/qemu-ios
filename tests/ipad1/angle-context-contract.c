/* Native EGL contract probe. This is not guest-transport acceptance. */
#include <assert.h>
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static EGLContext context(EGLDisplay d, EGLConfig cfg, EGLContext share, int version)
{
    EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
    return eglCreateContext(d, cfg, share, attrs);
}

int main(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    assert(get);
    EGLint attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
    EGLDisplay d = get(EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
    assert(d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL));
    assert(eglBindAPI(EGL_OPENGL_ES_API));
    EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                   EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                   EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n;
    assert(eglChooseConfig(d, ca, &cfg, 1, &n) && n);
    EGLint sa[] = {EGL_WIDTH, 1024, EGL_HEIGHT, 768, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
    assert(s != EGL_NO_SURFACE);
    for (int version = 1; version <= 2; version++) {
        EGLContext root = context(d, cfg, EGL_NO_CONTEXT, version);
        EGLContext child = context(d, cfg, root, version);
        EGLContext isolated = context(d, cfg, EGL_NO_CONTEXT, version);
        assert(root != EGL_NO_CONTEXT && child != EGL_NO_CONTEXT && isolated != EGL_NO_CONTEXT);
        assert(eglMakeCurrent(d, s, s, root));
        GLuint texture;
        unsigned char original[] = {17, 31, 47, 255};
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, original);
        assert(glGetError() == GL_NO_ERROR);
        assert(eglMakeCurrent(d, s, s, isolated));
        assert(!glIsTexture(texture));
        assert(eglMakeCurrent(d, s, s, child));
        assert(glIsTexture(texture));
        /* A sharegroup must survive the root context's destruction. */
        assert(eglDestroyContext(d, root));
        assert(glIsTexture(texture));
        /* ES1 exposes FBO through OES; ES2 uses the core names. */
        void (*gen)(GLsizei, GLuint *) = (void *)eglGetProcAddress(version == 1 ? "glGenFramebuffersOES" : "glGenFramebuffers");
        void (*bind)(GLenum, GLuint) = (void *)eglGetProcAddress(version == 1 ? "glBindFramebufferOES" : "glBindFramebuffer");
        void (*attach)(GLenum, GLenum, GLenum, GLuint, GLint) = (void *)eglGetProcAddress(version == 1 ? "glFramebufferTexture2DOES" : "glFramebufferTexture2D");
        GLenum (*status)(GLenum) = (void *)eglGetProcAddress(version == 1 ? "glCheckFramebufferStatusOES" : "glCheckFramebufferStatus");
        void (*del)(GLsizei, const GLuint *) = (void *)eglGetProcAddress(version == 1 ? "glDeleteFramebuffersOES" : "glDeleteFramebuffers");
        assert(gen && bind && attach && status && del);
        GLuint fbo;
        gen(1, &fbo);
        bind(GL_FRAMEBUFFER, fbo);
        attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        assert(status(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        unsigned char pixel[4];
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        assert(glGetError() == GL_NO_ERROR);
        for (int i = 0; i < 4; i++) assert(pixel[i] == original[i]);
        del(1, &fbo);
        glDeleteTextures(1, &texture);
        assert(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        assert(eglDestroyContext(d, child) && eglDestroyContext(d, isolated));
        printf("PASS ES%d sharegroup isolation, root lifetime, FBO readback at iPad surface size\n", version);
    }
    EGLContext es1 = context(d, cfg, EGL_NO_CONTEXT, 1);
    assert(es1 != EGL_NO_CONTEXT);
    EGLContext mixed = context(d, cfg, es1, 2);
    printf("INFO ES1/ES2 mixed sharegroup: %s (EGL error 0x%x)\n",
           mixed == EGL_NO_CONTEXT ? "rejected" : "accepted", eglGetError());
    if (mixed != EGL_NO_CONTEXT) assert(eglDestroyContext(d, mixed));
    assert(eglDestroyContext(d, es1));
    assert(eglDestroySurface(d, s) && eglTerminate(d));
}
