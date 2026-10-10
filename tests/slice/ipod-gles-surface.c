/* Native surface import and writeback, against real CGL and IOSurface: IOSurface page faults and the shim's ABI,
 * native textured draws, NV12 ranges, deferred FBO writeback, ES 2.0 refresh and bounds, dirty-page refresh,
 * detached copies and IOSurface page records.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host.c fn gles_reject
 * SLICE hw/arm/gles-host.c range #define GLES_PRIVATE_NAME | static bool gles_is_drawable(
 * SLICE hw/arm/gles-host.c range static GLESPVRTC *gles_pvrtc_texture( | static int64_t gles_generate_mipmap(
 * SLICE:helpers hw/arm/gles-host.c range static bool gles_surface_range( | /* ------------------------------------------------------------ ES 2.0
 * SLICE:shim contrib/it-gles/mbxshim.c range static char *put_dec( | /* A stub that IS the implementation
 * SLICE:shim contrib/it-gles/mbxshim.c range static int guest_fault_read( | static unsigned texture_bytes(
 * SLICE:shim contrib/it-gles/mbxshim.c range static int surface_fault_read( | static int GLESBindCoreSurface(
 * SLICE:bind contrib/it-gles/mbxshim.c range static int GLESBindCoreSurface( | /*\n * GLESBindView is
 * SLICE:finish contrib/it-gles/mbxshim.c fn GLESFinishTexture GLESSwapNotification
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include <sys/mman.h>
#include "slice.h"
static uint8_t ram[0x100000];
int gles_guest_rw(CPUState *cpu, vaddr a, void *p, size_t n, bool write)
{
    if (a < 0x10000000 || a + n > 0x10000000 + sizeof(ram)) return -1;
    if (write) memcpy(ram + (a - 0x10000000), p, n);
    else memcpy(p, ram + (a - 0x10000000), n);
    return 0;
}
/* The guest pages map 1:1 onto ram (RAM offset = va - 0x10000000); the VGA dirty log is
 * exact: a page is dirty while its bytes differ from what was last cleared. */
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1u << TARGET_PAGE_BITS)
#define TARGET_PAGE_MASK (~(vaddr)(TARGET_PAGE_SIZE - 1))
#define DIRTY_MEMORY_VGA 0
#define WITH_RCU_READ_LOCK_GUARD()
typedef struct { int unused; } MemTxAttrs;
typedef struct { int unused; } MemoryRegion;
typedef struct AddressSpace AddressSpace;
static MemoryRegion fake_ram;
static uint8_t seen[sizeof(ram)];
static unsigned dirty_clears;
static vaddr unmapped_lo, unmapped_hi;      /* pages the process no longer maps */
static hwaddr cpu_get_phys_page_attrs_debug(CPUState *c, vaddr va, MemTxAttrs *attrs)
{ return va >= 0x10000000 && va < 0x10000000 + sizeof(ram) && !(va >= unmapped_lo && va < unmapped_hi) ? va : (hwaddr)-1; }
static int cpu_asidx_from_attrs(CPUState *c, MemTxAttrs attrs) { return 0; }
static AddressSpace *cpu_get_address_space(CPUState *c, int i) { return NULL; }
static MemoryRegion *address_space_translate(AddressSpace *as, hwaddr pa, hwaddr *xlat, hwaddr *len,
                                             bool w, MemTxAttrs attrs)
{ *xlat = pa - 0x10000000; return &fake_ram; }
static bool memory_region_is_ram(MemoryRegion *mr) { return mr == &fake_ram; }
static void memory_region_set_log(MemoryRegion *mr, bool on, unsigned client) {}
static ram_addr_t memory_region_get_ram_addr(MemoryRegion *mr) { return 0; }
static void *qemu_map_ram_ptr(void *block, ram_addr_t at) { return ram + at; }
static bool physical_memory_get_dirty_flag(ram_addr_t at, unsigned client)
{ return memcmp(ram + at, seen + at, TARGET_PAGE_SIZE) != 0; }
/* the writeback's stores to a surface's pages: dirty here is "differs from seen", already exact */
#define DIRTY_CLIENTS_ALL 0xff
static void physical_memory_set_dirty_range(ram_addr_t at, ram_addr_t len, uint8_t mask) {}
static bool physical_memory_test_and_clear_dirty(ram_addr_t at, ram_addr_t len, unsigned client,
                                                 unsigned long *bmap)
{
    bool d = memcmp(ram + at, seen + at, len) != 0;
    memcpy(seen + at, ram + at, len); dirty_clears += d;
    return d;
}
#include "helpers.h"
#define CA_FOURCC_555L GLES_SURFACE_RGB555
static void w(const char *s) {}
static void wd(unsigned v) {}
static void wx(unsigned v) {}
#define CA_FOURCC_565L GLES_SURFACE_RGB565
#define CA_FOURCC_BGRA GLES_SURFACE_BGRA32
static unsigned abi_format = 0x34323076;
static unsigned surface_lock_flags = 1;       /* IOSurface's read-only lock (CoreSurface: 2) */
static int abi_surface_fault_read(unsigned long base,unsigned stride,unsigned rows,unsigned bytes)
{ /* a packed surface's rows are touched for their pixels only (2 x 2 bytes in a 16-byte stride),
   * not their padding, which past the last row's pixels can be another, unmapped page */
  bool packed=abi_format==GLES_SURFACE_RGB555;
  assert((base==0x10000000 || base==0x10000100) && stride==(packed?16:2) && bytes==(packed?4:2) && rows<=2);return 1; }
