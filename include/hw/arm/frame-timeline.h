#ifndef HW_ARM_FRAME_TIMELINE_H
#define HW_ARM_FRAME_TIMELINE_H
/*
 * A fixed ring of latched-frame events for the display models, timestamped in
 * guest-virtual time (QEMU_CLOCK_VIRTUAL): one entry per panel vsync, flagged
 * "new" when the scanned-out content changed that period and "held" when the
 * same frame was shown again.
 *
 * This is the ground truth the jank harness (tests/ipad1/jank.py) reads: frame
 * intervals, hitches, dropped and duplicated frames all come off these virtual-
 * time stamps, so the metric is deterministic and immune to host load -- unlike
 * the old wall-clock fps sampling (perf.py read QEMU_CLOCK_REALTIME; perf_ipad.py
 * sampled swaps over host time.time()), whose numbers moved with the machine's
 * load and proved nothing about smoothness. See docs/perf-jank.md.
 *
 * Recording is two stores per vsync (<=60 Hz), so there is no measurable cost on
 * the frame path; the ring is read out of band through the "frame-timeline" QOM
 * property, so nothing runs there unless a tool is actually measuring.
 */
#include "qemu/timer.h"

#define FRAME_TIMELINE_RING 4096   /* ~68 s at 60 Hz: any single gesture fits */

typedef struct {
    int64_t  virt_ns;   /* QEMU_CLOCK_VIRTUAL at the latch */
    uint32_t seq;       /* monotonic frame index since reset */
    uint32_t key;       /* content id (swap id / scanout base); 0 when held */
    uint8_t  newframe;  /* 1 = new content latched this vsync, 0 = held */
} FrameEvent;

typedef struct {
    FrameEvent ev[FRAME_TIMELINE_RING];
    uint32_t   head;    /* next write slot */
    uint32_t   seq;     /* frames latched since reset */
    uint32_t   count;   /* entries ever written (for wrap detection) */
} FrameTimeline;

static inline void frame_timeline_reset(FrameTimeline *ft)
{
    ft->head = ft->seq = ft->count = 0;
}

static inline void frame_timeline_record(FrameTimeline *ft, uint32_t key,
                                         bool newframe)
{
    FrameEvent *e = &ft->ev[ft->head];
    e->virt_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    e->seq = ft->seq++;
    e->key = newframe ? key : 0;
    e->newframe = newframe ? 1 : 0;
    ft->head = (ft->head + 1) % FRAME_TIMELINE_RING;
    ft->count++;
}

/*
 * Oldest-to-newest text dump: one "seq virt_ns newframe key" line per entry.
 * Caller owns the returned string.
 */
static inline char *frame_timeline_dump(const FrameTimeline *ft)
{
    GString *s = g_string_sized_new(128 * 1024);
    uint32_t n = ft->count < FRAME_TIMELINE_RING ? ft->count : FRAME_TIMELINE_RING;
    uint32_t start = ft->count < FRAME_TIMELINE_RING ? 0 : ft->head;
    for (uint32_t i = 0; i < n; i++) {
        const FrameEvent *e = &ft->ev[(start + i) % FRAME_TIMELINE_RING];
        g_string_append_printf(s, "%u %" PRId64 " %u %u\n",
                               e->seq, e->virt_ns, e->newframe, e->key);
    }
    return g_string_free(s, FALSE);
}
#endif /* HW_ARM_FRAME_TIMELINE_H */
