/* A texture's generated mip levels are sampled: glGenerateMipmapOES after a level-0 upload, and GL_GENERATE_MIPMAP
 * set before it, both leave a chain a minifying draw reads its small levels from.
 *
 * The level-0 upload caps the host's MAX_LEVEL at 0 (so a texture the guest gave one level stays complete, as ES
 * treats it), and that cap also kept glGenerateMipmapOES from generating or sampling any level past 0: an 8x8 texture
 * of red and blue columns drawn into one pixel with a mipmapping filter read level 0's first texel (blue) where the
 * device reads the 1x1 level (purple). GL_GENERATE_MIPMAP was not affected; its cases guard it.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c range /* The ES half-float type | /* ---------------------------------------------------------------- refusals
 * SLICE hw/arm/gles-host.c range static const uint8_t *gles_zeroed( | /*\n * Report a draw the host rejected
 * SLICE hw/arm/gles-host.c range static GLESPVRTC *gles_pvrtc_texture( | static int64_t gles_pvrtc_upload(
 * SLICE hw/arm/gles-host.c fn gles_texture_object gles_texparam_nparams
 * SLICE:cases hw/arm/gles-host.c range case GLES_SLOT_TEX_IMAGE_2D: { | case GLES_SLOT_TEX_SUB_IMAGE_2D: {
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include "slice.h"
static void gles_surface_forget(GLenum target) { (void)target; }   /* no IOSurfaces here */
static uint8_t guest[8 * 8 * 4];
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write) {
    assert(!write && addr == 0x1000 && n <= sizeof(guest));
    memcpy(data, guest, n);
    return 0;
}
static int64_t dispatch(unsigned slot, const uint32_t *a) {
    CPUState *cpu = NULL;
    switch (slot) {
#include "cases.h"
    default: assert(0); return -1;
    }
}

/* An 8x8 texture drawn into one pixel with NEAREST_MIPMAP_NEAREST: the color of the level it sampled. */
static void minified(uint8_t pixel[4]) {
    glViewport(0, 0, 4, 4);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0); glTexCoord2f(1, 0); glVertex2f(1, 0);
    glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(0, 1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
}

static GLuint texture(bool generate) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (generate) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    uint32_t a[9] = {GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0x1000};
    assert(dispatch(GLES_SLOT_TEX_IMAGE_2D, a) == 0);
    assert(glGetError() == GL_NO_ERROR && !gh.error);
    return t;
}

static void purple(const char *what, const uint8_t *p) {
    printf("%s: %d,%d,%d\n", what, p[0], p[1], p[2]);
    assert(abs(p[0] - 128) <= 8 && p[1] == 0 && abs(p[2] - 128) <= 8);
}

int main(void) {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj format;
    GLint count;
    CGLContextObj ctx;
    assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
    assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(format);
    assert(CGLSetCurrentContext(ctx) == kCGLNoError);
    for (unsigned i = 0; i < 64; i++) {   /* red and blue columns */
        guest[i * 4] = (i & 1) ? 255 : 0;
        guest[i * 4 + 1] = 0;
        guest[i * 4 + 2] = (i & 1) ? 0 : 255;
        guest[i * 4 + 3] = 255;
    }
    GLuint fb, rb;
    glGenFramebuffersEXT(1, &fb); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 4, 4);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    uint8_t p[4];

    /* one level only: complete (ES samples it), so the draw reads level 0 */
    texture(false);
    minified(p);
    printf("level 0 only: %d,%d,%d\n", p[0], p[1], p[2]);
    assert(p[0] == 0 && p[2] == 255);

    /* glGenerateMipmapOES after the upload */
    texture(false);
    assert(gles_generate_mipmap(GL_TEXTURE_2D) == 0 && !gh.error);
    minified(p);
    purple("glGenerateMipmapOES", p);

    /* GL_GENERATE_MIPMAP before the upload */
    texture(true);
    minified(p);
    purple("GL_GENERATE_MIPMAP", p);

    /* and a second upload into that texture regenerates the chain */
    for (unsigned i = 0; i < 64; i++) guest[i * 4 + 1] = 255;   /* now yellow and cyan */
    uint32_t a[9] = {GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0x1000};
    assert(dispatch(GLES_SLOT_TEX_IMAGE_2D, a) == 0);
    minified(p);
    printf("GL_GENERATE_MIPMAP, re-upload: %d,%d,%d\n", p[0], p[1], p[2]);
    assert(abs(p[0] - 128) <= 8 && p[1] == 255 && abs(p[2] - 128) <= 8);

    assert(glGetError() == GL_NO_ERROR);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    puts("PASS: generated mip levels are sampled (glGenerateMipmapOES and GL_GENERATE_MIPMAP)");
}
