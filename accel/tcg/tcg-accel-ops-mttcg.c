/*
 * QEMU TCG Multi Threaded vCPUs implementation
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "system/tcg.h"
#include "system/replay.h"
#include "system/cpu-timers.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "hw/boards.h"
#include "tcg/startup.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"

#ifdef CONFIG_DARWIN
#include <pthread/qos.h>
#endif

typedef struct MttcgForceRcuNotifier {
    Notifier notifier;
    CPUState *cpu;
} MttcgForceRcuNotifier;

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

static void mttcg_force_rcu(Notifier *notify, void *data)
{
    CPUState *cpu = container_of(notify, MttcgForceRcuNotifier, notifier)->cpu;

    /*
     * Called with rcu_registry_lock held, using async_run_on_cpu() ensures
     * that there are no deadlocks.
     */
    async_run_on_cpu(cpu, do_nothing, RUN_ON_CPU_NULL);
}

/*
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

/*
 * Ask for a performance core.
 *
 * This is the thread that IS the emulated CPU -- everything the guest does
 * happens here -- but qemu_thread_create() sets no QoS class, so on Apple
 * silicon it is eligible for an efficiency core, at roughly half the
 * throughput. The host app setting its own QoS does not help: that applies to
 * the thread calling qemu_init(), which becomes the iothread, not this one.
 *
 * USER_INTERACTIVE rather than USER_INITIATED because this thread is literally
 * driving what is on screen.
 */
static void mttcg_request_performance_core(void)
{
#ifdef CONFIG_DARWIN
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relpri = 0;

    pthread_get_qos_class_np(pthread_self(), &qos, &relpri);
    if (qos != QOS_CLASS_USER_INTERACTIVE) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
    if (getenv("IT_QOS_TRACE")) {
        qos_class_t now = QOS_CLASS_UNSPECIFIED;
        pthread_get_qos_class_np(pthread_self(), &now, &relpri);
        fprintf(stderr, "[qos] vcpu thread: was %d, now %d "
                "(USER_INTERACTIVE=%d)\n",
                (int)qos, (int)now, (int)QOS_CLASS_USER_INTERACTIVE);
    }
#endif
}

/*
 * IT_VCPU_ACCT: split this thread's wall time three ways.
 *
 * "run" is time inside the engine with the guest executing, "halt" is time the
 * guest spent halted (WFI) waiting for an interrupt, and "io" is everything
 * else -- taking the BQL back, servicing run_on_cpu work, the main loop. If a
 * slow emulator is not spending its time in "run", the engine is not the
 * bottleneck and optimising it is wasted effort.
 *
 * cpu_exec() returns EXCP_HALTED without executing anything when the guest is
 * halted, which is what lets the two be told apart from out here.
 */
extern bool it_acct_on;
extern uint64_t it_acct_insns, it_acct_blocks, it_acct_irqs, it_acct_exits;

static int64_t acct_run_ns, acct_halt_ns, acct_io_ns, acct_bql_ns;
static int64_t acct_io_cpu_ns, acct_io_cpu_mark;
static int64_t acct_t0, acct_next_dump, acct_interval_ns, acct_io_mark;
static uint64_t acct_halts;
static uint64_t acct_why_halted, acct_why_stopped, acct_why_other, acct_long;
static bool acct_halted;

/*
 * Wall time in the io window says the vCPU is not executing; it does NOT say
 * whether the thread is asleep or busy. Those need opposite fixes -- asleep
 * means something is failing to wake it, busy means queued vCPU work (TLB
 * flushes arrive as async_run_on_cpu items and run right here) is eating the
 * budget. The thread CPU clock tells them apart.
 */
static int64_t thread_cpu_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) {
        return 0;
    }
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void mttcg_acct_init(void)
{
    const char *v = getenv("IT_VCPU_ACCT");

    if (!v) {
        return;
    }
    it_acct_on = true;
    acct_interval_ns = (int64_t)(atof(v) > 0 ? atof(v) : 5.0) * 1000000000LL;
    acct_t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    acct_next_dump = acct_t0 + acct_interval_ns;
}

