/* The scaler's 32-bit RGB path (scaler_rgb) through the A4's DART (s5l8930_dart_xlate on segment and page tables
 * programmed as AppleS5L8930XDART programs them), on buffers whose pages are scattered through RAM in reverse, so no
 * layout of physical memory is assumed: a 1x iPhone app turned onto the iPad's landscape panel and its 2x into a
 * tiled destination (issue 47); Zoom's source, linear at the stride 4.x programs (issue 54) and tiled on the iPad 3.2;
 * 5.1.1's 2x of a 1x app from its tiled 320x480 layer (issue 96); and a linear buffer at a power-of-two stride past
 * its width, which only the format's tiled bit (+0x10/+0x30 bit 10) tells from a tiled one.
 *
 * Mutations (must fail this test): read the source as rows packed at ALIGN(w, 64) (the gather before issue 54);
 * turn every quarter turn counterclockwise, whatever +0x58 asks for (Smash Hit upside down, issue 47); take a buffer
 * as tiled when its stride is a power of two past its packed width, whatever bit 10 says (the linear destination
 * at 1024 then lands in 16-row bands); band the rows 64 KiB / stride at a time instead of 16 (5.1.1's 2x refused,
 * issue 96); read a tiled buffer as linear.
 *
 * SLICE include/hw/arm/s5l8930.h define S5L8930_DART_
 * SLICE include/hw/arm/s5l8930.h typedef S5L8930Dart
 * SLICE hw/arm/s5l8930_dart.c define DART_
 * SLICE hw/arm/s5l8930_dart.c fn dart_write s5l8930_dart_xlate
 * SLICE hw/arm/ipod_touch_scaler.c range typedef struct { | static const MemoryRegionOps
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef int SysBusDevice;
typedef int DeviceState;
#define ROUND_UP(n,d) (((n)+(d)-1)/(d)*(d))
typedef struct { uint64_t addr; } MemoryRegion;
#define trace_ipod_scaler_reg_write(...) ((void)0)
#define trace_ipod_scaler_transfer(...) ((void)0)
typedef uint64_t hwaddr;
typedef int *qemu_irq;
static uint8_t ram[0x400000];
static void cpu_physical_memory_read(uint64_t a,void *p,size_t n)
{ assert(a+n<=sizeof(ram));memcpy(p,ram+a,n); }
static void cpu_physical_memory_write(uint64_t a,const void *p,size_t n)
{ assert(a+n<=sizeof(ram));memcpy(ram+a,p,n); }
static void stw_le_p(void *p,unsigned v) { uint8_t *b=p;b[0]=v;b[1]=v>>8; }
static void qemu_set_irq(qemu_irq irq,int level) { *irq=level; }
#define error_report(...) ((void)0)
#define qemu_log_mask(...) ((void)0)
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
static uint32_t address_space_ldl_le(int *as, hwaddr a, int attrs, void *res)
{ uint32_t v; (void)as; (void)attrs; (void)res; assert(a + 4 <= sizeof(ram)); memcpy(&v, ram + a, 4); return v; }
typedef int QEMUTimer;   /* scaler_write's end of a transfer; scaler_rgb is called directly here */
enum { QEMU_CLOCK_VIRTUAL };
static int64_t qemu_clock_get_ns(int c) { (void)c; return 0; }
static void timer_mod(QEMUTimer *t, int64_t at) { (void)t; (void)at; }
static void timer_del(QEMUTimer *t) { (void)t; }
static bool timer_pending(QEMUTimer *t) { (void)t; return false; }
#include "slice.h"

/* dart2, stream 2 (the scaler's mapper), segment 0 through one page table at PT. */
#define PT 0x3ff000
static S5L8930Dart dart;
static hwaddr xlate(void *opaque, uint32_t va, unsigned sid) { return s5l8930_dart_xlate(opaque, sid, va); }
static void pte(uint32_t iova, uint32_t pa) { uint32_t v = pa | 1; memcpy(ram + PT + (iova >> 12) * 4, &v, 4); }

/* A buffer whose page k is the (k + 1)th page below `top`: the CPU's view of it, page by page, is the surface. */
typedef struct { uint32_t iova, top, pitch; } Buf;
static uint32_t page(const Buf *b, unsigned k) { return b->top - (k + 1) * 0x1000; }
static uint32_t *px(const Buf *b, unsigned x, unsigned y)
{ uint32_t l = y * b->pitch + x * 4; return (uint32_t *)(ram + page(b, l >> 12) + (l & 0xfff)); }
/* Linear: the pages in order from the IOVA. */
static void map_linear(const Buf *b, unsigned rows)
{ for (unsigned k = 0; k * 0x1000 < rows * b->pitch; k++) pte(b->iova + k * 0x1000, page(b, k)); }
/* Tiled at `stride` bytes (potTilesX * 256): each row of tiles' pages from its start, 16 strides apart. */
static void map_tiled(const Buf *b, unsigned rows, unsigned stride)
{
    unsigned tiles = b->pitch / 256;
    for (unsigned ty = 0; ty < (rows + 15) / 16; ty++)
        for (unsigned tx = 0; tx < tiles; tx++) pte(b->iova + (ty * stride / 256 + tx) * 0x1000, page(b, ty * tiles + tx));
}
static uint32_t src_px(unsigned u, unsigned v) { return 0xff000000u | v << 8 | u; }

static const Buf SRC = {0x100000, 0x200000, 0}, DST = {0x300000, 0x3fe000, 0};

