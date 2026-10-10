/* The scaler's NV12-to-NV12 path (scaler_nv12): a decoded movie frame turned upright for the iPhone 4's portrait
 * panel (480x270 at stride 512 to 270x480 at stride 320 on a 4.2.1 device; small sizes here), both planes.
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
typedef int QEMUTimer;
enum { QEMU_CLOCK_VIRTUAL };
static int64_t qemu_clock_get_ns(int c) { (void)c; return 0; }
static void timer_mod(QEMUTimer *t, int64_t at) { (void)t; (void)at; }
static void timer_del(QEMUTimer *t) { (void)t; }
static bool timer_pending(QEMUTimer *t) { (void)t; return false; }
#include "slice.h"

/* IOVA 0x100000 + n is RAM n. */
static hwaddr dart(void *opaque, uint32_t va, unsigned sid) { return va >= 0x100000 ? va - 0x100000 : (hwaddr)-1; }

enum { SW = 12, SH = 6, SYS = 16, DYS = 8, Y = 0x100000, UV = 0x101000, DY = 0x102000, DUV = 0x103000 };
static uint8_t luma(unsigned x, unsigned y) { return 16 * y + x; }
static uint8_t cb(unsigned x, unsigned y) { return 0x80 + 8 * y + x; }    /* x, y: the chroma sample's position */
static uint8_t cr(unsigned x, unsigned y) { return 0xc0 + 8 * y + x; }

int main(void)
{
    IPodScalerState s = {0};
    uint32_t *r = s.regs;

    s.xlate = dart;
    for (unsigned y = 0; y < SH; y++)
        for (unsigned x = 0; x < SW; x++) {
            ram[Y - 0x100000 + y * SYS + x] = luma(x, y);
            if (!(y & 1) && !(x & 1)) {
                ram[UV - 0x100000 + y / 2 * SYS + x] = cb(x / 2, y / 2);
                ram[UV - 0x100000 + y / 2 * SYS + x + 1] = cr(x / 2, y / 2);
            }
        }
    r[0x14/4] = Y; r[0x18/4] = UV; r[0x1c/4] = SYS << 16 | SYS; r[0x24/4] = SW << 16 | SH;
    r[0x34/4] = DY; r[0x38/4] = DUV; r[0x3c/4] = DYS << 16 | DYS; r[0x40/4] = SH << 16 | SW;
    assert(scaler_nv12(&s));
    /* Turned a quarter: destination (x, j) is source row SH - 1 - x, column j; the same for chroma at half size. */
    for (unsigned j = 0; j < SW; j++)
        for (unsigned x = 0; x < SH; x++)
            assert(ram[DY - 0x100000 + j * DYS + x] == luma(j, SH - 1 - x));
    for (unsigned j = 0; j < SW / 2; j++)
        for (unsigned x = 0; x < SH / 2; x++) {
            assert(ram[DUV - 0x100000 + j * DYS + 2 * x] == cb(j, SH / 2 - 1 - x));
            assert(ram[DUV - 0x100000 + j * DYS + 2 * x + 1] == cr(j, SH / 2 - 1 - x));
        }
    /* Neither a scaled transfer nor an RGB one is this path's. */
    r[0x40/4] = SH / 2 << 16 | SW / 2;
    assert(!scaler_nv12(&s));
    r[0x40/4] = SH << 16 | SW; r[0x30/4] = 6;
    assert(!scaler_nv12(&s));
    printf("ok\n");
    return 0;
}
