/* A6 PMGR performance-state transitions: a request at 0x1700 has finished
 * by the next read of the finish times at 0x1704-0x1728, which then hold
 * still until the next request, at or after the time the driver took just
 * after requesting (14G61 0x805a069c), so it stops polling.
 * SLICE hw/arm/s5l8950.c fn a6_counter_period_ns a6_pmgr_read a6_pmgr_write
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define QEMU_CLOCK_VIRTUAL 0
typedef struct {
    void *cpu[2];
    uint32_t pmgr[0x7000 / 4];
    uint64_t pmgr_transition_done;
    bool pmgr_transition_pending;
} S5L8950State;
static int64_t now;
static int64_t qemu_clock_get_ns(unsigned clock)
{
    return now;
}
static unsigned gt_cntfrq_period_ns(void *cpu)
{
    return 41;
}
#include "slice.h"
static uint64_t finish(S5L8950State *s, uint32_t high)
{
    return (uint64_t)a6_pmgr_read(s, high) << 32 | a6_pmgr_read(s, high + 4);
}
int main(void)
{
    static S5L8950State s;
    static const uint32_t times[] = {0x1704, 0x170c, 0x171c, 0x1724};
    for (int i = 0; i < 4; i++) {
        assert(finish(&s, times[i]) == 0);
    }
    now = 200000000000LL; /* past 2^32 ticks, so the high word is used */
    a6_pmgr_write(&s, 0x1700, 0x23333bbb);
    uint64_t asked = now / 41; /* the driver's mach_absolute_time after the write */
    now += 3000;
    uint64_t done = finish(&s, 0x1724);
    assert(done >= asked && done == (uint64_t)now / 41 && done >> 32);
    now += 1000000;
    for (int i = 0; i < 4; i++) {
        assert(finish(&s, times[i]) == done);
    }
    assert(a6_pmgr_read(&s, 0x1700) == 0x23333bbb);
    a6_pmgr_write(&s, 0x1700, 0x23222fff);
    asked = now / 41;
    assert(finish(&s, 0x1704) >= asked && finish(&s, 0x171c) > done);
    a6_pmgr_write(&s, 0x1714, 0x5a);
    assert(a6_pmgr_read(&s, 0x1714) == 0x5a);
    a6_pmgr_write(&s, 0x10c8, 0xf);
    assert(a6_pmgr_read(&s, 0x10c8) == 0xff);
    a6_pmgr_write(&s, 0x1204, 0x44);
    assert(a6_pmgr_read(&s, 0x1204) == 0x04);
    puts("PASS: A6 PMGR transition finish times move only after a request, at or after it");
}
