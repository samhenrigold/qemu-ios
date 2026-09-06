#!/usr/bin/env python3
"""Production frame-end clearing against native CGL with guest write masks."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
src = (root / 'hw/arm/gles-host.c').read_text()
helper = src[src.index('static void gles_frame_end(void)'):src.index('static void gles_check_draw(')]
code = r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
static struct { bool depth_cleared_this_frame; } gh;
''' + helper + r'''
int main(void) {
 CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, 0};
 CGLPixelFormatObj format; GLint count; CGLContextObj ctx;
 assert(CGLChoosePixelFormat(attrs, &format, &count) == kCGLNoError);
 assert(CGLCreateContext(format, NULL, &ctx) == kCGLNoError);
 CGLDestroyPixelFormat(format); assert(CGLSetCurrentContext(ctx) == kCGLNoError);
 GLuint fb, color, ds;
 glGenFramebuffersEXT(1, &fb); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
 glGenRenderbuffersEXT(1, &color); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, color);
 glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 4, 4);
 glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, color);
 glGenRenderbuffersEXT(1, &ds); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, ds);
 glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, 4, 4);
 glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, ds);
 glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, ds);
 assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
 GLuint masks[] = {0, 0x35, ~0u};
 for (unsigned m=0; m<3; m++) {
  glDisable(GL_SCISSOR_TEST); glDepthMask(GL_TRUE); glStencilMask(~0u);
  glClearDepth(.25); glClearStencil(0xa7);
  glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  glClearDepth(1); glClearStencil(0);
  glDepthMask(GL_FALSE); glStencilMask(masks[m]);
  glEnable(GL_SCISSOR_TEST); glScissor(1, 1, 1, 1);
  gles_frame_end();
  GLint mask, box[4]; GLboolean depth;
  glGetIntegerv(GL_STENCIL_WRITEMASK, &mask); assert((GLuint)mask == masks[m]);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depth); assert(!depth);
  assert(glIsEnabled(GL_SCISSOR_TEST)); glGetIntegerv(GL_SCISSOR_BOX, box);
  assert(box[0]==1 && box[1]==1 && box[2]==1 && box[3]==1);
  unsigned char stencil[16]; GLfloat depths[16];
  glReadPixels(0,0,4,4,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,stencil);
  glReadPixels(0,0,4,4,GL_DEPTH_COMPONENT,GL_FLOAT,depths);
  for (unsigned i=0;i<16;i++) { assert(stencil[i]==0); assert(depths[i]==1); }
  assert(glGetError()==GL_NO_ERROR);
 }
 glDeleteRenderbuffersEXT(1,&ds); glDeleteRenderbuffersEXT(1,&color);
 glDeleteFramebuffersEXT(1,&fb); CGLSetCurrentContext(NULL); CGLDestroyContext(ctx);
}
'''
with tempfile.TemporaryDirectory() as d:
    path = Path(d) / 'check.c'; path.write_text(code)
    exe = Path(d) / 'check'
    subprocess.run(['clang', '-fsanitize=address,undefined', str(path), '-framework', 'OpenGL', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: native depth/stencil frame clear ignores guest masks/scissor and restores guest state')
