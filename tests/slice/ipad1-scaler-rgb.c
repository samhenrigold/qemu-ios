/* The scaler's 32-bit RGB path (scaler_rgb) behind a fake DART: a 1x iPhone app turned onto the iPad's landscape
 * panel, and its 2x destination mapped in 64 KiB windows of packed rows (issue 47).
 *
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
typedef int MemoryRegion;
typedef uint64_t hwaddr;
typedef int *qemu_irq;
static uint8_t ram[0x100000];
static void cpu_physical_memory_read(uint64_t a,void *p,size_t n)
{ assert(a+n<=sizeof(ram));memcpy(p,ram+a,n); }
static void cpu_physical_memory_write(uint64_t a,const void *p,size_t n)
{ assert(a+n<=sizeof(ram));memcpy(ram+a,p,n); }
static void stw_le_p(void *p,unsigned v) { uint8_t *b=p;b[0]=v;b[1]=v>>8; }
static void qemu_set_irq(qemu_irq irq,int level) { *irq=level; }
#define error_report(...) ((void)0)
#include "slice.h"

/* The source at IOVA 0x100000 maps straight to RAM 0; the destination at IOVA 0x200000 maps, in each 64 KiB window,
 * only the first `dst_pages` pages, onto packed RAM from 0x80000. */
static unsigned dst_pages = 16;
static hwaddr dart(void *opaque, uint32_t va, unsigned sid)
{
    if (va >= 0x100000 && va < 0x180000) return va - 0x100000;
    if (va >= 0x200000 && va < 0x300000 && ((va & 0xffff) >> 12) < dst_pages)
        return 0x80000 + ((va - 0x200000) >> 16) * dst_pages * 0x1000 + (va & 0xffff);
    return (hwaddr)-1;
}
static uint32_t src_px(unsigned u, unsigned v) { return 0xff000000u | v << 8 | u; }
static uint32_t at(uint64_t a) { uint32_t x; memcpy(&x, ram + a, 4); return x; }

static void go(IPodScalerState *s, unsigned sw, unsigned sh, unsigned dw, unsigned dh, unsigned stride, unsigned step)
{
    uint32_t *r = s->regs;
    memset(ram, 0, sizeof(ram));
    for (unsigned v = 0; v < sh; v++)
        for (unsigned u = 0; u < sw; u++) memcpy(ram + (v * ROUND_UP(sw, 64) + u) * 4, &(uint32_t){src_px(u, v)}, 4);
    r[0x10/4] = 6; r[0x30/4] = 0x406; r[0x14/4] = 0x100000; r[0x34/4] = 0x200000;
    r[0x24/4] = sw << 16 | sh; r[0x40/4] = dw << 16 | dh; r[0x3c/4] = stride / 4 << 16 | stride / 4;
    r[0x50/4] = r[0x54/4] = step;
    assert(scaler_rgb(s));
}

int main(void)
{
    int irq = 0;
    IPodScalerState s = {.irq = &irq};
    ipod_scaler_set_iommu((DeviceState *)&s, dart, NULL, 2);
    /* 1:1 with the size turned (320x480 -> 480x320 as measured, here 4x6 -> 6x4): destination (x, y) is source
     * column sw - 1 - y, row x, as the panel turns the portrait UI. */
    go(&s, 4, 6, 6, 4, 256, 0x10000);
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 6; x++) assert(at(0x80000 + y * 256 + x * 4) == src_px(3 - y, x));
    /* 2x into rows of 128 pixels at a programmed stride of 1024 bytes: 64 rows per 64 KiB window, packed at 512
     * bytes, 8 pages of each mapped (960x640 at 4096: 16 rows, 15 pages). Every row lands; none is dropped at the
     * first unmapped page. */
    dst_pages = 8;
    go(&s, 64, 64, 128, 128, 1024, 0x8000);
    for (unsigned y = 0; y < 128; y++)
        for (unsigned x = 0; x < 128; x += 37) assert(at(0x80000 + y * 512 + x * 4) == src_px(x / 2, y / 2));
    /* A stride that is the packed pitch stays linear (the 1x destination: 480 wide at 2048). */
    dst_pages = 16;
    go(&s, 64, 32, 64, 32, 256, 0x10000);
    for (unsigned y = 0; y < 32; y++) assert(at(0x80000 + y * 256 + 4) == src_px(1, y));
    puts("PASS: turned 1x destination, 2x destination in packed 64 KiB windows, linear destination");
}
