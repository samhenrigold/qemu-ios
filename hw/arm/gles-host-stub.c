/*
 * Stub for hosts without a legacy OpenGL stack (currently: iOS builds).
 *
 * gles-host.c forwards the guest's GLES 1.1 calls into a CGL fixed-function
 * context, which only exists on macOS. Returning -1 here is the same "host
 * rejected the call" path a real gles-host takes when initialization fails,
 * so a guest probing the HLE simply falls back to its software path.
 *
 * iOS itself still ships the full fixed-function GLES 1.1 in
 * OpenGLES.framework, so a real EAGL-backed port is possible later.
 */

#include "qemu/osdep.h"
#include "cpu.h"

int64_t gles_host_call(CPUState *cpu, uint32_t slot, uint32_t ctx,
                       uint32_t argc, const uint32_t *a);
void gles_host_stats(uint64_t *draws, uint64_t *presents);

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
