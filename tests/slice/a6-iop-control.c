/* Native AP stop/reload/start must not execute the old IOP image.
 * SLICE hw/arm/s5l8950.c fn a6_iop_write
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#define A6_RAM_BASE 0x80000000ULL
#define A6_RAM_SIZE 0x40000000ULL
#define LOG_UNIMP 0
#define PSCI_OFF 0
#define PSCI_ON 1
#define NOT_HALTED 0
#define HALT_PSCI 3
#define CPU(x) (x)
#define OBJECT(x) (x)
#define qemu_log_mask(...) ((void)0)
typedef uint64_t hwaddr;
typedef struct {
    int unused;
} MemoryRegion;
typedef struct {
    unsigned halted, power_state;
    struct {
        unsigned halt_reason;
    } env;
} CPUState;
typedef struct {
    uint32_t iop_regs[1024];
    CPUState *iop_cpu;
    MemoryRegion iop_mem, iop_fw, ram;
    bool iop_fw_mapped;
} S5L8950State;
static CPUState *current_cpu;
static unsigned kicks, resets, aliases, removals, errors;
static uint64_t alias_offset;
static void qemu_cpu_kick(CPUState *c)
{
    kicks++;
}
static void cpu_reset(CPUState *c)
{
    resets++;
    c->halted = 1;
}
static void error_report(const char *s)
{
    errors++;
}
static void object_unparent(void *p)
{
}
static void memory_region_del_subregion(MemoryRegion *a, MemoryRegion *b)
{
    removals++;
}
static void memory_region_init_alias(MemoryRegion *a, void *b, const char *name,
                                     MemoryRegion *ram, uint64_t off,
                                     uint64_t size)
{
    assert(size == 0x100000);
    aliases++;
    alias_offset = off;
}
static void memory_region_add_subregion_overlap(MemoryRegion *a, uint64_t off,
                                                MemoryRegion *b, int priority)
{
    assert(!off && priority == 1);
}
#include "slice.h"
int main(void)
{
    CPUState ap = {0}, iop = {.power_state = PSCI_ON};
    S5L8950State s = {.iop_cpu = &iop};
    current_cpu = &ap;
    a6_iop_write(&s, 0, 0, 4);
    assert(iop.power_state == PSCI_OFF && iop.halted &&
           iop.env.halt_reason == HALT_PSCI && kicks == 1 && !resets);
    a6_iop_write(&s, 4, 0x80b88000, 4);
    assert(iop.power_state == PSCI_OFF && !resets);
    a6_iop_write(&s, 0, 1, 4);
    assert(iop.power_state == PSCI_ON && !iop.halted && resets == 1 &&
           aliases == 1 && alias_offset == 0xb88000);
    a6_iop_write(&s, 0, 0, 4);
    a6_iop_write(&s, 0, 1, 4);
    assert(resets == 2 && removals == 1 && aliases == 2);
    current_cpu = &iop;
    a6_iop_write(&s, 0, 0, 4);
    assert(iop.power_state == PSCI_ON && !iop.halted && resets == 2);
    current_cpu = &ap;
    a6_iop_write(&s, 4, 0x1000, 4);
    a6_iop_write(&s, 0, 1, 4);
    assert(errors == 1 && resets == 2 && aliases == 2);
    puts(
        "PASS: AP stop holds IOP across firmware reload; start remaps and resets; core writes and invalid bases cannot restart it");
}
