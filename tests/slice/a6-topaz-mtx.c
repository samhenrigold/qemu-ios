/* MTX firmware DMA walks the encoder's own two-level MMU, including page
 * boundaries, rejects missing mappings and overlong destinations, and reports
 * completion. The register port preserves values across read/write requests.
 * SLICE hw/arm/img_topaz.c define TOPAZ_
 * SLICE hw/arm/img_topaz.c range struct IMGTopazState { | static bool topaz_dma
 * SLICE hw/arm/img_topaz.c fn topaz_dma topaz_mtx_dma topaz_irq topaz_response_space topaz_response topaz_tile_picture topaz_encode topaz_commands topaz_read topaz_write
 * CFLAGS -I$ROOT/hw/arm
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "img_topaz_h264.h"
#define trace_a6_vxe_command(...) ((void)0)
#define trace_a6_vxe_control(...) ((void)0)
#define trace_a6_vxe_buffer(...) ((void)0)
#define trace_a6_vxe_source(...) ((void)0)
TopazH264 *topaz_h264_new(unsigned w, unsigned h)
{
    return NULL;
}
void topaz_h264_free(TopazH264 *s)
{
}
bool topaz_h264_encode(TopazH264 *s, const uint8_t *y, const uint8_t *uv,
                       unsigned stride, unsigned qp, bool idr, uint8_t **out,
                       size_t *n)
{
    static const uint8_t data[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0};
    *out = g_memdup2(data, sizeof(data));
    *n = sizeof(data);
    return true;
}
bool topaz_h264_picture(TopazH264 *s, uint8_t *y, uint8_t *uv, unsigned stride)
{
    memset(y, 37, 16 * 16);
    memset(uv, 125, 16 * 8);
    return true;
}
#define ROUND_UP(n, d) (((n) + (d) - 1) & ~((d) - 1))
typedef int SysBusDevice, MemoryRegion, qemu_irq, MemTxResult;
typedef uint64_t hwaddr;
typedef struct IMGTopazState IMGTopazState;
#define MEMTX_OK 0
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
static bool irq;
static void qemu_set_irq(int line, int level)
{
    irq = level;
}
static uint8_t ram[0x10000];
static int address_space_read(void *as, uint64_t pa, int attrs, void *dst,
                              size_t n)
{
    if (pa > sizeof(ram) || n > sizeof(ram) - pa)
        return 1;
    memcpy(dst, ram + pa, n);
    return 0;
}
static int address_space_write(void *as, uint64_t pa, int attrs,
                               const void *src, size_t n)
{
    if (pa > sizeof(ram) || n > sizeof(ram) - pa)
        return 1;
    memcpy(ram + pa, src, n);
    return 0;
}
static uint32_t address_space_ldl_le(void *as, uint64_t pa, int attrs,
                                     MemTxResult *r)
{
    uint32_t v = 0;
    *r = address_space_read(as, pa, attrs, &v, 4);
    return GUINT32_FROM_LE(v);
}
static uint16_t lduw_le_p(const void *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return GUINT16_FROM_LE(v);
}
static uint32_t ldl_le_p(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return GUINT32_FROM_LE(v);
}
static void stl_le_p(void *p, uint32_t v)
{
    v = GUINT32_TO_LE(v);
    memcpy(p, &v, 4);
}
#include "slice.h"
static IMGTopazState s;
static void word(unsigned a, uint32_t v)
{
    v = GUINT32_TO_LE(v);
    memcpy(ram + a, &v, 4);
}
int main(void)
{
    /* Native reconstruction tiles are 16x32 Y and 16x16 UV, in tile-row order. */
    uint8_t linear[32 * 64 * 3 / 2], tiled[sizeof(linear)];
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 32; x++)
            linear[y * 32 + x] = y + x * 3;
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            linear[2048 + y * 32 + x] = 70 + y + x * 5;
    topaz_tile_picture(tiled, linear, 32, 64);
    assert(tiled[0] == linear[0] && tiled[15] == linear[15]);
    assert(tiled[16] == linear[32] && tiled[511] == linear[31 * 32 + 15]);
    assert(tiled[512] == linear[16] && tiled[1024] == linear[32 * 32]);
    assert(tiled[2048 + 16] == linear[2048 + 32]);
    assert(tiled[2048 + 256] == linear[2048 + 16]);
    uint8_t short_frame[18 * 34 * 3 / 2], padded[32 * 64 * 3 / 2];
    memset(short_frame, 91, 18 * 34);
    memset(short_frame + 18 * 34, 151, 18 * 17);
    topaz_tile_picture(padded, short_frame, 18, 34);
    for (unsigned i = 0; i < 2048; i++)
        assert(padded[i] == 91);
    for (unsigned i = 2048; i < sizeof(padded); i++)
        assert(padded[i] == 151);

    word(0x1000, 0x2001);
    word(0x2004, 0x3001);
    word(0x2008, 0x5001);
    for (unsigned i = 0; i < 4096; i++) {
        ram[0x3000 + i] = i * 7;
        ram[0x5000 + i] = i * 11;
    }
    topaz_write(&s, 0x30, 0x1000, 4);
    topaz_write(&s, 0x400, 0x1ff0, 4);
    topaz_write(&s, 0xb44, TOPAZ_MTX_BASE + 0x20, 4);
    topaz_write(&s, 0xb40, 0x10000, 4);
    topaz_write(&s, 0x404, 0x20010010, 4);
    assert(topaz_read(&s, 0x40c, 4) == 0x20000);
    assert(!(topaz_read(&s, 0x404, 4) & 0x10000));
    assert(!(topaz_read(&s, 0xb40, 4) & 0x10000));
    assert(!memcmp(s.mtx_ram + 0x20, ram + 0x3ff0, 16));
    assert(!memcmp(s.mtx_ram + 0x30, ram + 0x5000, 48));
    memset(s.mtx_ram + 0x20, 0xa5, 64);
    topaz_write(&s, 0x404, 0x24010010, 4);
    assert(topaz_read(&s, 0x40c, 4) == 0x20000);
    for (unsigned i = 0; i < 16; i++)
        assert(ram[0x3ff0 + i] == 0xa5);
    for (unsigned i = 0; i < 48; i++)
        assert(ram[0x5000 + i] == 0xa5);
    word(0x2004, 0);
    topaz_write(&s, 0x404, 0x20010010, 4);
    assert(topaz_read(&s, 0x40c, 4) == 1);
    word(0x2004, 0x3001);
    topaz_write(&s, 0xb44, TOPAZ_MTX_BASE + TOPAZ_MTX_SIZE - 4, 4);
    topaz_write(&s, 0x404, 0x20010010, 4);
    assert(topaz_read(&s, 0x40c, 4) == 1);
    topaz_write(&s, 0x8f8, 0x80900000, 4);
    topaz_write(&s, 0x8fc, 0x51, 4);
    assert(topaz_read(&s, 0x8fc, 4) == 0x80000051);
    topaz_write(&s, 0x8f8, 0, 4);
    topaz_write(&s, 0x8fc, 0x10051, 4);
    assert(topaz_read(&s, 0x8f8, 4) == 0x80900000);
    topaz_write(&s, 0x44, 6, 4);
    assert(topaz_read(&s, 0x44, 4) == 0x0d0d0406);
    topaz_write(&s, 0x880, 1, 4);
    assert(topaz_read(&s, 0x104, 4) == 0);
    topaz_write(&s, 0xb44, TOPAZ_MTX_BASE, 4);
    topaz_write(&s, 0x404, 0x20010010, 4);
    topaz_write(&s, 0x880, 1, 4);
    assert(topaz_read(&s, 0x104, 4) == 0);
    topaz_write(&s, 0x8f8, 0x80900000, 4);
    topaz_write(&s, 0x8fc, 5, 4);
    topaz_write(&s, 0x880, 1, 4);
    assert(topaz_read(&s, 0x104, 4) == 0xed5);
    for (unsigned i = 0; i < 65; i++)
        topaz_write(&s, 0x60, i, 4);
    assert(topaz_read(&s, 0x64, 4) == 0);
    assert(s.command_count == 64 && s.commands[63] == 63);
    s.command_count = 4;
    s.commands[0] = 0x00a50004;
    s.commands[1] = 0x2200;
    s.commands[2] = 0x2300;
    s.commands[3] = 0x5a500;
    word(0x5208, (720 << 16) | 1280);
    word(0x52c4, 0x2500);
    topaz_write(&s, 0x104, 0, 4);
    topaz_write(&s, 0xc, 0x8000000e, 4);
    topaz_write(&s, 0x880, 1, 4);
    assert(!s.command_count && s.width == 1280 && s.height == 720);
    uint32_t reply;
    memcpy(&reply, ram + 0x5304, 4);
    assert(GUINT32_FROM_LE(reply) == 0x5a500);
    assert(ldl_le_p(ram + 0x5500) == 0 && ldl_le_p(ram + 0x5508) == 0x5a500);
    assert(topaz_read(&s, 0x104, 4) == 1 && irq);
    assert(topaz_read(&s, 4, 4) == 0x80000002);
    topaz_write(&s, 0x10, 2, 4);
    assert(!irq && !topaz_read(&s, 4, 4));
    /* The firmware must not overwrite an unconsumed ring slot, including wrap. */
    s.command_count = 4;
    s.commands[0] = 0x00a60004;
    s.commands[1] = 0x2200;
    s.commands[2] = 0x2300;
    s.commands[3] = 0x5a600;
    word(0x5340, 0x2600);
    topaz_write(&s, 0x104, 31, 4);
    topaz_write(&s, 0x880, 1, 4);
    assert(s.command_count == 4 && !irq);
    topaz_write(&s, 0x108, 1, 4);
    topaz_write(&s, 0x880, 1, 4);
    assert(s.command_count == 0 && topaz_read(&s, 0x104, 4) == 0 && irq);
    assert(ldl_le_p(ram + 0x5608) == 0x5a600);
    s.command_count = 4;
    s.commands[0] = 7;
    s.commands[1] = 0x2700;
    word(0x5700, 4);
    word(0x5704, 0x3000);
    word(0x5708, 4096);
    word(0x570c, 0xffffff03);
    topaz_write(&s, 0x880, 1, 4);
    assert(!s.command_count && s.coded[3][2]);
    word(0x5700, 0);
    word(0x5704, 2);
    word(0x5708, 0x1800);
    word(0x570c, 0x1900);
    s.command_count = 4;
    s.commands[0] = 7;
    s.commands[1] = 0x2700;
    topaz_write(&s, 0x880, 1, 4);
    assert(!s.command_count && s.source_count == 1 &&
           s.sources[0][0] == 0x1800);
    assert(s.sources[0][1] == 0x1900 && s.sources[0][2] == 2);
    /* A completed picture must identify its source and be marked coded;
  * otherwise the guest releases buffers but emits no video samples. */
    s.width = s.height = 16;
    s.reconstruction = 0x3800;
    word(0x200c, 0x6001);
    word(0x52c8, 0x2600);
    s.regs[0x104 / 4] = s.regs[0x108 / 4] = 0;
    s.command_count = 4;
    s.commands[0] = 3;
    s.commands[3] = 0x5a700;
    topaz_write(&s, 0x880, 1, 4);
    assert(!s.command_count && !s.source_count && !s.coded[3][2]);
    for (unsigned i = 0; i < 512; i++)
        assert(ram[0x6800 + i] == 37);
    for (unsigned i = 512; i < 768; i++)
        assert(ram[0x6800 + i] == 125);
    assert(!s.reconstruction);
    assert(ldl_le_p(ram + 0x6000) == 8);
    assert((ldl_le_p(ram + 0x6004) & 0xc3c) == 0xc08);
    assert(ldl_le_p(ram + 0x5500) == 1 && ldl_le_p(ram + 0x5508) == 3);
    assert(ldl_le_p(ram + 0x5600) == 0 && ldl_le_p(ram + 0x5608) == 0x5a700);
    /* Context save must complete without losing the host encoder state or
  * rewriting the caller's video context. This is used between iOS 7 frames. */
    word(0x52cc, 0x2700);
    s.command_count = 4;
    s.commands[0] = 0x00a80005;
    s.commands[1] = 0x2200;
    s.commands[2] = 0x2300;
    s.commands[3] = 0x5a800;
    unsigned frames = s.frame_count;
    topaz_write(&s, 0x880, 1, 4);
    assert(!s.command_count && s.frame_count == frames &&
           s.video_context == 0x2200);
    assert(ldl_le_p(ram + 0x5208) == ((720 << 16) | 1280));
    assert(ldl_le_p(ram + 0x5708) == 0x5a800);
    assert(ldl_le_p(ram + 0x5300) == 0x00a80005);
    /* A flush cannot pass an unencoded frame, and a completed flush leaves
  * the codec ready for context save or another low-delay picture. */
    word(0x5800, 5);
    word(0x5804, 10);
    s.source_count = 1;
    s.command_count = 4;
    s.commands[0] = 6;
    s.commands[1] = 0x2800;
    topaz_write(&s, 0x880, 1, 4);
    assert(s.command_count == 4);
    s.source_count = 0;
    topaz_write(&s, 0x880, 1, 4);
    assert(!s.command_count);
    assert(s.context_saved);
    topaz_write(&s, 0, 1, 4);
    assert(s.frame_count == frames && s.context_saved && !s.source_count);
    s.context_saved = false;
    topaz_write(&s, 0, 1, 4);
    assert(!s.frame_count);
    puts("ok");
}
