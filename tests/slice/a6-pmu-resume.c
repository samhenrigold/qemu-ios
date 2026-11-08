/* Guard the original N42 LLB handoff: never overwrite retained DRAM or
 * reset guest service tokens on wake; invalid markers must not resume.
 * SLICE hw/arm/s5l8950.c fn a6_resume a6_ap_cut a6_ap_power a6_machine_reset
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#define A6_RAM_BASE 0x80000000ULL
#define MEMTXATTRS_UNSPECIFIED 0
#define GTIMER_VIRT 0
#define GTIMER_PHYS 1
#define ARM_CPU_MODE_SVC 0x13
#define CPSR_I 0x80
#define CPSR_F 0x40
#define CPSRWriteRaw 0
#define QEMU_CLOCK_VIRTUAL 0
#define SHUTDOWN_CAUSE_GUEST_RESET 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CPU(p) (p)
#define le32_to_cpu(x) (x)
typedef struct {
    uint64_t cval;
} TimerReg;
typedef struct {
    uint32_t regs[16];
    struct {
        TimerReg c14_timer[2];
    } cp15;
    unsigned cpsr;
} Env;
typedef struct {
    Env env;
    unsigned id;
} ARMCPU;
typedef struct {
    bool armed;
    uint64_t deadline;
} QEMUTimer;
typedef struct {
    ARMCPU *cpu[2];
    bool button_down[4];
    void *gpio, *pmu;
    QEMUTimer *ap_off_timer;
    bool resume_boot;
    unsigned pkg_token, agent_token;
} S5L8950State;
typedef S5L8950State MachineState;
typedef unsigned ResetType;
#define S5L8950_MACHINE(p) (p)
static unsigned address_space_memory, resets, soc_resets, irq_levels[4],
    off_mask, reset_requests, device_resets;
static void qemu_devices_reset(ResetType type)
{
    device_resets++;
}
static unsigned cold_prepares;
static void a6_pmu_cancel_resume(void *p)
{
    cold_prepares++;
}
static uint32_t marker[2];
static bool suspend_marker;
static void address_space_read(void *a, uint64_t pa, unsigned attrs, void *out,
                               unsigned n)
{
    assert(pa == A6_RAM_BASE + 0x80 && n == 8);
    memcpy(out, marker, n);
}
static void address_space_set(void *a, uint64_t pa, unsigned v, unsigned n,
                              unsigned attrs)
{
    assert(pa == A6_RAM_BASE + 0x80 && n == 8 && !v);
    memset(marker, 0, n);
}
static bool a6_pmu_take_suspend(void *p)
{
    bool valid = suspend_marker;
    suspend_marker = false;
    return valid;
}
static void a6_reset_ap(S5L8950State *s)
{
    soc_resets++;
}
static void cpu_reset(ARMCPU *c)
{
    unsigned id = c->id;
    memset(c, 0, sizeof(*c));
    c->id = id;
    resets++;
}
static void cpsr_write(Env *e, unsigned v, unsigned mask, unsigned type)
{
    e->cpsr = v;
}
static void cpu_set_pc(ARMCPU *c, unsigned pc)
{
    c->env.regs[15] = pc;
}
static unsigned qdev_get_gpio_in(void *p, unsigned i)
{
    return i;
}
static void qemu_set_irq(unsigned i, unsigned v)
{
    assert(i < 4);
    irq_levels[i] = v;
}
static void a6_pmu_reassert(void *p)
{
}
static unsigned arm_cpu_mp_affinity(ARMCPU *c)
{
    return c->id;
}
static void arm_set_cpu_off(unsigned id)
{
    off_mask |= 1u << id;
}
static void timer_del(QEMUTimer *t)
{
    t->armed = false;
}
static void timer_mod(QEMUTimer *t, uint64_t n)
{
    t->armed = true;
    t->deadline = n;
}
static uint64_t qemu_clock_get_ns(unsigned clock)
{
    return 1234567;
}
static void qemu_system_reset_request(unsigned why)
{
    reset_requests++;
}
#include "slice.h"
int main(void)
{
    ARMCPU cores[2] = {{.id = 0}, {.id = 1}};
    QEMUTimer timer = {0};
    S5L8950State s = {.cpu = {&cores[0], &cores[1]},
                      .ap_off_timer = &timer,
                      .pkg_token = 123,
                      .agent_token = 456,
                      .button_down = {true, false, false, false}};
    marker[0] = 0x4d4f5358;
    marker[1] = 0;
    suspend_marker = true;
    assert(!a6_resume(&s) && !resets && !soc_resets && suspend_marker);
    marker[1] = 0x53555350;
    suspend_marker = false;
    assert(!a6_resume(&s) && !resets && !soc_resets && marker[0] == 0x4d4f5358);
    suspend_marker = true;
    assert(a6_resume(&s));
    assert(resets == 2 && soc_resets == 1 && !marker[0] && !marker[1] &&
           !suspend_marker);
    for (unsigned i = 0; i < 2; i++)
        assert(!cores[i].env.regs[0] && !cores[i].env.regs[15] &&
               cores[i].env.cpsr == 0xd3 &&
               cores[i].env.cp15.c14_timer[0].cval == UINT64_MAX);
    assert(s.pkg_token == 123 && s.agent_token == 456 && irq_levels[0] == 0 &&
           irq_levels[1] == 1);
    a6_ap_power(&s, 0, 0);
    assert(timer.armed && timer.deadline == 21234567 && !off_mask &&
           !reset_requests);
    a6_ap_cut(&s);
    assert(off_mask == 3);
    a6_ap_power(&s, 0, 1);
    assert(!timer.armed && s.resume_boot && reset_requests == 1);
    /* Machine-level wake must bypass qemu_devices_reset, preserving NAND
  * transactions and the independently powered firmware domains. */
    marker[0] = 0x4d4f5358;
    marker[1] = 0x53555350;
    suspend_marker = true;
    s.resume_boot = true;
    a6_machine_reset(&s, 0);
    assert(!device_resets && !s.resume_boot && soc_resets == 2 &&
           !cold_prepares);
    s.resume_boot = true;
    marker[0] = 0;
    a6_machine_reset(&s, 0);
    assert(device_resets == 1 && !s.resume_boot && cold_prepares == 1);
    a6_machine_reset(&s, 0);
    assert(device_resets == 2);
    puts(
        "PASS: N42 guarded retained-RAM trampoline, both AP cores, held buttons, service tokens and deferred rail cycle");
}
