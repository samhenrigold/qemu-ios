/* The scaler's NV12-to-NV12 path (scaler_nv12): a decoded movie frame turned upright for the iPhone 4's portrait
 * panel (480x270 at stride 512 to 270x480 at stride 320 on a 4.2.1 device; small sizes here), both planes, and
 * the camera viewfinder's turn scaled down. And its NV12-to-RGB path (scaler_convert) behind a DART, as the 3GS
 * uses it. Both paths with tiled planes (+0x10/+0x30 bit 10, which AppleM2ScalerCSCHal sets whatever the format):
 * 16-row bands of tilesX * 256 bytes a row, potTilesX * 256 apart, the padding past each band unmapped, as the
 * kernel maps a tiled surface (scaler_row).
 *
 * Mutations (named, must fail this test): scaler_nv12 or scaler_convert reading or writing a tiled plane linear at
 * its stride; scaler_convert refusing a tiled format.
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
typedef struct { uint64_t addr; } MemoryRegion;
#define trace_ipod_scaler_reg_write(...) ((void)0)
#define trace_ipod_scaler_transfer(...) ((void)0)
typedef uint64_t hwaddr;
typedef int *qemu_irq;
static uint8_t ram[0x200000];
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

/* Tiled planes: [base, base + bands * 16 * stride), only the first 16 * pitch bytes of each band mapped. */
static struct { uint32_t base, stride, pitch, bands; } tiled[8];
static unsigned ntiled;

/* IOVA 0x100000 + n is RAM n, but for a tiled plane's padding. */
static hwaddr dart(void *opaque, uint32_t va, unsigned sid)
{
    for (unsigned i = 0; i < ntiled; i++)
        if (va >= tiled[i].base && va < tiled[i].base + tiled[i].bands * 16 * tiled[i].stride &&
            (va - tiled[i].base) % (16 * tiled[i].stride) >= 16 * tiled[i].pitch)
            return (hwaddr)-1;
    return va >= 0x100000 ? va - 0x100000 : (hwaddr)-1;
}

