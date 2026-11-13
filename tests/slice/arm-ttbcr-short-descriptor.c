/* On a core with LPAE, a TTBCR write flushes the TLB only where it can change the ASID: TTBCR.EAE set before or after
 * (the Long-descriptor format, whose A1 picks the ASID's TTBR). With the Short-descriptor format the ASID is
 * CONTEXTIDR's and N, PD0 and PD1 only steer table walks, so the write flushes nothing, as on a core without LPAE.
 *
 * The iPhone 5's iOS 10 runs the Short-descriptor format on its LPAE cores and writes TTBCR 1,100 to 1,400 times a
 * second per core: every write emptied the whole TLB, the kernel's global entries and the TB jump cache with it.
 *
 * Mutation (must fail this test): drop the EAE test from vmsa_ttbcr_write's flush condition.
 *
 * SLICE target/arm/internals.h define TTBCR_
 * SLICE target/arm/helper.c fn vmsa_ttbcr_write
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { ARM_FEATURE_V8, ARM_FEATURE_LPAE, ARM_FEATURE_EL3 };
typedef struct { uint64_t ttbcr; bool lpae; } CPUARMState;
typedef struct { CPUARMState env; } ARMCPU;
typedef struct { int unused; } ARMCPRegInfo;
typedef struct CPUState CPUState;

static int flushes;
static bool arm_feature(CPUARMState *env, int f) { return f == ARM_FEATURE_EL3 || (f == ARM_FEATURE_LPAE && env->lpae); }
static ARMCPU *env_archcpu(CPUARMState *env) { return (ARMCPU *)env; }
static CPUState *CPU(ARMCPU *cpu) { return (CPUState *)cpu; }
static void tlb_flush(CPUState *cs) { flushes++; }
static uint64_t raw_read(CPUARMState *env, const ARMCPRegInfo *ri) { return env->ttbcr; }
static void raw_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t v) { env->ttbcr = v; }

#include "slice.h"

/* Flushes for one TTBCR write of `to` over `from`, and that the write lands. */
static int write(bool lpae, uint32_t from, uint32_t to)
{
    CPUARMState env = { from, lpae };
    ARMCPRegInfo ri;

    flushes = 0;
    vmsa_ttbcr_write(&env, &ri, to);
    assert(env.ttbcr == to);
    return flushes;
}

int main(void)
{
    assert(write(true, 0, TTBCR_PD0 | 1) == 0);                    /* Short-descriptor: N and PD0 change, no ASID */
    assert(write(true, TTBCR_PD0 | 1, 0) == 0);
    assert(write(true, 0, TTBCR_EAE) == 1);                        /* into the Long-descriptor format */
    assert(write(true, TTBCR_EAE, 0) == 1);                        /* out of it */
    assert(write(true, TTBCR_EAE, TTBCR_EAE | TTBCR_A1) == 1);     /* A1 moves the ASID */
    assert(write(false, 0, TTBCR_PD1 | 2) == 0);                   /* no LPAE: never flushed */
    puts("PASS: TTBCR writes flush only with the Long-descriptor format");
}
