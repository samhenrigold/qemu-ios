/* The VM clock waits for a vCPU the host isn't running: while a vCPU that isn't idle reaches no TB boundary between
 * two of the stall thread's looks, the clock runs at 1/16 of host time, and afterwards at 1.25 times until it has
 * caught up with host time.
 *
 * iOS 10's hw_lock_bit (14G61, 800da1ee) spins on a bit lock until the generic timer, which reads the VM clock, has
 * moved 0xc00000 ticks (0.52 s at 24 MHz) and then panics "hw_lock_bit(): timed out". The iPhone 5 did that on a
 * loaded Mac, where the holder's vCPU thread waits for the host (descheduled, or behind the BQL or a broadcast TLB
 * flush's exclusive section) while the clock runs on; the board's cores never stop. Here a holder stalled for a
 * second must cost the spinner well under the timeout; the clock must then catch up with host time exactly, within
 * four times what it owes, never faster than 1.25 times host time, and slow again for a stall during the catch-up; a
 * vCPU that runs a chained loop (which reaches a TB boundary only when kicked) or sleeps idle must not move the clock
 * off host rate; and the clock stays continuous and monotonic across the changes and a stop.
 *
 * Mutation (must fail this test): mttcg_stall_check counting a vCPU as stalled without !seen, or a stalled one as not;
 * dropping its kick; not skipping an idle vCPU; cpu_clock_at ignoring cpu_clock_slow_since or cpu_clock_catch_since,
 * or not capping the catch-up at cpu_clock_owed; cpu_clock_settle not adding the slowed time to what is owed, or not
 * subtracting the repaid time; cpu_clock_set_slow not catching up after a stall, or not slowing during a catch-up;
 * cpu_enable_ticks not resuming the catch-up; cpu_disable_ticks not settling.
 *
 * SLICE include/system/cpu-timers-internal.h define CPU_CLOCK_SLOW_SHIFT|CPU_CLOCK_CATCH_SHIFT
 * SLICE system/cpu-timers.c fn cpu_clock_at cpu_clock_settle cpu_get_clock_locked cpu_enable_ticks cpu_disable_ticks
 * SLICE system/cpu-timers.c fn cpu_clock_set_slow
 * SLICE accel/tcg/tcg-accel-ops-mttcg.c define STALL_TICK_US|STALL_IDLE_TICK_US
 * SLICE accel/tcg/tcg-accel-ops-mttcg.c fn mttcg_stall_check mttcg_stall_look
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-function
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
static int64_t host_ns;   /* get_clock(): the host's monotonic time */
static int64_t get_clock(void) { return host_ns; }
static int64_t cpu_get_host_ticks(void) { return host_ns; }
typedef struct {
    int64_t cpu_ticks_offset, cpu_clock_offset, cpu_clock_slow_since, cpu_clock_catch_since, cpu_clock_owed;
    int16_t cpu_ticks_enabled;
    int vm_clock_seqlock, vm_clock_lock;
} TimersState;
static TimersState timers_state;
static int writers, locked, notified;
static void seqlock_write_lock(int *seq, int *lock) { (void)seq; (void)lock; writers++; locked++; }
static void seqlock_write_unlock(int *seq, int *lock) { (void)seq; (void)lock; writers--; locked--; }
static void qemu_spin_lock(int *lock) { (void)lock; locked++; }
static void qemu_spin_unlock(int *lock) { (void)lock; locked--; }
static void seqlock_write_begin(int *seq) { (void)seq; assert(locked); writers++; }
static void seqlock_write_end(int *seq) { (void)seq; writers--; }
enum { QEMU_CLOCK_VIRTUAL };
static void qemu_clock_notify(int clock) { (void)clock; notified++; }

typedef struct CPUState {
    bool created, idle, exec_seen;
    struct { union { struct { int16_t high; } u16; } icount_decr; } neg;
} CPUState;
static CPUState cpus[2];
#define CPU_FOREACH(cpu) for ((cpu) = cpus; (cpu) < cpus + 2; (cpu)++)
#define WITH_RCU_READ_LOCK_GUARD()
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
#define qatomic_xchg(p, v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)

#include "slice.h"

enum { RUNS, STALLS, IDLE };          /* what a simulated vCPU does */
static int state[2];
static bool awake = true, off_rate;
static int64_t next_look, last_clock;
static const int64_t ms = 1000000, step = 50000;
#define slow (timers_state.cpu_clock_slow_since != 0)
#define catching (timers_state.cpu_clock_catch_since != 0)

