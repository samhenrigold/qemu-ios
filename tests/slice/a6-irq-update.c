/* The A6 AIC drives each CPU's IRQ line from per-target masks of its routed sources, and only when the level the CPU
 * holds changes.
 *
 * Random source, software, mask, destination, IPI and IOP-timer states must give every target (AP CPU 0 and 1, the
 * IOP) the level the per-source rule gives it (a source counts for a target when it is raised or set in software,
 * unmasked, and its destination has the target's bit); an update that changes nothing must not touch a line (every
 * raise is a cpu_interrupt that sends the CPU out of its TBs); and a CPU reset, which clears the CPU's
 * interrupt_request, must get a pending line back on the next update.
 *
 * Mutations: drop the level != held test in a6_irq_drive (the no-change check fails); compare with a level the AIC
 * remembers instead of the CPU's interrupt_request (the reset check fails); leave routed[] alone in a6_set_destination
 * (the random states disagree with the rule).
 *
 * SLICE:defs hw/arm/s5l8950.c define A6_IRQS
 * SLICE hw/arm/s5l8950.c fn a6_set_destination a6_irq_drive a6_irq_update
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CPU_INTERRUPT_HARD 0x0002
#define ARM_CPU_IRQ 0
typedef struct ARMCPU {
    int interrupt_request;
} ARMCPU;
#define CPU(x) (x)
#define DEVICE(x) (x)
#define qatomic_read(p) (*(p))
static int set_calls;
static ARMCPU *qdev_get_gpio_in(ARMCPU *cpu, int n)
{
    assert(n == ARM_CPU_IRQ);
    return cpu;
}
static void qemu_set_irq(ARMCPU *cpu, int level)
{
    set_calls++;
    cpu->interrupt_request = level ? cpu->interrupt_request | CPU_INTERRUPT_HARD
                                   : cpu->interrupt_request & ~CPU_INTERRUPT_HARD;
}
#include "defs.h"
typedef struct S5L8950State {
    ARMCPU *iop_cpu, *cpu[2];
    uint32_t iop_timer_status, iop_other_mask;
    uint32_t level[6], software[6], mask[6], destination[A6_IRQS];
    uint32_t routed[3][6];
    uint32_t ipi[2], ipi_mask[2];
} S5L8950State;
#include "slice.h"

/* The rule a6_irq_update replaced: every source, one at a time. */
static bool expected(S5L8950State *s, int target)
{
    bool pending = target == 2 ? (s->iop_timer_status & 1) && !(s->iop_other_mask & 2)
                               : (s->ipi[target] & ~s->ipi_mask[target]) != 0;
    for (int irq = 0; irq < A6_IRQS; irq++) {
        uint32_t bit = 1u << (irq & 31);
        pending |= ((s->level[irq / 32] | s->software[irq / 32]) & ~s->mask[irq / 32] & bit) &&
                   (s->destination[irq] & (1u << target));
    }
    return pending;
}

static bool held(ARMCPU *cpu)
{
    return cpu->interrupt_request & CPU_INTERRUPT_HARD;
}

int main(void)
{
    ARMCPU iop = {0}, ap0 = {0}, ap1 = {0};
    ARMCPU *targets[3] = {&ap0, &ap1, &iop};
    S5L8950State s = {.iop_cpu = &iop, .cpu = {&ap0, &ap1}};
    srand(1);
    for (int i = 0; i < A6_IRQS; i++) {
        a6_set_destination(&s, i, 1);
    }
    for (int round = 0; round < 20000; round++) {
        /* sparse states, so that each target is sometimes idle */
        for (int w = 0; w < 6; w++) {
            s.level[w] = rand() % 4 ? 0 : 1u << (rand() % 32);
            s.software[w] = rand() % 8 ? 0 : 1u << (rand() % 32);
            s.mask[w] = rand() % 2 ? ~0u : ~(s.level[w] | s.software[w]);
        }
        for (int k = 0; k < 8; k++) {
            a6_set_destination(&s, rand() % A6_IRQS, rand() % 8);
        }
        s.ipi[0] = rand() % 16 == 0;
        s.ipi[1] = rand() % 16 == 0;
        s.ipi_mask[0] = rand() % 2;
        s.ipi_mask[1] = rand() % 2;
        s.iop_timer_status = rand() % 8 == 0;
        s.iop_other_mask = rand() % 2 ? 2 : 0;
        a6_irq_update(&s);
        for (int t = 0; t < 3; t++) {
            if (held(targets[t]) != expected(&s, t)) {
                fprintf(stderr, "round %d: target %d holds %d, the rule gives %d\n", round, t,
                        held(targets[t]), expected(&s, t));
                return 1;
            }
        }
        /* nothing changed: no line is touched */
        set_calls = 0;
        a6_irq_update(&s);
        assert(set_calls == 0);
        /* a reset CPU gets its pending line back */
        if (held(&ap1)) {
            ap1.interrupt_request = 0;
            a6_irq_update(&s);
            assert(held(&ap1) && set_calls == 1);
        }
    }
    puts("ok");
    return 0;
}