/* A plane of `w` bytes by `h` rows at `base`, tiled at `stride` (a power of two of 256-byte tiles). */
static void tiled_plane(uint32_t base, unsigned w, unsigned h, unsigned stride)
{
    tiled[ntiled].base = base; tiled[ntiled].stride = stride; tiled[ntiled].pitch = ROUND_UP(w, 256);
    tiled[ntiled++].bands = (h + 15) / 16;
}
/* Row y of such a plane. */
static uint8_t *tiled_at(uint32_t base, unsigned y, unsigned stride, unsigned w)
{
    return &ram[base - 0x100000 + y / 16 * 16 * stride + y % 16 * ROUND_UP(w, 256)];
}

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
    /* Turned and scaled down, as the camera's viewfinder (2592x1936 to 640x852): 12x6 to 4x8, every destination
     * sample the source's nearest at the size ratio (1.5 both ways). */
    enum { DW = 4, DH = 8 };
    memset(&ram[DY - 0x100000], 0, 0x2000);
    r[0x40/4] = DW << 16 | DH;
    assert(scaler_nv12(&s));
    for (unsigned j = 0; j < DH; j++)
        for (unsigned x = 0; x < DW; x++)
            assert(ram[DY - 0x100000 + j * DYS + x] == luma(j * SW / DH, SH - 1 - x * SH / DW));
    for (unsigned j = 0; j < DH / 2; j++)
        for (unsigned x = 0; x < DW / 2; x++) {
            assert(ram[DUV - 0x100000 + j * DYS + 2 * x] == cb(j * SW / DH, SH / 2 - 1 - x * SH / DW));
            assert(ram[DUV - 0x100000 + j * DYS + 2 * x + 1] == cr(j * SW / DH, SH / 2 - 1 - x * SH / DW));
        }
    /* An RGB destination is not this path's. */
    r[0x40/4] = SH << 16 | SW; r[0x30/4] = 6;
    assert(!scaler_nv12(&s));

    /* NV12 to BGRA, unscaled, through the DART: the 3GS's movie path on 3.1.3 (480x270 to 480x270 at 32 bpp, its
     * buffers dart0 IOVAs). The matrix here (2.9, rows R, G, B) gives each channel the luma. */
    enum { DRGB = 0x104000 };
    memset(r, 0, 0x80);
    r[0x14/4] = Y; r[0x18/4] = UV; r[0x1c/4] = SYS << 16 | SYS; r[0x24/4] = SW << 16 | SH;
    r[0x30/4] = 6; r[0x34/4] = DRGB; r[0x3c/4] = SW; r[0x40/4] = SW << 16 | SH;
    for (unsigned i = 0; i < 9; i++)
        r[0x220/4 + i] = i % 3 ? 0 : 0x200;
    assert(scaler_convert(&s));
    for (unsigned y = 0; y < SH; y++)
        for (unsigned x = 0; x < SW; x++) {
            const uint8_t *px = &ram[DRGB - 0x100000 + (y * SW + x) * 4];
            assert(px[0] == luma(x, y) && px[1] == luma(x, y) && px[2] == luma(x, y) && px[3] == 255);
        }

    /* Tiled NV12 to NV12, unscaled and unturned: 600x34 (3 tiles of 4 across, 34 rows: three bands of luma, two of
     * chroma), the source tiled at a stride of 1024, into a linear destination; then from that into a tiled one. */
    enum { TW = 600, TH = 34, TS = 1024, TY = 0x110000, TUV = 0x120000, LY = 0x130000, LUV = 0x140000,
           T2Y = 0x150000, T2UV = 0x160000 };
    tiled_plane(TY, TW, TH, TS); tiled_plane(TUV, TW, TH / 2, TS);
    tiled_plane(T2Y, TW, TH, TS); tiled_plane(T2UV, TW, TH / 2, TS);
    for (unsigned y = 0; y < TH; y++)
        for (unsigned x = 0; x < TW; x++) {
            tiled_at(TY, y, TS, TW)[x] = (uint8_t)(x * 7 + y * 13);
            if (y < TH / 2) tiled_at(TUV, y, TS, TW)[x] = (uint8_t)(x * 5 + y * 11 + 1);
        }
    memset(r, 0, 0x80);
    r[0x10/4] = 0x400; r[0x14/4] = TY; r[0x18/4] = TUV; r[0x1c/4] = TS << 16 | TS; r[0x24/4] = TW << 16 | TH;
    r[0x34/4] = LY; r[0x38/4] = LUV; r[0x3c/4] = TW << 16 | TW; r[0x40/4] = TW << 16 | TH;
    assert(scaler_nv12(&s));
    for (unsigned y = 0; y < TH; y++)
        for (unsigned x = 0; x < TW; x++) {
            assert(ram[LY - 0x100000 + y * TW + x] == (uint8_t)(x * 7 + y * 13));
            if (y < TH / 2) assert(ram[LUV - 0x100000 + y * TW + x] == (uint8_t)(x * 5 + y * 11 + 1));
        }
    r[0x10/4] = 0; r[0x14/4] = LY; r[0x18/4] = LUV; r[0x1c/4] = TW << 16 | TW;
    r[0x30/4] = 0x400; r[0x34/4] = T2Y; r[0x38/4] = T2UV; r[0x3c/4] = TS << 16 | TS;
    assert(scaler_nv12(&s));
    for (unsigned y = 0; y < TH; y++)
        for (unsigned x = 0; x < TW; x++) {
            assert(tiled_at(T2Y, y, TS, TW)[x] == (uint8_t)(x * 7 + y * 13));
            if (y < TH / 2) assert(tiled_at(T2UV, y, TS, TW)[x] == (uint8_t)(x * 5 + y * 11 + 1));
        }

    /* Tiled NV12 to BGRA, and NV12 to a tiled BGRA destination (600 pixels: 2400 bytes, 10 tiles of 16 across, at
     * 4096), the luma matrix as above. */
    enum { TRGB = 0x170000, TRS = 4096 };
    memset(r, 0, 0x80);
    for (unsigned i = 0; i < 9; i++)
        r[0x220/4 + i] = i % 3 ? 0 : 0x200;
    r[0x10/4] = 0x400; r[0x14/4] = TY; r[0x18/4] = TUV; r[0x1c/4] = TS << 16 | TS; r[0x24/4] = TW << 16 | TH;
    r[0x30/4] = 6; r[0x34/4] = LY; r[0x3c/4] = TW; r[0x40/4] = TW << 16 | TH;
    assert(scaler_convert(&s));
    for (unsigned y = 0; y < TH; y++)
        for (unsigned x = 0; x < TW; x++)
            assert(ram[LY - 0x100000 + (y * TW + x) * 4] == (uint8_t)(x * 7 + y * 13));
    tiled_plane(TRGB, TW * 4, TH, TRS);
    r[0x30/4] = 0x406; r[0x34/4] = TRGB; r[0x3c/4] = TRS / 4;
    assert(scaler_convert(&s));
    for (unsigned y = 0; y < TH; y++)
        for (unsigned x = 0; x < TW; x++)
            assert(tiled_at(TRGB, y, TRS, TW * 4)[x * 4 + 1] == (uint8_t)(x * 7 + y * 13));
    printf("ok\n");
    return 0;
}
