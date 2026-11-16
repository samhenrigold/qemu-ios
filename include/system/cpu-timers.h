/*
 * CPU timers state API
 *
 * Copyright 2020 SUSE LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#ifndef SYSTEM_CPU_TIMERS_H
#define SYSTEM_CPU_TIMERS_H

#include "qemu/timer.h"

/* init the whole cpu timers API, including icount, ticks, and cpu_throttle */
void cpu_timers_init(void);

/*
 * CPU Ticks and Clock
 */

/* Caller must hold BQL */
void cpu_enable_ticks(void);
/* Caller must hold BQL */
void cpu_disable_ticks(void);

/*
 * return the time elapsed in VM between vm_start and vm_stop.
 * cpu_get_ticks() uses units of the host CPU cycle counter.
 */
int64_t cpu_get_ticks(void);

/*
 * Returns the monotonic time elapsed in VM, i.e.,
 * the time between vm_start and vm_stop
 */
int64_t cpu_get_clock(void);

/*
 * While @slow, the VM clock runs at 1/16 of host time (CPU_CLOCK_SLOW_SHIFT), from what it reads at the call: the
 * guest's time for a vCPU the host doesn't run (tcg-accel-ops-mttcg.c). Called with @slow false, it then runs at 1.25
 * times host time (CPU_CLOCK_CATCH_SHIFT) until it has repaid what slowing cost it, then at host rate; each call
 * notices the repayment. Continuous and monotonic throughout; what is owed at a stop is repaid after the start, and is
 * not in the snapshot. Nothing while ticks are disabled. Any thread. Returns whether the clock is off host rate.
 */
bool cpu_clock_set_slow(bool slow);

void qemu_timer_notify_cb(void *opaque, QEMUClockType type);

/* get/set VIRTUAL clock and VM elapsed ticks via the cpus accel interface */
int64_t cpus_get_virtual_clock(void);
void cpus_set_virtual_clock(int64_t new_time);
int64_t cpus_get_elapsed_ticks(void);

#endif /* SYSTEM_CPU_TIMERS_H */
