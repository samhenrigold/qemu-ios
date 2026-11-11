/* A deferred GPIO handler must still observe active-low attention before it
 * collects the pending touch frame. The old zero-width pulse loses that level.
 * Legacy pulse and SYSIC boards retain their existing signaling contract.
 *
 * SLICE hw/arm/ipod_touch_multitouch.c fn ipod_touch_multitouch_inform_frame_ready ipod_touch_multitouch_frame_taken
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define MTT(...) ((void)0)
#define QEMU_CLOCK_VIRTUAL 0
typedef struct {
    int level, falling;
} Pin;
typedef struct {
    int64_t deadline;
} QEMUTimer;
typedef struct {
    void *sysic;
    unsigned sysic_atn_group, sysic_atn_bit;
    Pin *atn;
    bool hold_atn, next_frame_changes, frame_owed;
    QEMUTimer *touch_end_timer;
} IPodTouchMultitouchState;
static void qemu_irq_raise(Pin *p)
{
    p->level = 1;
}
static void qemu_irq_lower(Pin *p)
{
    p->falling += p->level != 0;
    p->level = 0;
}
static int64_t qemu_clock_get_ns(int clock)
{
    (void)clock;
    return 100;
}
static int64_t mt_frame_period_ns(IPodTouchMultitouchState *s)
{
    (void)s;
    return 10;
}
static void timer_mod(QEMUTimer *t, int64_t at)
{
    t->deadline = at;
}
static unsigned sysic_edges, sysic_group, sysic_bit;
static void ipod_touch_sysic_request_edge(void *sysic, unsigned group,
                                          unsigned bit)
{
    (void)sysic;
    sysic_edges++;
    sysic_group = group;
    sysic_bit = bit;
}
#include "slice.h"
int main(void)
{
    Pin pin = {.level = 1};
    QEMUTimer timer = {0};
    IPodTouchMultitouchState s = {
        .atn = &pin, .hold_atn = true, .touch_end_timer = &timer};
    ipod_touch_multitouch_inform_frame_ready(&s);
    /* The workloop samples after the interrupt, not in the edge callback. */
    assert(pin.level == 0 && pin.falling == 1);
    ipod_touch_multitouch_inform_frame_ready(&s);
    assert(pin.level == 0 && pin.falling == 2);
    s.frame_owed = true;
    s.next_frame_changes = true;
    ipod_touch_multitouch_frame_taken(&s);
    assert(pin.level == 1 && !s.frame_owed && !s.next_frame_changes);
    assert(timer.deadline == 110);
    s.hold_atn = false;
    ipod_touch_multitouch_inform_frame_ready(&s);
    assert(pin.level == 1 && pin.falling == 3);
    s.sysic = &s;
    ipod_touch_multitouch_inform_frame_ready(&s);
    assert(sysic_edges == 1 && sysic_group == 3 && sysic_bit == 13);
    puts(
        "touch attention: deferred GPIO, pickup, legacy pulse and SYSIC passed");
}
