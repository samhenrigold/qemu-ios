/* Cube-map faces upload through glTexImage2D without host errors and sample as uploaded.
 * 
 * SpinningiPhoneApp uploads six 512x512 faces, then glGenerateMipmap(GL_TEXTURE_CUBE_MAP)
 * with a mipmapping min filter. The host used to cap MAX_LEVEL on the face target
 * (GL_INVALID_ENUM) and reject the cube-map mipmap call, leaving the cube incomplete:
 * Metal logged "GLD_TEXTURE_INDEX_CUBE_MAP is unloadable" and sampled zero.
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
    memcpy(data, guest, n); return 0;
}
static int64_t dispatch(unsigned slot, const uint32_t *a) { CPUState *cpu = NULL; switch (slot) {
#include "cases.h"
default: assert(0); return -1; } }
int main(void) {
 CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, (CGLPixelFormatAttribute)0};
 CGLPixelFormatObj format; GLint count; CGLContextObj ctx;
 assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
 assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
 CGLDestroyPixelFormat(format); assert(CGLSetCurrentContext(ctx) == kCGLNoError);
 memset(guest, 200, sizeof(guest));
 GLuint cube; glGenTextures(1, &cube); glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
 glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
 glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
 /* the app's face order: NEGATIVE_X first */
 static const GLenum faces[] = {0x8516, 0x8518, 0x851a, 0x8515, 0x8517, 0x8519};
 for (unsigned i = 0; i < 6; i++) {
  uint32_t a[9] = {faces[i], 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0x1000};
  assert(dispatch(GLES_SLOT_TEX_IMAGE_2D, a) == 0);
  assert(glGetError() == GL_NO_ERROR && !gh.error);
 }
 assert(gles_generate_mipmap(GL_TEXTURE_CUBE_MAP) == 0 && !gh.error);
 assert(gles_texparam_nparams(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER) == 1);
 assert(gles_generate_mipmap(GL_TEXTURE_3D) == -1 && gh.error == GL_INVALID_ENUM); gh.error = 0;
 /* complete: sampling returns the uploaded texels, not the zero texture */
 GLuint fb, rb; glGenFramebuffersEXT(1, &fb); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
 glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
 glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 4, 4);
 glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
 assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
 glViewport(0, 0, 4, 4); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
 glEnable(GL_TEXTURE_CUBE_MAP); glColor4f(1, 1, 1, 1);
 glBegin(GL_QUADS);
 glTexCoord3f(1, 0, 0); glVertex2f(-1, -1); glTexCoord3f(1, 0, 0); glVertex2f(1, -1);
 glTexCoord3f(1, 0, 0); glVertex2f(1, 1); glTexCoord3f(1, 0, 0); glVertex2f(-1, 1);
 glEnd();
 uint8_t pixel[4]; glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
 assert(abs(pixel[0] - 200) <= 1 && abs(pixel[1] - 200) <= 1);
 assert(glGetError() == GL_NO_ERROR);
 CGLSetCurrentContext(NULL); CGLDestroyContext(ctx);
 puts("PASS: cube faces upload cleanly, cube-map mipmaps generate, the cube samples its texels");
}