static void mttcg_acct_dump(int64_t now)
{
    double wall = (now - acct_t0) / 1e9;

    if (wall <= 0) {
        return;
    }
    fprintf(stderr, "[acct] %6.1fs  run %5.1f%%  halt %5.1f%%  io %5.1f%% "
            "(bql %4.1f%% cpu %4.1f%%)  |  "
            "%.2f Minsn/s  %.0f blk/s (%.1f insn/blk)  %.0f irq/s  "
            "%.0f halt/s  %.0f exit/s  |  long %lu (halted %lu stopped %lu other %lu)\n",
            wall,
            100.0 * acct_run_ns / (now - acct_t0),
            100.0 * acct_halt_ns / (now - acct_t0),
            100.0 * acct_io_ns / (now - acct_t0),
            100.0 * acct_bql_ns / (now - acct_t0),
            100.0 * acct_io_cpu_ns / (now - acct_t0),
            it_acct_insns / wall / 1e6,
            it_acct_blocks / wall,
            it_acct_blocks ? (double)it_acct_insns / it_acct_blocks : 0.0,
            it_acct_irqs / wall,
            acct_halts / wall,
            it_acct_exits / wall,
            (unsigned long)acct_long, (unsigned long)acct_why_halted,
            (unsigned long)acct_why_stopped, (unsigned long)acct_why_other);
}

static void *mttcg_cpu_thread_fn(void *arg)
{
    MttcgForceRcuNotifier force_rcu;
    CPUState *cpu = arg;

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    mttcg_request_performance_core();
    mttcg_acct_init();
    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->neg.can_do_io = true;
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    /* process any pending work */
    cpu->exit_request = 1;

    do {
        if (cpu_can_run(cpu)) {
            int r;
            int64_t a = 0, b;
            bql_unlock();
            if (it_acct_on) {
                a = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            }
            r = tcg_cpu_exec(cpu);
            if (it_acct_on) {
                b = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                /*
                 * cpu_exec() RETURNS on a halt, cheaply -- the actual sleep is
                 * inside the qemu_wait_io_event() below, so the wait has to be
                 * attributed by what the preceding exec returned, or halt time
                 * hides inside "io" and the split says nothing.
                 */
                acct_run_ns += b - a;
                acct_halted = (r == EXCP_HALTED);
                acct_halts += acct_halted;
                acct_io_mark = b;   /* closed after qemu_wait_io_event */
                acct_io_cpu_mark = thread_cpu_ns();
            }
            bql_lock();
            /*
             * "io" is two very different things -- waiting for the BQL (the
             * iothread is holding it, e.g. across an LCD conversion) and
             * waiting in the main loop itself. Only the first is contention,
             * and only the first is fixable here, so time it separately.
             */
            if (it_acct_on) {
                acct_bql_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - b;
            }
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /*
                 * Usually cpu->halted is set, but may have already been
                 * reset by another thread by the time we arrive here.
                 */
                break;
            case EXCP_ATOMIC:
                bql_unlock();
                cpu_exec_step_atomic(cpu);
                bql_lock();
            default:
                /* Ignore everything else? */
                break;
            }
        }

        qatomic_set_mb(&cpu->exit_request, 0);
        qemu_wait_io_event(cpu);

        if (it_acct_on) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            if (acct_io_mark) {
                if (acct_halted) {
                    acct_halt_ns += now - acct_io_mark;
                } else {
                    acct_io_ns += now - acct_io_mark;
                    acct_io_cpu_ns += thread_cpu_ns() - acct_io_cpu_mark;
                    /*
                     * Only long waits matter, and the reason has to be sampled
                     * right after waking or it has already been cleared.
                     */
                    if (now - acct_io_mark > 2000000) {
                        acct_long++;
                        if (cpu->halted) {
                            acct_why_halted++;
                        } else if (!runstate_is_running()) {
                            acct_why_stopped++;
                        } else {
                            acct_why_other++;
                        }
                    }
                }
                acct_io_mark = 0;
            }
            if (now >= acct_next_dump) {
                acct_next_dump = now + acct_interval_ns;
                mttcg_acct_dump(now);
            }
        }
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpu_destroy(cpu);
    bql_unlock();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
    rcu_unregister_thread();
    return NULL;
}

void mttcg_kick_vcpu_thread(CPUState *cpu)
{
    cpu_exit(cpu);
}

void mttcg_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1);

    /* create a thread per vCPU with TCG (MTTCG) */
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, mttcg_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}
