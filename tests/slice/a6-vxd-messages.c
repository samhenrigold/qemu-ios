/* Initialization has no decode fence: only submitted pictures complete.
 * SLICE hw/arm/s5l8920_vxd.c define VXD_BS_SEGS
 * SLICE hw/arm/s5l8920_vxd.c typedef VXDRender
 * SLICE hw/arm/s5l8920_vxd.c define MSG_|RENDER_|VXD_SETUP_NS|VXD_MB_NS|VXD_PENDING
 * SLICE hw/arm/s5l8920_vxd.c fn vxd_message
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#define QEMU_CLOCK_VIRTUAL 0
#define VXD_LOG(...) ((void)0)
typedef struct {
    FILE *trace;
    unsigned pending;
    uint32_t fence[16];
    bool compact[16];
    int64_t due[16], busy_until;
    void *done;
    bool a6;
} S5L8920VXDState;
static bool armed;
static int64_t deadline;
static int64_t qemu_clock_get_ns(int clock)
{
    return 1000;
}
static bool timer_pending(void *timer)
{
    return armed;
}
static void timer_mod(void *timer, int64_t when)
{
    armed = true;
    deadline = when;
}
static void vxd_complete(void *opaque)
{
    assert(false);
}
static bool vxd_rw(uint32_t ptd, uint32_t addr, void *out, unsigned n,
                   bool write)
{
    return false;
}
/* The parser and codec have separate tests; here exercise message ownership. */
static bool vxd_parse_render(uint32_t ptd, uint32_t addr, void *out, bool a6)
{
    return false;
}
static bool vxd_parse_packed(uint32_t ptd, uint32_t addr, unsigned n, void *out)
{
    return false;
}
static int vxd_decode(S5L8920VXDState *s, uint32_t ptd, void *render)
{
    return 0;
}
#include "slice.h"
int main(void)
{
    uint32_t init[] = {0x8014, 0x1000, 0x101000, 0x400100, 0x12340000};
    /* AppleVXD375's boards ("a6" off) complete every message as before, INIT included (fence word 4) */
    S5L8920VXDState old = {0};
    vxd_message(&old, init, G_N_ELEMENTS(init));
    assert(old.pending == 1 && old.fence[0] == 0x12340000 && !old.compact[0]);
    armed = false;
    S5L8920VXDState s = {.a6 = true};
    vxd_message(&s, init, G_N_ELEMENTS(init));
    assert(!s.pending && !armed && !s.busy_until);
    uint32_t legacy_init[8] = {0x8020};
    vxd_message(&s, legacy_init, G_N_ELEMENTS(legacy_init));
    assert(!s.pending && !armed);
    uint32_t picture[] = {0x18114, 0x844015, 0x470000, 0x1234000, 0x10211000};
    vxd_message(&s, picture, G_N_ELEMENTS(picture));
    assert(s.pending == 1 && s.fence[0] == 1 && s.compact[0]);
    assert(armed && deadline == s.due[0] && deadline > 1000);
    /* Ring-wrap padding owns neither a fence nor a legacy-format reply. */
    int64_t due = s.due[0], busy_until = s.busy_until;
    uint32_t padding[] = {0xddde0004};
    vxd_message(&s, padding, G_N_ELEMENTS(padding));
    assert(s.pending == 1 && s.fence[0] == 1 && s.compact[0]);
    assert(s.due[0] == due && s.busy_until == busy_until && deadline == due);
    /* A nonfinal slice explicitly waives completion. */
    picture[0] = 0x28114;
    picture[1] = 0x840815;
    vxd_message(&s, picture, G_N_ELEMENTS(picture));
    assert(s.pending == 1 && s.fence[0] == 1);
    puts("ok");
}