/* host time to @until in 50 us steps: the vCPUs act, the stall thread looks on its schedule */
static void run(int64_t until)
{
    for (; host_ns < until; host_ns += step) {
        for (int i = 0; i < 2; i++) {
            cpus[i].idle = state[i] == IDLE;
            /* a chained loop reaches cpu_handle_interrupt only when kicked; a stalled vCPU not at all */
            if (state[i] == RUNS && cpus[i].neg.icount_decr.u16.high == -1) {
                cpus[i].neg.icount_decr.u16.high = 0;
                cpus[i].exec_seen = true;
            }
        }
        if (host_ns >= next_look) {
            off_rate = mttcg_stall_look(&awake);
            assert(off_rate == (slow || catching));
            next_look = host_ns + (awake || off_rate ? STALL_TICK_US : STALL_IDLE_TICK_US) * 1000;
        }
        int64_t now = cpu_get_clock_locked();
        /* monotonic, and never faster than 1.25 times host time */
        assert(now >= last_clock && (now - last_clock) * 4 <= step * 5 && writers == 0 && locked == 0);
        last_clock = now;
    }
}

int main(void)
{
    const int64_t timeout_ns = 0xc00000 * 41;   /* hw_lock_bit's, at the 41 ns tick QEMU gives 24 MHz */
    const int64_t tick = STALL_TICK_US * 1000;
    cpus[0].created = cpus[1].created = true;
    host_ns = 1000 * ms;
    cpu_enable_ticks();
    run(host_ns + tick);   /* the first look has kicked nobody yet */
    run(host_ns + 100 * ms);   /* nor caught up the tick it cost */

    /* both running, one in a loop that never leaves its TB chain unless kicked: the clock is host time */
    int64_t t0 = cpu_get_clock_locked(), h0 = host_ns;
    run(host_ns + 1000 * ms);
    assert(cpu_get_clock_locked() - t0 == host_ns - h0 && !off_rate);

    /* the holder (CPU 0) stalls a second while CPU 1 spins on its lock against the clock */
    int64_t ts = cpu_get_clock_locked();
    state[0] = STALLS;
    run(host_ns + 1000 * ms);
    int64_t spun = cpu_get_clock_locked() - ts;
    assert(slow && spun < 1000 * ms / 16 + 3 * tick);
    assert(spun < timeout_ns / 4);
    printf("a vCPU stalled 1 s: the spinner saw %.1f ms (timeout %.1f ms)\n", spun / 1e6, timeout_ns / 1e6);

    /* running again, the clock catches up with host time within four times what it owes, then keeps host rate */
    state[0] = RUNS;
    int64_t end = host_ns, owed = host_ns - h0 - (cpu_get_clock_locked() - t0);
    run(host_ns + tick + 100 * ms);
    assert(catching && cpu_get_clock_locked() - t0 < host_ns - h0);
    run(end + 4 * owed + 2 * tick);
    assert(!off_rate && cpu_get_clock_locked() - t0 == host_ns - h0);
    printf("it owed %.1f ms and caught up in %.0f ms\n", owed / 1e6, (host_ns - end) / 1e6);

    /* a stall during the catch-up slows the clock again, and the catch-up repays both */
    state[0] = STALLS;
    run(host_ns + 1000 * ms);
    state[0] = RUNS;
    run(host_ns + 500 * ms);
    assert(catching);
    int64_t t1 = cpu_get_clock_locked();
    state[1] = STALLS;
    run(host_ns + 500 * ms);
    assert(slow && cpu_get_clock_locked() - t1 < 500 * ms / 16 + 3 * tick);
    state[1] = RUNS;
    run(host_ns + 8000 * ms);
    assert(!off_rate && cpu_get_clock_locked() - t0 == host_ns - h0);

    /* a vCPU asleep in WFI is no stall */
    state[0] = IDLE;
    t0 = cpu_get_clock_locked(), h0 = host_ns;
    run(host_ns + 1000 * ms);
    assert(!off_rate && cpu_get_clock_locked() - t0 == host_ns - h0);

    /* both idle: the stall thread looks less often, and the clock is host time */
    state[1] = IDLE;
    run(host_ns + 100 * ms);
    assert(!awake && !off_rate);
    state[0] = state[1] = RUNS;
    run(host_ns + 100 * ms);
    assert(awake && !off_rate);

    /* stopped while slowed, the clock holds; started again, it catches up what slowing cost from where it held */
    t0 = cpu_get_clock_locked(), h0 = host_ns;
    state[1] = STALLS;
    run(host_ns + 100 * ms);
    assert(slow);
    cpu_disable_ticks();
    int64_t stopped = cpu_get_clock_locked();
    host_ns += 5000 * ms;
    assert(cpu_get_clock_locked() == stopped);
    cpu_enable_ticks();
    assert(cpu_get_clock_locked() == stopped);
    host_ns += 10 * ms;
    assert(cpu_get_clock_locked() == stopped + 10 * ms + 10 * ms / 4);
    state[1] = RUNS;
    last_clock = cpu_get_clock_locked();
    run(host_ns + 1000 * ms);
    assert(!off_rate && cpu_get_clock_locked() - t0 == host_ns - h0 - 5000 * ms);
    assert(notified > 0);
    puts("PASS: the VM clock slows while a vCPU stalls, and only then, and catches up after");
    return 0;
}
