/* Mixed old/new completions retain their fence and their own wire layout.
 * SLICE hw/arm/s5l8920_vxd.c fn vxd_complete
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define QEMU_CLOCK_VIRTUAL 0
#define MSG_CMD_COMPLETED 0xc0
typedef struct S5L8920VXDState {
    unsigned pending;
    uint32_t fence[16];
    bool compact[16];
    int64_t due[16];
    void *done;
} S5L8920VXDState;
static int64_t now, armed;
static uint32_t reply[16][5];
static unsigned length[16], posted;
static int64_t qemu_clock_get_ns(int clock)
{
    (void)clock;
    return now;
}
static void timer_mod(void *timer, int64_t due)
{
    (void)timer;
    armed = due;
}
static void vxd_post(S5L8920VXDState *s, const uint32_t *msg, unsigned words)
{
    (void)s;
    assert(words <= 5 && posted < 16);
    memcpy(reply[posted], msg, words * 4);
    length[posted++] = words;
}
#include "slice.h"
int main(void)
{
    S5L8920VXDState s = {.pending = 3,
                         .fence = {0x12345678, 0xabcd, 0x32},
                         .compact = {false, true, false},
                         .due = {10, 20, 30}};
    now = 10;
    vxd_complete(&s);
    assert(posted == 1 && length[0] == 3);
    assert(reply[0][0] == 0xc00c && reply[0][1] == 0x12345678 &&
           reply[0][2] == 0);
    assert(s.pending == 2 && s.compact[0] && s.fence[0] == 0xabcd &&
           armed == 20);
    now = 20;
    vxd_complete(&s);
    assert(posted == 2 && length[1] == 5 && reply[1][0] == 0xabcdc014);
    for (unsigned i = 1; i < 5; i++)
        assert(reply[1][i] == 0);
    assert(s.pending == 1 && !s.compact[0] && s.fence[0] == 0x32 &&
           armed == 30);
    now = 30;
    vxd_complete(&s);
    assert(s.pending == 0 && posted == 3 && length[2] == 3 &&
           reply[2][1] == 0x32);
    puts("ok: legacy and compact completion queues");
}
