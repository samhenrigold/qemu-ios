/* A tap released before the guest's next read still reaches it as TOUCH_START, then TOUCH_ENDED, then the end of the
 * gesture. The pending-frame slot holds one frame; a newer frame used to replace it unread, so a press and release
 * between two reads (a Mac trackpad's tap to click) arrived as a lone TOUCH_ENDED and iOS 6's status bar never
 * scrolled to the top. Pure motion frames still replace each other, and a report whose frame is kept back still raises
 * ATN (qtest ipod-touch-irq-test masked-atn: an edge acked while masked is followed by the next report's).
 *
 * Mutations (named, each must fail this test): drop mt_emit_frame's "pending frame starts or ends a contact" early
 * return; drop the inform_frame_ready call in it.
 *
 * SLICE:wire include/hw/arm/ipod_touch_multitouch.h range #define MT_INTERFACE_VERSION | typedef struct IPodTouchMultitouchState
 * SLICE hw/arm/ipod_touch_multitouch.c fn mt_frame_period_ns mt_any_down mt_any_active mt_emit_frame mt_cancel_pending_end
 * SLICE hw/arm/ipod_touch_multitouch.c fn ipod_touch_multitouch_set_finger touch_timer_tick touch_end_timer_tick
 * SLICE hw/arm/ipod_touch_multitouch.c fn ipod_touch_multitouch_frame_taken
 * CFLAGS -Wall -Werror -Wno-unused-function
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define g_free free
#define QEMU_CLOCK_VIRTUAL 0
#define NANOSECONDS_PER_SECOND 1000000000LL
#define MTT(...) ((void)0)
static int64_t now;
static int64_t qemu_clock_get_ns(int clock) { return now; }
typedef struct { int64_t at; } QEMUTimer;   /* -1: not armed */
static void timer_mod(QEMUTimer *t, int64_t at) { t->at = at; }
static void timer_del(QEMUTimer *t) { t->at = -1; }
#include "wire.h"
typedef struct {
    MTFrame *next_frame;
    uint32_t next_frame_len;
    bool next_frame_changes, frame_owed, touch_down;
    QEMUTimer *touch_timer, *touch_end_timer;
    MTFingerState fingers[MT_MAX_FINGERS];
    const MTSensorProfile *profile;
    float touch_x, touch_y, prev_touch_x, prev_touch_y;
    int64_t last_motion_ns;
    uint32_t report_hz;
    bool hold_atn;
    int atn;
} IPodTouchMultitouchState;
static void qemu_irq_raise(int irq) { (void)irq; }
static int atn;   /* ATN pulses: one per report, the frame new or kept */
static void ipod_touch_multitouch_inform_frame_ready(IPodTouchMultitouchState *s) { atn++; }
/* A frame is the finger events it carries: S start, M moved, E ended, F the gesture's end. */
static MTFrame *mt_build_frame(IPodTouchMultitouchState *s, MTFingerState *fingers, uint32_t *len)
{
    char *tag = calloc(1, sizeof(MTFrame));
    for (int i = 0; i < MT_MAX_FINGERS; i++) {
        MTFingerState *f = &fingers[i];
        if (f->phase != MT_FINGER_FREE) {
            tag[strlen(tag)] = f->phase == MT_FINGER_DOWN ? (f->started ? 'M' : 'S')
                             : f->phase == MT_FINGER_LIFTED ? 'E' : 'F';
        }
    }
    *len = sizeof(MTFrame);
    return (MTFrame *)tag;
}
#include "slice.h"

static IPodTouchMultitouchState s;
static QEMUTimer touch = {-1}, end = {-1};
static char got[64];

/* Time passes to `at`, firing the timers due; then the guest reads (if a frame is pending). */
static void guest_reads_at(int64_t at)
{
    now = at;
    if (end.at >= 0 && end.at <= now) { end.at = -1; touch_end_timer_tick(&s); }
    if (touch.at >= 0 && touch.at <= now) { touch.at = -1; touch_timer_tick(&s); }
    if (!s.next_frame) return;
    strcat(got, (char *)s.next_frame);
    strcat(got, " ");
    free(s.next_frame);
    s.next_frame = NULL;
    s.next_frame_len = 0;
    ipod_touch_multitouch_frame_taken(&s);
}

static void reset(void)
{
    memset(&s, 0, sizeof(s));
    s.touch_timer = &touch;
    s.touch_end_timer = &end;
    s.report_hz = 60;
    touch.at = end.at = -1;
    got[0] = 0;
    atn = 0;
    now = 1000000000;
}

int main(void)
{
    /* A quick tap: press and release before the guest reads anything. */
    reset();
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .98f, true);
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .98f, false);
    assert(!s.touch_down);                         /* the pointer path's next press is a press */
    guest_reads_at(now + 1000000);                 /* the start */
    guest_reads_at(now + 1000000);                 /* nothing yet: the owed end waits a report period */
    guest_reads_at(now + 20000000);                /* the end */
    guest_reads_at(now + 200000000);               /* the gesture's end */
    guest_reads_at(now + 200000000);
    printf("quick tap: %s\n", got);
    assert(!strcmp(got, "S E F "));

    /* A held tap, read between: as before. */
    reset();
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, true);
    guest_reads_at(now + 1000000);
    now += 80000000;
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, false);
    guest_reads_at(now + 1000000);
    guest_reads_at(now + 200000000);
    printf("held tap: %s\n", got);
    assert(!strcmp(got, "S E F "));

    /* Motion the guest has not read yet is replaced by newer motion, not queued. */
    reset();
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, true);
    guest_reads_at(now + 1000000);
    now += 20000000;
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .4f, true);
    now += 20000000;
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .3f, true);
    guest_reads_at(now + 1000000);
    printf("drag: %s\n", got);
    assert(!strcmp(got, "S M "));
    assert(s.fingers[0].y == .3f);
    /* A guest that misses the end's ATN still gets another: the gesture-end report, kept back, raises it again. */
    reset();
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, true);
    guest_reads_at(now + 1000000);
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, false);
    int before = atn;
    now += 200000000;
    guest_reads_at(now);                           /* the end, read only after the 100 ms gesture-end report */
    assert(atn == before + 1);                     /* the kept-back gesture-end report pulsed it again */
    guest_reads_at(now + 200000000);
    printf("missed ATN: %s\n", got);
    assert(!strcmp(got, "S E F "));

    /* A held finger whose start the guest has not read (ATN acked while masked) still raises ATN every report. */
    reset();
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, true);
    before = atn;
    now += 20000000;
    touch.at = -1;
    touch_timer_tick(&s);
    assert(atn == before + 1);
    reset();
    ipod_touch_multitouch_set_finger(&s,0,0,.5f,true);assert(s.fingers[0].contact_flags==1);
    now+=20000000;ipod_touch_multitouch_set_finger(&s,0,.5f,.5f,true);assert(s.fingers[0].contact_flags==1);
    ipod_touch_multitouch_set_finger(&s,0,.5f,.5f,false);assert(!s.fingers[0].contact_flags);
    reset();ipod_touch_multitouch_set_finger(&s,0,.5f,.5f,true);assert(!s.fingers[0].contact_flags);
    reset();ipod_touch_multitouch_set_finger(&s,0,1,1,true);assert(s.fingers[0].contact_flags==4);
    puts("PASS: a tap released before the guest's read arrives whole (start, end, gesture end); motion still coalesces");
}