#define A(...) (const unsigned[]){__VA_ARGS__}
static unsigned abi_args[9], abi_locks, abi_finished, abi_signals;
static int abi_signal_error;
static void iosurface_init(void) {}
static void surface_arg(void *s) { assert(s == (void *)0x1234); }
static int p_IOSurfaceLock(void *s, unsigned mode, unsigned *seed)
{ surface_arg(s); assert(mode==1); abi_locks++; return 0; }
static int p_IOSurfaceUnlock(void *s, unsigned mode, unsigned *seed)
{ surface_arg(s); assert(mode==1); abi_locks--; return 0; }
static void *p_IOSurfaceGetBaseAddress(void *s) { surface_arg(s); return (void *)0x10000000; }
static unsigned p_IOSurfaceGetBytesPerRow(void *s) { surface_arg(s); return abi_format==GLES_SURFACE_RGB555?16:2; }
static unsigned p_IOSurfaceGetBytesPerElement(void *s) { surface_arg(s); return 2; }
static unsigned p_IOSurfaceGetWidth(void *s) { surface_arg(s); return 2; }
static unsigned p_IOSurfaceGetHeight(void *s) { surface_arg(s); return 2; }
static unsigned p_IOSurfaceGetPixelFormat(void *s) { surface_arg(s); return abi_format; }
static unsigned p_IOSurfaceGetID(void *s) { surface_arg(s); return 4242; }
static unsigned p_IOSurfaceGetPlaneCount(void *s) { surface_arg(s); return abi_format==GLES_SURFACE_RGB555?0:2; }
static void *p_IOSurfaceGetBaseAddressOfPlane(void *s, unsigned plane)
{ surface_arg(s); return (void *)(uintptr_t)(0x10000000 + plane * 256); }
static unsigned p_IOSurfaceGetBytesPerRowOfPlane(void *s, unsigned plane)
{ surface_arg(s); return 2; }
static long long qc(unsigned op, void *gc, unsigned argc, const unsigned *a)
{ if(op==89) { assert(!argc); abi_finished++; return 0; } assert(op==GLES_OP_BIND_SURFACE && (argc==8 || argc==9)); memset(abi_args,0,sizeof(abi_args)); memcpy(abi_args,a,argc*4); return 0; }

