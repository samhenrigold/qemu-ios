/*
 * Stub for hosts with no fixed-function GL at all.
 *
 * gles-host.c forwards the guest's GLES 1.1 calls into a real host context:
 * CGL on macOS, EAGL on iOS. Neither framework exists anywhere else, so on any
 * other host this takes its place. Returning -1 is the same "host rejected the
 * call" path gles-host.c itself takes when its context cannot be created, so a
 * guest probing the HLE simply falls back to its software renderer.
 */

#include "qemu/osdep.h"
#include "cpu.h"

int64_t gles_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx,
                       uint32_t argc, const uint32_t *a);
void gles_host_stats(uint64_t *draws, uint64_t *presents);
void gles_host_set_allowed(bool allowed);
void gles_host_reset(void);

int64_t gles_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx,
                       uint32_t argc, const uint32_t *a)
{
    return -1;
}

void gles_host_stats(uint64_t *draws, uint64_t *presents)
{
    *draws = 0;
    *presents = 0;
}

/* Nothing to gate when there is no host GL in the first place. */
void gles_host_set_allowed(bool allowed)
{
}

void gles_host_reset(void)
{
}
