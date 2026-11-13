/* Reading the generic timer's counters (CNTPCT, CNTVCT and their AArch64 forms) takes no BQL; other ARM_CP_IO reads
 * still do.
 *
 * The counter is the virtual clock less this CPU's offsets, safe to read from any thread, as s5l8930_pmgr's timebase
 * already is. iOS 10 reads it for mach_absolute_time and its time accounting, over 40,000 times a second per iPhone 5
 * core, and each read waited for the BQL behind the IOP's NAND work and the display's frame copy: 62 ms a second per
 * core while booting.
 *
 * Mutation (must fail this test): drop ARM_CP_IO_READ_UNLOCKED from get_cp_reg64's condition, or from a counter.
 *
 * SLICE:counters target/arm/helper.c range { .name = "CNTPCT", .cp = 15, | { .name = "CNTP_CVAL", .cp = 15,
 * SLICE target/arm/tcg/op_helper.c range uint64_t HELPER(get_cp_reg64) | void HELPER(pre_hvc)
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { ARM_CP_STATE_AA32, ARM_CP_STATE_AA64 };
enum { PL0_R = 1 };
enum {
    ARM_CP_64BIT = 1 << 5,
    ARM_CP_IO = 1 << 9,
    ARM_CP_NO_RAW = 1 << 10,
    ARM_CP_IO_READ_UNLOCKED = 1 << 23,
};
typedef struct { int unused; } CPUARMState;
typedef struct ARMCPRegInfo ARMCPRegInfo;
struct ARMCPRegInfo {
    const char *name;
    int cp, crn, crm, opc0, opc1, opc2, state, access, type;
    int (*accessfn)(void);
    uint64_t (*readfn)(CPUARMState *, const ARMCPRegInfo *);
    void (*resetfn)(void);
};
#define HELPER(name) helper_##name

static int locked, reads_locked;
static void bql_lock(void) { locked++; }
static void bql_unlock(void) { locked--; }
static uint64_t counter(CPUARMState *env, const ARMCPRegInfo *ri) { reads_locked += locked; return 42; }
#define gt_cnt_read counter
#define gt_virt_cnt_read counter
static int gt_pct_access(void) { return 0; }
static int gt_vct_access(void) { return 0; }
static void arm_cp_reset_ignore(void) {}

static const ARMCPRegInfo counters[] = {
#include "counters.h"
};
#include "slice.h"

int main(void)
{
    CPUARMState env;
    ARMCPRegInfo io = { .name = "CNTP_TVAL", .type = ARM_CP_NO_RAW | ARM_CP_IO, .readfn = counter };

    assert(sizeof(counters) / sizeof(counters[0]) == 4);
    for (int i = 0; i < 4; i++) {
        reads_locked = 0;
        assert(helper_get_cp_reg64(&env, &counters[i]) == 42);
        assert(reads_locked == 0 && locked == 0);   /* no BQL for CNTPCT, CNTPCT_EL0, CNTVCT, CNTVCT_EL0 */
    }
    reads_locked = 0;
    assert(helper_get_cp_reg64(&env, &io) == 42);
    assert(reads_locked == 1 && locked == 0);       /* another ARM_CP_IO read still takes it */
    puts("PASS: the generic timer's counters read without the BQL; other I/O registers with it");
}
