/* AE2 interrupt routing used by the original N42 audio firmware.
 *
 * A masked AP doorbell must survive until its IRQ/FIQ bank is enabled.
 * Clearing one source must preserve other pending sources. Disabling a
 * bank suppresses its output without discarding the event.
 *
 * Mutation: replace pending & enable with pending | enable in irq_update;
 * the masked-doorbell assertions below must fail.
 *
 * SLICE hw/arm/s5l8950.c fn a6_ae2_irq_update a6_ae2_irq_read a6_ae2_irq_write
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t hwaddr;
typedef struct ARMCPU {
    int unused;
} ARMCPU;
typedef struct S5L8950State {
    ARMCPU *ae2_cpu;
    uint32_t ae2_pending, ae2_irq_enable, ae2_fiq_enable;
    unsigned ae2_accesses;
} S5L8950State;
#define qemu_log_mask(...) ((void)0)
#define DEVICE(x) (x)
#define ARM_CPU_IRQ 0
#define ARM_CPU_FIQ 1
static bool levels[2];
static int qdev_get_gpio_in(ARMCPU *cpu, int n)
{
    assert(cpu);
    return n;
}
static void qemu_set_irq(int n, int level)
{
    assert(n < 2);
    levels[n] = level;
}
#include "slice.h"

int main(void)
{
    ARMCPU cpu = {0};
    S5L8950State s = {.ae2_cpu = &cpu};
    /* Original message doorbells are vectors 4 and 5. */
    a6_ae2_irq_write(&s, 0x1020, 0x30, 4);
    assert(!levels[0] && !levels[1]);
    assert(a6_ae2_irq_read(&s, 0x1000, 4) == 0x30);
    a6_ae2_irq_write(&s, 0x1008, 0x10, 4);
    assert(levels[0] && !levels[1]);
    assert(a6_ae2_irq_read(&s, 0x1004, 4) == 0x10);
    a6_ae2_irq_write(&s, 0x1018, 0x20, 4);
    assert(levels[0] && levels[1]);
    assert(a6_ae2_irq_read(&s, 0x1014, 4) == 0x20);
    a6_ae2_irq_write(&s, 0x1024, 0x10, 4);
    assert(!levels[0] && levels[1]);
    assert(a6_ae2_irq_read(&s, 0x1010, 4) == 0x20);
    a6_ae2_irq_write(&s, 0x101c, 0x20, 4);
    assert(!levels[0] && !levels[1]);
    assert(a6_ae2_irq_read(&s, 0x1010, 4) == 0x20);
    a6_ae2_irq_write(&s, 0x1008, 0x20, 4);
    assert(levels[0] && !levels[1]);
    a6_ae2_irq_write(&s, 0x1024, 0x20, 4);
    assert(!levels[0] && !levels[1] && !s.ae2_pending);
    a6_ae2_irq_write(&s, 0x100c, 0x30, 4);
    assert(a6_ae2_irq_read(&s, 0x1004, 4) == 0);
    puts(
        "PASS: AE2 masked doorbells, independent IRQ/FIQ banks, W1S/W1C and source preservation");
}
