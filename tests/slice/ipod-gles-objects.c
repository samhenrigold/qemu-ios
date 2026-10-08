/* Actual dispatch against CGL: object lifetime, drawable predicate, mip pixels, write masks and fixed-point
 * state and transforms.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c fn gles_x gles_f gles_reject
 * SLICE:mip hw/arm/gles-host.c range static int64_t gles_generate_mipmap( | static int64_t gles_pvrtc_upload(
 * SLICE:cases hw/arm/gles-host.c range case GLES_SLOT_LOAD_MATRIXX: | case GLES_SLOT_GEN_RENDERBUFFERS:
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include "slice.h"
static int32_t guest_matrix[16];
int gles_guest_rw(CPUState *cpu, vaddr addr, void *data, size_t n, bool write) {
    assert(!write && n == sizeof(guest_matrix));
    if (addr != 0x1000) return -1;
    memcpy(data, guest_matrix, n); return 0;
}
static GLuint drawable;
static bool gles_is_drawable(uint32_t name) { return name == drawable; }
static GLESPVRTC *gles_pvrtc_texture(bool create) {return NULL;}
static void gles_texture_begin(void) {GLenum e=glGetError();if(e)gles_reject(e);}
#include "mip.h"
static int dispatch(unsigned slot, const uint32_t *a) { CPUState *cpu = NULL; switch(slot) {
#include "cases.h"
default: assert(0); return -1; } }
int main(void) {
 CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, (CGLPixelFormatAttribute)0};
 CGLPixelFormatObj format; GLint count; CGLContextObj ctx;
 assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
 assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
 CGLDestroyPixelFormat(format); assert(CGLSetCurrentContext(ctx) == kCGLNoError);
 uint32_t a[6] = {0}; GLuint fb, rb, texture;
 assert(!dispatch(GLES_SLOT_IS_FRAMEBUFFER, a));
 assert(!dispatch(GLES_SLOT_IS_RENDERBUFFER, a));
 glGenFramebuffersEXT(1, &fb); a[0] = fb;
 assert(!dispatch(GLES_SLOT_IS_FRAMEBUFFER, a));
 glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
 assert(dispatch(GLES_SLOT_IS_FRAMEBUFFER, a));
 glDeleteFramebuffersEXT(1, &fb); assert(!dispatch(GLES_SLOT_IS_FRAMEBUFFER, a));
 drawable = 987; a[0] = drawable; assert(dispatch(GLES_SLOT_IS_FRAMEBUFFER, a)); drawable = 0;
 glGenRenderbuffersEXT(1, &rb); a[0] = rb; assert(!dispatch(GLES_SLOT_IS_RENDERBUFFER, a));
 glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb); assert(dispatch(GLES_SLOT_IS_RENDERBUFFER, a));
 glDeleteRenderbuffersEXT(1, &rb); assert(!dispatch(GLES_SLOT_IS_RENDERBUFFER, a));
 a[0] = 1; a[1] = 0; a[2] = 1; a[3] = 0; dispatch(GLES_SLOT_COLOR_MASK, a);
 GLboolean mask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, mask);
 assert(mask[0] && !mask[1] && mask[2] && !mask[3]);
 a[0] = 0x35; dispatch(GLES_SLOT_STENCIL_MASK, a);
 GLint stencil; glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil); assert(stencil == 0x35);
 glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
 uint8_t pixels[4*4*4]; for (unsigned i=0; i<sizeof(pixels); i++) pixels[i] = 99;
 glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
 a[0] = GL_TEXTURE_2D; assert(!dispatch(GLES_SLOT_GENERATE_MIPMAP, a));
 GLint width; glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &width); assert(width == 1);
 uint8_t pixel[4]; glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
 for (unsigned i=0; i<4; i++) assert(pixel[i] == 99);
 a[0] = GL_TEXTURE_3D; assert(dispatch(GLES_SLOT_GENERATE_MIPMAP, a) == -1 && gh.error == GL_INVALID_ENUM);
 a[0] = (uint32_t)-32768; a[1] = 16384; a[2] = 65536; a[3] = 32768;
 dispatch(GLES_SLOT_COLOR4X, a);
 GLfloat color[4]; glGetFloatv(GL_CURRENT_COLOR, color);
 assert(color[0] == -.5f && color[1] == .25f && color[2] == 1 && color[3] == .5f);
 glMatrixMode(GL_MODELVIEW); glLoadIdentity(); dispatch(GLES_SLOT_TRANSLATEX, a);
 GLfloat matrix[16]; glGetFloatv(GL_MODELVIEW_MATRIX, matrix);
 assert(matrix[12] == -.5f && matrix[13] == .25f && matrix[14] == 1);
 a[0] = 0; a[1] = 2*65536; a[2] = 0; a[3] = 4*65536; a[4] = -65536; a[5] = 65536;
 glLoadIdentity(); dispatch(GLES_SLOT_ORTHOX, a); glGetFloatv(GL_MODELVIEW_MATRIX, matrix);
 assert(matrix[0] == 1 && matrix[5] == .5f && matrix[10] == -1);
 for (unsigned j=0; j<16; j++) guest_matrix[j] = j % 5 == 0 ? 65536 : 0;
 guest_matrix[12] = -32768; guest_matrix[13] = 16384;
 a[0] = 0x1000; assert(!dispatch(GLES_SLOT_LOAD_MATRIXX, a));
 assert(!dispatch(GLES_SLOT_MULT_MATRIXX, a)); glGetFloatv(GL_MODELVIEW_MATRIX, matrix);
 assert(matrix[12] == -1 && matrix[13] == .5f);
 a[0] = 0x2000; assert(dispatch(GLES_SLOT_LOAD_MATRIXX, a) == -1);
 glGetFloatv(GL_MODELVIEW_MATRIX, matrix); assert(matrix[12] == -1);
 a[0] = GL_GREATER; a[1] = 32768; dispatch(GLES_SLOT_ALPHA_FUNCX, a);
 GLfloat alpha; glGetFloatv(GL_ALPHA_TEST_REF, &alpha); assert(fabsf(alpha-.5f) < .005f);
 a[0] = GL_ALWAYS; a[1] = 3; a[2] = 0x17; dispatch(GLES_SLOT_STENCIL_FUNC, a);
 GLint state; glGetIntegerv(GL_STENCIL_VALUE_MASK, &state); assert(state == 0x17);
 a[0] = GL_KEEP; a[1] = GL_INCR; a[2] = GL_REPLACE; dispatch(GLES_SLOT_STENCIL_OP, a);
 glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &state); assert(state == GL_INCR);
 a[0] = GL_FRONT; dispatch(GLES_SLOT_CULL_FACE, a); glGetIntegerv(GL_CULL_FACE_MODE, &state); assert(state == GL_FRONT);
 a[0] = 0; assert(!dispatch(GLES_SLOT_POINT_SIZEX, a)); assert(glGetError() == GL_INVALID_VALUE);
 assert(glGetError() == GL_NO_ERROR); glDeleteTextures(1, &texture);
 CGLSetCurrentContext(NULL); CGLDestroyContext(ctx);
 puts("PASS: native object lifetime, drawable predicate, mipmap pixels, masks and fixed-point state/transforms");
}