#define RTLD_NOW 2
static int swap_signal(unsigned port,unsigned selector,const unsigned long long *args,
                       unsigned count,unsigned long long *out,unsigned *n)
{
    assert(abi_finished && port==0x123 && selector==20 && count==2);
    assert(args[0]==42 && args[1]==3 && !out && !n);abi_signals++;
    return abi_signal_error;
}
/* IOKit's swap signal; the 4.x fallback's IOMobileFramebuffer lookups find nothing here. */
static void *dlopen(const char *path,int mode) { assert(strstr(path,"IOKit") || strstr(path,"IOMobileFramebuffer"));return (void *)1; }
static void *dlsym(void *lib,const char *name)
{ assert(lib==(void *)1);return strcmp(name,"IOConnectCallScalarMethod") ? NULL : (void *)swap_signal; }
#include "shim.h"
/* GLESBindCoreSurface reads the surface through the fake above, not the real fault probe. */
#define surface_fault_read abi_surface_fault_read
#include "bind.h"
#undef surface_fault_read
#include "finish.h"
int main(void)
{
    void *fault_pages=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(fault_pages!=MAP_FAILED);
    assert(surface_fault_read((unsigned long)fault_pages+4095,8192,1,8192));
    assert(!surface_fault_read(~0UL-15,32,1,32));
    assert(!surface_fault_read(0x10000000,1,1,2));
    assert(GLESBindCoreSurface(NULL, 0x84f5, (void *)0x1234));
    assert(abi_args[0]==0x84f5 && abi_args[1]==0x10000000 && abi_args[6]==0x10000100 && abi_args[8]==4242);
    assert(!abi_locks);
    abi_format=GLES_SURFACE_RGB555;
    assert(GLESBindCoreSurface(NULL,0x84f5,(void *)0x1234));
    assert(abi_args[2]==16 && abi_args[5]==GLES_SURFACE_RGB555 && !abi_args[6] && !abi_locks);
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
    assert(!gles_bind_surface(NULL,a,0));
    uint8_t got[32]; glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int y=0;y<2;y++) assert(!memcmp(got+y*16,ram+y*20,16));
    glGenFramebuffersEXT(1,&fb);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fb);
    gh.bound_framebuffer=fb;
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,tex,0);
    assert(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
    /* A rendered-into surface is written back at the flush, not when the target is left:
     * the mark leaves guest memory alone, a refresh keeps the host's newer pixels. */
    glClearColor(0,1,1,1);glClear(GL_COLOR_BUFFER_BIT);assert(!gles_sync_surface(NULL));
    for(int y=0;y<2;y++) assert(ram[y*20+1]==y*80);
    gh.bound_framebuffer=0;glEnable(GL_TEXTURE_RECTANGLE_ARB);assert(gles_refresh_surfaces(NULL));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    assert(!memcmp(got,"\xff\xff\0\xff",4));gh.bound_framebuffer=fb;
    assert(!gles_surface_flush(NULL,0));
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
    assert(!gles_bind_surface(NULL,a,0));
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
    a[1]=0;assert(!gles_bind_surface(NULL,a,0));memset(ram,0x5a,40);
    glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);assert(!gles_sync_surface(NULL)&&!gles_surface_flush(NULL,0));
    for(int i=0;i<40;i++) assert(ram[i]==0x5a);
    a[1]=0xfffffff0;assert(gles_bind_surface(NULL,a,0)==-1);
    a[1]=0x10000000;a[2]=15;assert(gles_bind_surface(NULL,a,0)==-1);
    a[2]=20;a[3]=4097;assert(gles_bind_surface(NULL,a,0)==-1);
    a[3]=2;a[4]=2;a[2]=2;a[5]=0x34323076;a[6]=0x10000100;a[7]=2;
    memset(ram,16,4);ram[256]=ram[257]=128;assert(!gles_bind_surface(NULL,a,0));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<4;i++) assert(!memcmp(got+i*4,"\0\0\0\xff",4));
    memset(ram,235,4);assert(!gles_bind_surface(NULL,a,0));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<16;i++) assert(got[i]==255);
    a[5]=0x34323066;memset(ram,0,4);assert(!gles_bind_surface(NULL,a,0));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<4;i++) assert(!memcmp(got+i*4,"\0\0\0\xff",4));
    a[3]=3;assert(gles_bind_surface(NULL,a,0)==-1);
    /* The rest of what QuartzCore and IOSurface name, one 2x1 surface each, read back as BGRA bytes:
     * A008 and L008 sample (0,0,0,a) and (l,l,l,1); 4444 and 1555 are GL's own packed orders; ARGB and
     * ABGR are the 32-bit orders byte-reversed. */
    {
        struct { uint32_t fmt, stride; uint8_t in[8]; uint8_t out[8]; } cases[] = {
            { GLES_SURFACE_A8,       2, {0x40,0xff},            {0,0,0,0x40, 0,0,0,0xff} },
            { GLES_SURFACE_L8,       2, {0x40,0xff},            {0x40,0x40,0x40,0xff, 0xff,0xff,0xff,0xff} },
            { GLES_SURFACE_RGBA4444, 4, {0x0f,0xf0, 0xf0,0x0f}, {0,0,0xff,0xff, 0xff,0xff,0,0} },   /* 0xf00f: R=f A=f; 0x0ff0: G=f B=f */
            { GLES_SURFACE_RGBA5551, 4, {0x01,0xf8, 0x3f,0x00}, {0,0,0xff,0xff, 0xff,0,0,0xff} },   /* 0xf801 red, 0x003f blue */
            { GLES_SURFACE_ARGB32,   8, {0x80,1,2,3, 0xff,9,8,7}, {3,2,1,0x80, 7,8,9,0xff} },
            { GLES_SURFACE_ABGR32,   8, {0x80,1,2,3, 0xff,9,8,7}, {1,2,3,0x80, 9,8,7,0xff} },
        };
        for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            uint32_t b[] = { GL_TEXTURE_RECTANGLE_ARB, 0x10000000, cases[c].stride, 2, 1, cases[c].fmt, 0, 0 };
            memcpy(ram, cases[c].in, 8);
            assert(!gles_bind_surface(NULL,b,0));
            glGetTexImage(GL_TEXTURE_RECTANGLE_ARB, 0, GL_BGRA, GL_UNSIGNED_BYTE, got);
            assert(!memcmp(got, cases[c].out, 8));
        }
        /* and one nobody produces: refused, counted by its four characters */
        uint32_t b[] = { GL_TEXTURE_RECTANGLE_ARB, 0x10000000, 8, 2, 1, 0x58595a30, 0, 0 };
        assert(gles_bind_surface(NULL,b,0) == -1);
        char *rejects = gles_host_rejects();
        assert(strstr(rejects, "surface:XYZ0\t1\n"));
        free(rejects);
    }
    /* Photos thumbnail rows use opaque RGB555, with padded guest rows. */
    a[3]=4;a[4]=2;a[2]=10;a[5]=GLES_SURFACE_RGB555;a[6]=a[7]=0;
    const uint16_t colors[]={0x7c00,0x03e0,0x001f,0xffff};
    const uint8_t expected[]={0,0,255,255, 0,255,0,255, 255,0,0,255, 255,255,255,255};
    memset(ram,0xa5,20);
    for(int y=0;y<2;y++) memcpy(ram+y*10,colors,8);
    assert(!gles_bind_surface(NULL,a,0));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int y=0;y<2;y++) assert(!memcmp(got+y*16,expected,16));
    glClearColor(1,0,1,0);glClear(GL_COLOR_BUFFER_BIT);assert(!gles_sync_surface(NULL)&&!gles_surface_flush(NULL,0));
    for(int y=0;y<2;y++) {
        for(int x=0;x<4;x++) assert(ram[y*10+x*2]==0x1f && ram[y*10+x*2+1]==0x7c);
        assert(ram[y*10+8]==0xa5 && ram[y*10+9]==0xa5);
    }
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_RECTANGLE_ARB,0,0);
    memset(ram,0,20);assert(gles_refresh_surfaces(NULL));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<8;i++) assert(!memcmp(got+i*4,"\0\0\0\xff",4));
    /* ES1 refreshes only enabled targets; a bound ES 2.0 program samples without glEnable. */
    glDisable(GL_TEXTURE_RECTANGLE_ARB);for(int y=0;y<2;y++) memcpy(ram+y*10,colors,8);
    assert(gles_refresh_surfaces(NULL));
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<8;i++) assert(!memcmp(got+i*4,"\0\0\0\xff",4));
    gh.program=1;assert(gles_refresh_surfaces(NULL));gh.program=0;
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
    for(int y=0;y<2;y++) assert(!memcmp(got+y*16,expected,16));
    /* Only written memory is re-read. A texture whose pages nobody wrote keeps what it holds
     * (a host-side overwrite stands in for "not re-read"), a guest rebind of that memory too;
     * a write on any of its pages brings the guest's pixels back, and a second texture on the
     * same pages still sees the write after the first one's check cleared the log. */
    {
        uint32_t c[] = { GL_TEXTURE_RECTANGLE_ARB, 0x10000000 + 8192, 4096 + 16, 4, 2, GLES_SURFACE_BGRA32, 0, 0 };
        GLuint two[2]; uint8_t mark[32];
        memset(ram + 8192, 0x11, 8192); memset(mark, 0x77, sizeof(mark));
        glGenTextures(2, two);
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_RECTANGLE_ARB, two[i]); assert(!gles_bind_surface(NULL,c,0));
            glTexSubImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, 0, 0, 4, 2, GL_BGRA, GL_UNSIGNED_BYTE, mark);
        }
        glEnable(GL_TEXTURE_RECTANGLE_ARB);
        assert(gles_refresh_surfaces(NULL) && !gles_bind_surface(NULL,c,0));
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(!memcmp(got, mark, 32));
        unsigned clears = dirty_clears;
        ram[8192 + 4112] = 0x22;                        /* the second row's page */
        assert(gles_refresh_surfaces(NULL) && dirty_clears == clears + 1);
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(got[0] == 0x11 && got[16] == 0x22);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, two[0]);
        assert(gles_refresh_surfaces(NULL) && dirty_clears == clears + 1);
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(got[0] == 0x11 && got[16] == 0x22);
        /* A detached texture keeps the surface's pixels: re-attaching that memory, to the
         * same name or another, takes them from it (the host mark shows no re-read), and
         * an upload into the detached texture (gles_surface_forget) ends that. */
        uint32_t none[] = { GL_TEXTURE_RECTANGLE_ARB, 0, 0, 0, 0, 0, 0, 0 };
        GLuint third; glGenTextures(1, &third);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, two[0]);
        glTexSubImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, 0, 0, 4, 2, GL_BGRA, GL_UNSIGNED_BYTE, mark);
        assert(!gles_bind_surface(NULL, none, 0) && !gles_bind_surface(NULL, c, 0));
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(!memcmp(got, mark, 32));
        assert(!gles_bind_surface(NULL, none, 0));
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, third); assert(!gles_bind_surface(NULL, c, 0));
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(!memcmp(got, mark, 32));
        assert(!gles_bind_surface(NULL, none, 0));
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, two[0]); gles_surface_forget(GL_TEXTURE_RECTANGLE_ARB);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, two[1]); assert(!gles_bind_surface(NULL, none, 0));
        gles_surface_forget(GL_TEXTURE_RECTANGLE_ARB);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, third); assert(!gles_bind_surface(NULL, c, 0));
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(!memcmp(got, mark, 32));                 /* third's own detached copy */
        assert(!gles_bind_surface(NULL, none, 0)); gles_surface_forget(GL_TEXTURE_RECTANGLE_ARB);
        assert(!gles_bind_surface(NULL, c, 0));
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(got[0] == 0x11 && got[16] == 0x22);      /* no copy left: the guest's pixels */
        /* With the IOSurface ID, the pages are the surface's own: an attach after the
         * process dropped its mapping of them (but the last) stays tracked. */
        assert(!gles_bind_surface(NULL, none, 0)); gles_surface_forget(GL_TEXTURE_RECTANGLE_ARB);
        assert(!gles_bind_surface(NULL, c, 77));
        unmapped_lo = 0x10000000 + 8192; unmapped_hi = unmapped_lo + 4096;
        assert(!gles_bind_surface(NULL, none, 0)); gles_surface_forget(GL_TEXTURE_RECTANGLE_ARB);
        assert(!gles_bind_surface(NULL, c, 77));
        GLESSurface *tracked = g_hash_table_lookup(gh.surfaces, GUINT_TO_POINTER(third));
        assert(tracked && tracked->npages == 2);
        assert(!gles_bind_surface(NULL, none, 0)); gles_surface_forget(GL_TEXTURE_RECTANGLE_ARB);
        assert(!gles_bind_surface(NULL, c, 0));         /* no ID: through the mapping, untracked */
        tracked = g_hash_table_lookup(gh.surfaces, GUINT_TO_POINTER(third));
        assert(tracked && tracked->npages == 0);
        unmapped_lo = unmapped_hi = 0;
        /* Memory the process no longer maps is not the surface's: its next owner's writes
         * stay out of the texture, which keeps the surface's last pixels (issue 47). */
        assert(!gles_bind_surface(NULL, none, 0)); gles_surface_forget(GL_TEXTURE_RECTANGLE_ARB);
        memset(ram + 8192, 0x33, 8192);
        assert(!gles_bind_surface(NULL, c, 0) && gles_refresh_surfaces(NULL));
        unmapped_lo = 0x10000000 + 8192; unmapped_hi = unmapped_lo + 8192;
        memset(ram + 8192, 0x44, 8192);
        assert(gles_refresh_surfaces(NULL));
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(got[0] == 0x33 && got[16] == 0x33);
        assert(!g_hash_table_lookup(gh.surfaces, GUINT_TO_POINTER(third)));
        unmapped_lo = unmapped_hi = 0;
        /* So is a written page the process stopped mapping while it still maps the last one
         * (6.x under memory pressure: one wallpaper page in a hundred, issue 48). */
        assert(!gles_bind_surface(NULL, c, 0) && gles_refresh_surfaces(NULL));
        unmapped_lo = 0x10000000 + 8192; unmapped_hi = unmapped_lo + 4096;
        memset(ram + 8192, 0x55, 4096);
        assert(gles_refresh_surfaces(NULL));
        glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_BGRA,GL_UNSIGNED_BYTE,got);
        assert(got[0] == 0x44 && got[16] == 0x44);
        assert(!g_hash_table_lookup(gh.surfaces, GUINT_TO_POINTER(third)));
        unmapped_lo = unmapped_hi = 0;
        glDisable(GL_TEXTURE_RECTANGLE_ARB);
    }
    munmap(fault_pages,16384);
    g_hash_table_destroy(gh.surfaces);
    CGLSetCurrentContext(NULL);CGLDestroyContext(context);
    puts("PASS: IOSurface page faults and ABI, native textured draw, NV12 ranges, deferred FBO writeback, ES 2.0 refresh and bounds, dirty-page refresh, detached copies, IOSurface page records, unmapped surfaces keep their pixels");
}
