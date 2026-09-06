#!/usr/bin/env python3
"""Actual PVRTC upload/replacement handlers against CGL, with guarded guest bytes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
s = (root/'hw/arm/gles-host.c').read_text()
types = s[s.rfind('typedef struct {', 0, s.index('} GLESPVRTCLevel;')):
          s.index('} GLESPVRTC;')+len('} GLESPVRTC;')]
decoder = s[s.index('#define PVRTC_RGB_4BPP'):s.index('/*\n * The paletted formats:')]
helpers = s[s.index('static GLESPVRTC *gles_pvrtc_texture('):s.index('/* IOSurface-backed textures')]
cases = s[s.index('    case GLES_SLOT_COMPRESSED_TEX_SUB_IMAGE_2D:'):
          s.index('    case GLES_SLOT_ALPHA_FUNC:')]
code = r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GLES_MAX_NAMES 65536u
#define GLES_SLOT_COMPRESSED_TEX_SUB_IMAGE_2D 1
#define GLES_SLOT_DELETE_TEXTURES 2
typedef int CPUState;
static uint32_t ldl_le_p(const uint8_t*p) {return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
''' + types + r'''
typedef struct {GHashTable *pvrtc,*surfaces; GLESPVRTC default_pvrtc; GLenum error;} State;
static State first, second, *current=&first;
#define gh (*current)
static uint8_t *guest, *decoded;
static size_t guest_size;
static unsigned fetches;
static GLuint deleted;
static int64_t gles_reject(GLenum e) {if (!gh.error) gh.error=e; return -1;}
static const uint8_t *gles_fetch_texels(CPUState*c,uint32_t addr,size_t size,const char*label) {
    fetches++; assert(size==guest_size); return addr==1 ? guest : NULL;
}
static uint8_t *gles_decode_buf(size_t n) {free(decoded); return decoded=malloc(n);}
static void gles_report_decode(uint32_t f,uint32_t w,uint32_t h,const uint8_t*d) {}
static int cpu_memory_rw_debug(CPUState*c,uint32_t addr,uint8_t*d,size_t n,int write) {
    assert(addr==2 && !write && n==sizeof(deleted)); memcpy(d,&deleted,n);return 0;
}
''' + decoder + helpers + r'''
static int64_t dispatch(unsigned slot,const uint32_t*a) {CPUState*cpu=NULL;switch(slot) {
''' + cases + r'''
default: abort();}}
static void words(size_t count) {
    free(guest);guest_size=count*8;guest=malloc(guest_size);
    for(size_t i=0;i<count;i++) {uint32_t v[2]={0,0xfffffffe};memcpy(guest+i*8,v,8);}
}
static int upload(unsigned w,unsigned h,unsigned size,unsigned level,unsigned border,unsigned data) {
    return gles_pvrtc_upload(NULL,GL_TEXTURE_2D,level,PVRTC_RGBA_4BPP,w,h,border,size,data,false);
}
static void expect_error(GLenum e) {assert(gh.error==e);gh.error=0;assert(glGetError()==GL_NO_ERROR);}
int main(void) {
    CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,0}; CGLPixelFormatObj format;
    GLint count; CGLContextObj ctx,other;
    assert(!CGLChoosePixelFormat(attrs,&format,&count));
    assert(!CGLCreateContext(format,NULL,&ctx));assert(!CGLCreateContext(format,ctx,&other));
    CGLDestroyPixelFormat(format);assert(!CGLSetCurrentContext(ctx));
    GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    words(4);
    assert(!gles_pvrtc_upload(NULL,GL_TEXTURE_2D,0,PVRTC_RGB_4BPP,8,8,0,32,1,false));
    GLint internal;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_ALPHA_SIZE,&internal);
    assert(internal==0); /* RGB texture-env semantics must preserve fragment alpha. */
    fetches=0;assert(!upload(8,8,32,0,0,1));assert(fetches==1);
    assert(gles_pvrtc_texture(false)->levels[0].format==PVRTC_RGBA_4BPP);
    assert(upload(8,8,31,0,0,1)==-1);expect_error(GL_INVALID_VALUE);
    assert(upload(8,8,33,0,0,1)==-1);expect_error(GL_INVALID_VALUE);
    assert(upload(8,8,32,0,1,1)==-1);expect_error(GL_INVALID_VALUE);
    assert(upload(3,8,32,0,0,1)==-1);expect_error(GL_INVALID_VALUE);
    assert(upload(8,8,32,13,0,1)==-1);expect_error(GL_INVALID_VALUE);
    assert(upload(8192,8,32,0,0,1)==-1);expect_error(GL_INVALID_VALUE);
    assert(upload(8,8,32,UINT32_MAX,0,1)==-1);expect_error(GL_INVALID_VALUE);
    assert(fetches==1); /* Reject before touching a guest pointer. */
    assert(gles_pvrtc_upload(NULL,GL_TEXTURE_3D,0,PVRTC_RGBA_4BPP,8,8,0,32,1,false)==-1);
    expect_error(GL_INVALID_ENUM);
    uint32_t a[]={GL_TEXTURE_2D,0,0,0,8,8,PVRTC_RGBA_4BPP,32,1};
    assert(!dispatch(1,a));a[2]=1;assert(dispatch(1,a)==-1);expect_error(GL_INVALID_OPERATION);a[2]=0;
    a[4]=4;assert(dispatch(1,a)==-1);expect_error(GL_INVALID_OPERATION);a[4]=8;
    a[6]=PVRTC_RGB_4BPP;assert(dispatch(1,a)==-1);expect_error(GL_INVALID_OPERATION);a[6]=PVRTC_RGBA_4BPP;
    a[6]=GL_RGBA;assert(dispatch(1,a)==-1);expect_error(GL_INVALID_ENUM);a[6]=PVRTC_RGBA_4BPP;
    /* Standard padding has twiddled stride two even for a single row. */
    uint32_t endpoints[]={0x8000u|(31u<<10),0x8000u|(31u<<5),0x8000u|0x1e,0xfffe};
    for(unsigned i=0;i<4;i++)memcpy(guest+8*i+4,&endpoints[i],4);
    assert(!upload(8,4,32,0,0,1));uint8_t pixels[8*8*4];
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    unsigned at=(2*8+6)*4;assert(pixels[at]==0 && pixels[at+1]==0 && pixels[at+2]==255);
    words(1);assert(!upload(1,1,8,0,0,1)); /* Compact driver mip. */
    words(4);assert(!upload(1,1,32,0,0,1)); /* Standard padded mip. */
    assert(!upload(8,8,32,0,0,1));assert(!gles_generate_mipmap(GL_TEXTURE_2D));
    assert(gles_pvrtc_texture(false)->levels[2].width==2);
    words(1);a[1]=2;a[4]=a[5]=2;a[7]=8;assert(!dispatch(1,a));
    /* Named metadata shares with native textures, default texture does not. */
    second.pvrtc=first.pvrtc;current=&second;assert(!CGLSetCurrentContext(other));
    glBindTexture(GL_TEXTURE_2D,texture);assert(!dispatch(1,a));
    glBindTexture(GL_TEXTURE_2D,0);assert(dispatch(1,a)==-1);expect_error(GL_INVALID_OPERATION);
    glBindTexture(GL_TEXTURE_2D,texture);
    /* A successful uncompressed redefinition invalidates compressed identity. */
    gles_texture_begin();glTexImage2D(GL_TEXTURE_2D,2,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    assert(!gles_texture_end(GL_TEXTURE_2D,2,(GLESPVRTCLevel){0}));
    assert(dispatch(1,a)==-1);expect_error(GL_INVALID_OPERATION);
    /* Invalid GL mutations preserve both metadata and the guest's older error. */
    words(4);assert(!upload(8,8,32,0,0,1));
    glEnable(0xdead);gles_texture_begin();glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,-1,8,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    assert(gles_texture_end(GL_TEXTURE_2D,0,(GLESPVRTCLevel){0})==-1);expect_error(GL_INVALID_ENUM);
    assert(gles_pvrtc_texture(false)->levels[0].width==8);
    deleted=texture;uint32_t del[]={1,2};assert(!dispatch(2,del));
    assert(!g_hash_table_lookup(first.pvrtc,GUINT_TO_POINTER(texture)));
    assert(!glIsTexture(texture));g_hash_table_destroy(first.pvrtc);
    free(guest);free(decoded);CGLSetCurrentContext(NULL);CGLReleaseContext(other);CGLReleaseContext(ctx);
    puts("PASS: PVRTC exact sizes/errors, compact/padded mips, full replacement, shared lifetime and native CGL pixels");
}
'''
with tempfile.TemporaryDirectory(prefix='pvrtc-upload-') as directory:
    c=Path(directory)/'check.c';exe=Path(directory)/'check';c.write_text(code)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    str(c),'-o',str(exe),*flags,'-framework','OpenGL'],check=True)
    subprocess.run([str(exe)],check=True)