/* sw x sh from SRC (CPU rows `spitch` bytes, programmed at `ss` pixels, `sfmt`) to dw x dh in DST likewise. */
static void go(IPodScalerState *s, unsigned sw, unsigned sh, unsigned spitch, unsigned ss, uint32_t sfmt,
               unsigned dw, unsigned dh, unsigned dpitch, unsigned ds, uint32_t dfmt, unsigned step)
{
    uint32_t *r = s->regs;
    Buf src = SRC, dst = DST;
    src.pitch = spitch;
    dst.pitch = dpitch;
    memset(ram, 0, sizeof(ram));
    for (unsigned v = 0; v < sh; v++)
        for (unsigned u = 0; u < sw; u++) *px(&src, u, v) = src_px(u, v);
    if (sfmt & 0x400) map_tiled(&src, sh, ss * 4); else map_linear(&src, sh);
    if (dfmt & 0x400) map_tiled(&dst, dh, ds * 4); else map_linear(&dst, dh);
    r[0x10/4] = sfmt; r[0x30/4] = dfmt; r[0x14/4] = SRC.iova; r[0x34/4] = DST.iova;
    r[0x1c/4] = ss << 16 | ss;
    r[0x24/4] = sw << 16 | sh; r[0x40/4] = dw << 16 | dh; r[0x3c/4] = ds << 16 | ds;
    r[0x50/4] = r[0x54/4] = step;
    assert(scaler_rgb(s));
}
static uint32_t at(unsigned dpitch, unsigned x, unsigned y) { Buf d = DST; d.pitch = dpitch; return *px(&d, x, y); }

int main(void)
{
    int irq = 0;
    IPodScalerState s = {.irq = &irq};
    /* The driver's way: DATA = the page table, then TLB_OP = seg << 22 | sid << 8 | 5. */
    dart_write(&dart, DART_DATA, PT, 4);
    dart_write(&dart, DART_TLB_OP, 0 << 22 | 2 << 8 | 5, 4);
    ipod_scaler_set_iommu((DeviceState *)&s, xlate, &dart, 2);

    /* 1:1 with the size turned (320x480 -> 480x320 as measured, here 4x6 -> 6x4): destination (x, y) is source
     * column sw - 1 - y, row x, as the panel turns the portrait UI. */
    go(&s, 4, 6, 256, 64, 6, 6, 4, 256, 64, 6, 0x10000);
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 6; x++) assert(at(256, x, y) == src_px(3 - y, x));
    /* +0x58 = 3 turns it the other way, clockwise: a landscape-right app's 960x640 layer onto the iPhone 4's
     * portrait panel (here 6x4 -> 4x6), destination (x, y) is source column y, row sh - 1 - x. */
    s.regs[0x58 / 4] = 3;
    go(&s, 6, 4, 256, 64, 6, 4, 6, 256, 64, 6, 0x10000);
    for (unsigned y = 0; y < 6; y++)
        for (unsigned x = 0; x < 4; x++) assert(at(256, x, y) == src_px(y, 3 - x));
    s.regs[0x58 / 4] = 0;
    /* 2x into a tiled destination of 128 pixels at a stride of 256 (2 tiles of 4 across, as 960x640 at 1024 is 15
     * of 16): every row lands, none dropped at the first unmapped page. */
    go(&s, 64, 64, 256, 64, 6, 128, 128, 512, 256, 0x406, 0x8000);
    for (unsigned y = 0; y < 128; y++)
        for (unsigned x = 0; x < 128; x += 37) assert(at(512, x, y) == src_px(x / 2, y / 2));
    /* A linear destination at a power-of-two stride past its width (64 pixels at 256): rows at the stride. */
    go(&s, 64, 40, 256, 64, 6, 64, 40, 1024, 256, 6, 0x10000);
    for (unsigned y = 0; y < 40; y++) assert(at(1024, 5, y) == src_px(5, y));
    /* Zoom on 4.x: a 27-wide crop at the 32-pixel stride it is programmed with (427 at 432 on the iPhone 4), not
     * packed at ALIGN(27, 64) = 64, blown up 2x. */
    go(&s, 27, 20, 128, 32, 6, 54, 40, 256, 64, 6, 0x8000);
    for (unsigned y = 0; y < 40; y++)
        for (unsigned x = 0; x < 54; x += 5) assert(at(256, x, y) == src_px(x / 2, y / 2));
    /* Zoom on the iPad 3.2: a tiled source at a stride of 256 pixels (1 tile of 4 across; 683 wide at 1024 is 11 of
     * 16), here across 5 rows of tiles. */
    go(&s, 27, 80, 256, 256, 0x406, 27, 40, 256, 64, 6, 0x20000);
    for (unsigned y = 0; y < 40; y++) assert(at(256, 26, y) == src_px(26, y * 2));
    /* 5.1.1's 2x of a 1x app (issue 96): the app's own tiled 320x480 layer is the source at a stride of 512 pixels
     * (5 tiles of 8 across); here turned 1:1 onto the 480x320 destination. */
    go(&s, 320, 480, 1280, 512, 0x406, 480, 320, 1920, 480, 6, 0x10000);
    for (unsigned y = 0; y < 320; y += 7)
        for (unsigned x = 0; x < 480; x += 13) assert(at(1920, x, y) == src_px(319 - y, x));
    puts("PASS: through the DART on scattered pages: turned 1x, a clockwise turn by +0x58, 2x into a tiled "
         "destination, a linear destination at a power-of-two stride, Zoom's source linear at its stride and tiled, "
         "5.1.1's tiled 2x source");
}
