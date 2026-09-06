#!/usr/bin/env python3
"""Actual dispatch against CGL: object lifetime, mip pixels and write masks."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
src = (root / 'hw/arm/gles-host.c').read_text()
shim = (root / 'contrib/it-gles/mbxshim.c').read_text()
slots = dict(re.findall(r'^(\d+) (\w+)$', (root / 'contrib/it-gles/slotmap.txt').read_text(), re.M))
cases = src[src.index('    case GLES_SLOT_CLEAR_STENCIL:'):src.index('    case GLES_SLOT_GEN_RENDERBUFFERS:')]
macros = dict(re.findall(r'#define (GLES_SLOT_\w+)\s+(\d+)', (root / 'include/hw/arm/guest-services/gles.h').read_text()))
for macro in re.findall(r'case (GLES_SLOT_\w+):', cases):
    slot = macros[macro]
    name = re.search(r'table\[' + slot + r'\]\s*= \(void \*\)(s_\w+)', shim)[1]
    assert slots[slot].lower().removesuffix('oes') == ('gl' + name[2:]).lower()
    assert re.search(name + r'\([^}]+qc\(' + slot + r',', shim)
code = r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
typedef struct CPUState CPUState;
#include "hw/arm/guest-services/gles.h"
''' + src[src.index('static float gles_x('):src.index('/* ------------------------------------------------------- buffer objects')] + r'''
static GLuint drawable;
static GLenum error;
static int gles_is_drawable(GLuint name) { return name == drawable; }
static int gles_reject(GLenum e) { error = e; return -1; }
static int dispatch(unsigned slot, const uint32_t *a) { switch(slot) {
''' + cases + r'''
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
 a[0] = GL_TEXTURE_3D; assert(dispatch(GLES_SLOT_GENERATE_MIPMAP, a) == -1 && error == GL_INVALID_ENUM);
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
}
'''
with tempfile.TemporaryDirectory() as d:
    path = Path(d) / 'check.c'; path.write_text(code)
    exe = Path(d) / 'check'
    subprocess.run(['clang', '-fsanitize=address,undefined', '-I' + str(root / 'include'),
                    str(path), '-framework', 'OpenGL', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: native object lifetime, drawable predicate, mipmap pixels, masks, fixed-point state/transforms and slot wiring')
