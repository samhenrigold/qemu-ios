/* Tagged decoder contexts sharing a page directory must retain separate DPBs.
 * SLICE:types hw/arm/s5l8920_vxd.c define VXD_STREAMS|VXD_TARGETS
 * SLICE:types hw/arm/s5l8920_vxd.c typedef VXDTarget VXDStream
 * SLICE hw/arm/s5l8920_vxd.c fn vxd_stream vxd_streams_clear
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef struct VXDH264 {
    unsigned frames;
} VXDH264;
static unsigned live;
static VXDH264 *vxd_h264_new(void)
{
    live++;
    return g_new0(VXDH264, 1);
}
static void vxd_h264_free(VXDH264 *codec)
{
    if (codec) {
        live--;
        g_free(codec);
    }
}
#include "types.h"
typedef struct {
    VXDStream streams[VXD_STREAMS];
    uint64_t stream_clock;
} S5L8920VXDState;
#include "slice.h"
int main(void)
{
    S5L8920VXDState s = {0};
    VXDStream *thumb = vxd_stream(&s, 0x12345000);
    VXDStream *play = vxd_stream(&s, 0x12345001);
    assert(thumb != play && live == 2);
    thumb->codec->frames = 1;
    play->codec->frames = 7;
    assert(vxd_stream(&s, 0x12345000)->codec->frames == 1);
    assert(vxd_stream(&s, 0x12345001)->codec->frames == 7);
    thumb->target[1] = (VXDTarget){.seq = 1, .luma = 0x1000};
    play->target[1] = (VXDTarget){.seq = 1, .luma = 0x2000};
    assert(thumb->target[1].luma == 0x1000 && play->target[1].luma == 0x2000);
    /* Fill the bounded cache; keep playback active as idle streams leave. */
    for (unsigned i = 2; i < VXD_STREAMS; i++)
        vxd_stream(&s, 0x12345000 + i);
    assert(live == VXD_STREAMS);
    assert(vxd_stream(&s, 0x12345001) == play);
    vxd_stream(&s, 0x98765000);
    assert(live == VXD_STREAMS &&
           vxd_stream(&s, 0x12345001)->codec->frames == 7);
    assert(vxd_stream(&s, 0x12345000)->codec->frames ==
           0); /* oldest was evicted */
    vxd_streams_clear(&s);
    assert(!live && !s.stream_clock);
    for (unsigned i = 0; i < VXD_STREAMS; i++)
        assert(!s.streams[i].codec);
    vxd_streams_clear(&s);
    assert(!live);
    puts("ok: tagged VXD stream isolation, bounded retirement and reset");
}
