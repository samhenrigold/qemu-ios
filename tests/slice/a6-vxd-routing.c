/* Reordered pictures from interleaved tagged contexts keep their original DMA
 * destinations and extended strides. Codec pixels are stubbed here; the real
 * H.264 fixture suite independently verifies decoding and reorder tags.
 * SLICE:types hw/arm/s5l8920_vxd.c define VXD_STREAMS|VXD_TARGETS|VXD_BS_SEGS
 * SLICE:types hw/arm/s5l8920_vxd.c typedef VXDTarget VXDStream VXDRender
 * SLICE hw/arm/s5l8920_vxd.c fn vxd_stream vxd_streams_clear vxd_decode
 * PKG glib-2.0
 * CFLAGS -I$ROOT/hw/arm
 */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "s5l8920_vxd_h264.h"
struct VXDH264 {
    int calls, next;
    int64_t tags[2];
};
VXDH264 *vxd_h264_new(void)
{
    return g_new0(VXDH264, 1);
}
void vxd_h264_free(VXDH264 *p)
{
    g_free(p);
}
#include "types.h"
typedef struct {
    VXDStream streams[VXD_STREAMS];
    uint64_t stream_clock;
    FILE *trace;
} S5L8920VXDState;
static uint8_t ram[65536], y[8], u[2] = {80, 80}, v[2] = {90, 90};
static bool vxd_rw(uint32_t ptd, uint32_t va, void *p, size_t n, bool write)
{
    assert(va + n <= sizeof(ram));
    if (write)
        memcpy(ram + va, p, n);
    else
        memcpy(p, ram + va, n);
    return true;
}
static int vxd_nal_start(const uint8_t *p, int back, unsigned len, bool *annexb)
{
    *annexb = false;
    return back;
}
static bool vxd_more_slices(const uint8_t p[6], bool a)
{
    return false;
}
static int emit(VXDH264 *h, VXDPicture *p)
{
    if (h->next == 2)
        return 0;
    int i = h->next++;
    memset(y, 11 * (i + 1), sizeof(y));
    *p = (VXDPicture){.plane = {y, u, v},
                      .stride = {4, 2, 2},
                      .width = 4,
                      .height = 2,
                      .tag = h->tags[i]};
    return 1;
}
int vxd_h264_decode(VXDH264 *h, const VXDSlice *s, VXDPicture *p)
{
    assert(h->calls < 2);
    h->tags[h->calls++] = s->tag;
    return h->calls == 1 ? 0 : emit(h, p);
}
int vxd_h264_next(VXDH264 *h, VXDPicture *p)
{
    return emit(h, p);
}
#define VXD_LOG(...) ((void)0)
#include "slice.h"
int main(void)
{
    S5L8920VXDState s = {0};
    memset(ram, 0xa5, sizeof(ram));
    VXDRender a = {.bs_va = 0x400,
                   .bs_len = 1,
                   .seg_va = {0x400},
                   .seg_len = {1},
                   .segs = 1,
                   .luma = 0x1000,
                   .chroma = 0x2000,
                   .mode = 1u << 28,
                   .row_stride = 64};
    VXDRender b = a;
    b.mode = 0;
    b.luma = 0x3000;
    b.chroma = 0x4000;
    assert(vxd_decode(&s, 0x123000, &a) == 0);
    assert(vxd_decode(&s, 0x123001, &b) == 0);
    a.luma = 0x5000;
    a.chroma = 0x6000;
    assert(vxd_decode(&s, 0x123000, &a) == 1);
    assert(ram[0x1000] == 11 && ram[0x1040] == 11 && ram[0x5000] == 22);
    assert(ram[0x2000] == 80 && ram[0x2001] == 90 && ram[0x3000] == 0xa5);
    b.luma = 0x7000;
    b.chroma = 0x8000;
    assert(vxd_decode(&s, 0x123001, &b) == 1);
    assert(ram[0x3000] == 11 && ram[0x3180] == 11 && ram[0x7000] == 22);
    assert(ram[0x4000] == 80 && ram[0x4001] == 90 && ram[0x1004] == 0xa5);
    vxd_streams_clear(&s);
    puts("ok: per-stream reordered DMA targets and extended strides");
}
