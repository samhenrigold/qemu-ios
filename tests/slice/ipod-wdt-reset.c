/* Normal watchdog kicks must not be mistaken for immediate resets.
 *
 * SLICE include/hw/arm/ipod_touch_wdt.h define WDT_
 * SLICE hw/arm/ipod_touch_wdt.c fn ipod_touch_wdt_write
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int clock) { return 0; }
typedef struct { uint32_t ctrl, cnt; bool noreset; } IPodTouchWDTState;
typedef struct { struct { uint32_t regs[16]; } env; } ARMCPU;
#define ARM_CPU(p) ((ARMCPU *)(p))
#define SHUTDOWN_CAUSE_GUEST_RESET 1
static void *current_cpu;
static int resets;
static bool wdt_trace(void) { return false; }
static void qemu_system_reset_request(int cause) {
    assert(cause == SHUTDOWN_CAUSE_GUEST_RESET);
    resets++;
}
#include "slice.h"
int main(void) {
    IPodTouchWDTState s = {0};
    ipod_touch_wdt_write(&s, WDT_CTRL, 0x001f4a00, 4);
    assert(s.ctrl == 0x001f4a00 && !resets);
    ipod_touch_wdt_write(&s, WDT_CTRL, 0, 4);
    assert(!s.ctrl && !resets);
    ipod_touch_wdt_write(&s, WDT_CNT, 0x100000, 4);
    assert(s.cnt == 0x100000 && !resets);
    ipod_touch_wdt_write(&s, WDT_CTRL, 0x100000, 4);
    assert(resets == 1);
    s.noreset = true;
    ipod_touch_wdt_write(&s, WDT_CTRL, 0x100000, 4);
    assert(resets == 1);
    puts("Watchdog reset command checks passed");
}
