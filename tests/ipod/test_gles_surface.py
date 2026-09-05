#!/usr/bin/env python3
"""Native surface import/writeback check; macOS media and OpenGL access required."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/gles-host.c').read_text()
helpers = source[source.index('static bool gles_surface_range('):source.index('static int64_t gles_host_call_1(')]
preamble = r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <VideoToolbox/VideoToolbox.h>
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#define GLES_MAX_TEXUNITS 8
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GLES_SURFACE_BGRA32 0x42475241
#define GLES_SURFACE_RGBA32 0x52474241
#define GLES_SURFACE_RGB565 0x4c353635
struct CPUState { int unused; };
typedef struct CPUState CPUState;
typedef struct { uint32_t base, stride, width, height, format, uv, uvstride; } GLESSurface;
static struct { GHashTable *surfaces; uint32_t bound_framebuffer; } gh;
static uint8_t ram[0x100000];
static int cpu_memory_rw_debug(CPUState *cpu, uint64_t a, uint8_t *p, size_t n, int write)
{
    if (a < 0x10000000 || a + n > 0x10000000 + sizeof(ram)) return -1;
    if (write) memcpy(ram + (a - 0x10000000), p, n);
    else memcpy(p, ram + (a - 0x10000000), n);
    return 0;
}
'''
shim = (root / 'contrib/it-gles/mbxshim.c').read_text()
shim = shim[shim.index('static int surface_fault_read('):shim.index('/*\n * GLESBindView is')]
helper_end = shim.index('static int GLESBindCoreSurface(')
shim = shim[:helper_end] + shim[helper_end:].replace('surface_fault_read(', 'abi_surface_fault_read(')
finish_start = (root / 'contrib/it-gles/mbxshim.c').read_text().index('static int GLESFinishTexture(')
finish_source = (root / 'contrib/it-gles/mbxshim.c').read_text()[finish_start:]
finish_source = finish_source[:finish_source.index('\n}') + 2]
swap_start = (root / 'contrib/it-gles/mbxshim.c').read_text().index('static int GLESSwapNotification(')
swap_source = (root / 'contrib/it-gles/mbxshim.c').read_text()[swap_start:]
swap_source = swap_source[:swap_source.index('\n}') + 2]
abi = r'''
#define GLES_OP_BIND_SURFACE 0x1003
#define CA_FOURCC_565L GLES_SURFACE_RGB565
#define CA_FOURCC_BGRA GLES_SURFACE_BGRA32
static int abi_surface_fault_read(unsigned long base,unsigned stride,unsigned rows,unsigned bytes)
{ assert((base==0x10000000 || base==0x10000100) && stride==2 && bytes==2 && rows<=2);return 1; }
#define A(...) (const unsigned[]){__VA_ARGS__}
static unsigned abi_args[8], abi_locks, abi_finished, abi_signals;
static int abi_signal_error;
static void iosurface_init(void) {}
static void surface_arg(void *s) { assert(s == (void *)0x1234); }
static int p_IOSurfaceLock(void *s, unsigned mode, unsigned *seed)
{ surface_arg(s); assert(mode==1); abi_locks++; return 0; }
static int p_IOSurfaceUnlock(void *s, unsigned mode, unsigned *seed)
{ surface_arg(s); assert(mode==1); abi_locks--; return 0; }
static void *p_IOSurfaceGetBaseAddress(void *s) { surface_arg(s); return (void *)0x10000000; }
static unsigned p_IOSurfaceGetBytesPerRow(void *s) { surface_arg(s); return 2; }
static unsigned p_IOSurfaceGetWidth(void *s) { surface_arg(s); return 2; }
static unsigned p_IOSurfaceGetHeight(void *s) { surface_arg(s); return 2; }
static unsigned p_IOSurfaceGetPixelFormat(void *s) { surface_arg(s); return 0x34323076; }
static unsigned p_IOSurfaceGetPlaneCount(void *s) { surface_arg(s); return 2; }
static void *p_IOSurfaceGetBaseAddressOfPlane(void *s, unsigned plane)
{ surface_arg(s); return (void *)(uintptr_t)(0x10000000 + plane * 256); }
static unsigned p_IOSurfaceGetBytesPerRowOfPlane(void *s, unsigned plane)
{ surface_arg(s); return 2; }
static long long qc(unsigned op, void *gc, unsigned argc, const unsigned *a)
{ if(op==89) { assert(!argc); abi_finished++; return 0; } assert(op==GLES_OP_BIND_SURFACE && argc==8); memcpy(abi_args,a,sizeof(abi_args)); return 0; }

#define RTLD_NOW 2
static int swap_signal(unsigned port,unsigned selector,const unsigned long long *args,
                       unsigned count,unsigned long long *out,unsigned *n)
{
    assert(abi_finished && port==0x123 && selector==20 && count==2);
    assert(args[0]==42 && args[1]==3 && !out && !n);abi_signals++;
    return abi_signal_error;
}
static void *dlopen(const char *path,int mode) { assert(strstr(path,"IOKit"));return (void *)1; }
static void *dlsym(void *lib,const char *name)
{ assert(lib==(void *)1 && !strcmp(name,"IOConnectCallScalarMethod"));return swap_signal; }
'''
check = r'''
int main(void)
{
    void *fault_pages=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(fault_pages!=MAP_FAILED);
    assert(surface_fault_read((unsigned long)fault_pages+4095,8192,1,8192));
    assert(!surface_fault_read(~0UL-15,32,1,32));
    assert(!surface_fault_read(0x10000000,1,1,2));
    assert(GLESBindCoreSurface(NULL, 0x84f5, (void *)0x1234));
    assert(abi_args[0]==0x84f5 && abi_args[1]==0x10000000 && abi_args[6]==0x10000100);
    assert(!abi_locks);
    assert(GLESBindCoreSurface(NULL, 0x84f5, NULL));
    assert(!abi_args[1] && !abi_locks);
    assert(GLESFinishTexture(NULL, 0x0de1));
    assert(GLESFinishTexture(NULL, 0x84f5));
    assert(!GLESFinishTexture(NULL, 0xdead));
    abi_finished=0;
    assert(GLESSwapNotification(NULL,0x123,42,3));assert(abi_finished==1 && abi_signals==1);
    abi_signal_error=-1;assert(!GLESSwapNotification(NULL,0x123,42,3));
    assert(abi_finished==2 && abi_signals==2);
    CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pixel; GLint count; CGLContextObj context;
    assert(!CGLChoosePixelFormat(attrs, &pixel, &count));
    assert(!CGLCreateContext(pixel, NULL, &context)); CGLDestroyPixelFormat(pixel);
    assert(!CGLSetCurrentContext(context));
    GLuint tex, fb; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    uint32_t a[] = { GL_TEXTURE_RECTANGLE_ARB, 0x10000000, 20, 4, 2, GLES_SURFACE_BGRA32, 0, 0 };
    memset(ram, 0xa5, sizeof(ram));
    for (int y=0;y<2;y++) for(int x=0;x<4;x++) {
        uint8_t *p=ram+y*20+x*4; p[0]=x*30;p[1]=y*80;p[2]=100;p[3]=255;
    }
    assert(!gles_bind_surface(NULL,a));
    uint8_t got[32]; glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int y=0;y<2;y++) assert(!memcmp(got+y*16,ram+y*20,16));
    glGenFramebuffersEXT(1,&fb);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fb);
    gh.bound_framebuffer=fb;
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,tex,0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
    glClearColor(0,1,1,1);glClear(GL_COLOR_BUFFER_BIT);assert(!gles_sync_surface(NULL));
    for(int y=0;y<2;y++) {
        for(int x=0;x<4;x++) assert(!memcmp(ram+y*20+x*4,"\xff\xff\0\xff",4));
        for(int x=16;x<20;x++) assert(ram[y*20+x]==0xa5);
    }
    /* Current render target must survive a refresh; sampled aliases must see DMA. */
    glEnable(GL_TEXTURE_RECTANGLE_ARB);
    memset(ram,0,40);assert(gles_refresh_surfaces(NULL));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    assert(!memcmp(got,"\xff\xff\0\xff",4));
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,0,0);
    assert(gles_refresh_surfaces(NULL));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<32;i++) assert(!got[i]);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,tex,0);
    /* Imported rectangle pixels must survive actual fixed-function sampling. */
    GLuint output; glGenTextures(1,&output);glBindTexture(GL_TEXTURE_2D,output);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,4,2,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,output,0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
    for(int y=0;y<2;y++) for(int x=0;x<4;x++) {
        uint8_t *p=ram+y*20+x*4;p[0]=30;p[1]=80;p[2]=100;p[3]=255;
    }
    assert(!gles_bind_surface(NULL,a));
    glViewport(0,0,4,2);glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,4,0,2,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();glColor4f(1,1,1,1);
    glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_RGB,GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_ALPHA,GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE0_ALPHA,GL_CONSTANT);
    glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE1_ALPHA,GL_PREVIOUS);
    GLfloat white[]={1,1,1,1};glTexEnvfv(GL_TEXTURE_ENV,GL_TEXTURE_ENV_COLOR,white);
    glBegin(GL_QUADS);
    glTexCoord2f(0,0);glVertex2f(0,0);glTexCoord2f(4,0);glVertex2f(4,0);
    glTexCoord2f(4,2);glVertex2f(4,2);glTexCoord2f(0,2);glVertex2f(0,2);
    glEnd();glReadPixels(0,0,4,2,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<8;i++) assert(!memcmp(got+i*4,"\x1e\x50\x64\xff",4));
    assert(glGetError()==GL_NO_ERROR);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,tex,0);
    /* Detached storage must not overwrite a subsequently reused guest allocation. */
    a[1]=0;assert(!gles_bind_surface(NULL,a));memset(ram,0x5a,40);
    glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);assert(!gles_sync_surface(NULL));
    for(int i=0;i<40;i++) assert(ram[i]==0x5a);
    a[1]=0xfffffff0;assert(gles_bind_surface(NULL,a)==-1);
    a[1]=0x10000000;a[2]=15;assert(gles_bind_surface(NULL,a)==-1);
    a[2]=20;a[3]=2049;assert(gles_bind_surface(NULL,a)==-1);
    a[3]=2;a[4]=2;a[2]=2;a[5]=0x34323076;a[6]=0x10000100;a[7]=2;
    memset(ram,16,4);ram[256]=ram[257]=128;assert(!gles_bind_surface(NULL,a));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<4;i++) assert(!memcmp(got+i*4,"\0\0\0\xff",4));
    memset(ram,235,4);assert(!gles_bind_surface(NULL,a));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<16;i++) assert(got[i]==255);
    a[5]=0x34323066;memset(ram,0,4);assert(!gles_bind_surface(NULL,a));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<4;i++) assert(!memcmp(got+i*4,"\0\0\0\xff",4));
    a[3]=3;assert(gles_bind_surface(NULL,a)==-1);
    munmap(fault_pages,16384);
    g_hash_table_destroy(gh.surfaces);
    CGLSetCurrentContext(NULL);CGLDestroyContext(context);
    puts("PASS: IOSurface page faults and ABI, native textured draw, NV12 ranges, FBO writeback and bounds");
}
'''
with tempfile.TemporaryDirectory(prefix='it-gles-surface-') as tmp:
    c = Path(tmp) / 'check.c'; exe = Path(tmp) / 'check'
    c.write_text(preamble + helpers + abi + shim + finish_source + swap_source + check)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
    subprocess.run(['clang', '-g', '-Wno-pointer-to-int-cast', '-Wno-pointer-bool-conversion', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(c), '-o', str(exe),
        *flags, '-framework', 'OpenGL', '-framework', 'VideoToolbox', '-framework', 'CoreVideo',
        '-framework', 'CoreFoundation', '-framework', 'CoreMedia'], check=True)
    subprocess.run([str(exe)], check=True)
