/*
 * OpenGL ES 1.1 high-level emulation -- host-side dispatch.
 *
 * The guest's replacement for MBXGLEngine.bundle turns every gl* call into a
 * QC_GLES request and traps to the host with the cp15 QEMU_CALL register. This
 * file is where those requests land.
 *
 * Right now it only accounts for what arrives: each slot is counted and the
 * first few are traced, so the guest shim can be brought up and verified one
 * entry point at a time before any host GL context exists. Slots are the
 * OpenGLES framework's own dispatch-table offsets divided by four -- an
 * engine-independent numbering that we did not choose, so there is no table of
 * ours to keep in sync with the framework's.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/arm/guest-services/general.h"

/* One counter per slot. The framework's table tops out at slot 820 and our
 * engine-level ops start at GLES_OP_BASE (0x1000), so this covers both. A slot
 * the guest invents must never index outside it -- the guest is not trusted to
 * stay in range. */
#define GLES_MAX_SLOTS (GLES_OP_BASE + 16)

/* Widest ES 1.1 entry point is nine scalars (glTexImage2D,
 * glCompressedTexSubImage2D). */
#define GLES_MAX_ARGS 12

static uint64_t gles_slot_calls[GLES_MAX_SLOTS];
static uint64_t gles_total_calls;
static uint64_t gles_bad_slots;

int64_t qc_handle_gles(CPUState *cpu, qc_gles_args_t *a)
{
    if (a->slot >= GLES_MAX_SLOTS) {
        gles_bad_slots++;
        return -1;
    }

    gles_slot_calls[a->slot]++;
    gles_total_calls++;

    /* Trace only the first sighting of each slot. A GL stream is tens of
     * thousands of calls a second; anything per-call would drown the log and
     * slow the guest enough to change what we are trying to measure. */
    if (gles_slot_calls[a->slot] == 1) {
        fprintf(stderr, "[gles] slot %u first call: ctx=0x%08x argc=%u "
                "spill=0x%08x args %08x %08x %08x %08x\n",
                a->slot, a->ctx, a->argc, a->spill,
                a->args[0], a->args[1], a->args[2], a->args[3]);
    }

    /* Gather the arguments. Four ride inline; anything longer was spilled to a
     * guest buffer, because qc_gles_args_t is capped at 32 bytes to keep
     * qemu_call_t's layout frozen (see general.h). */
    uint32_t args[GLES_MAX_ARGS];
    uint32_t argc = a->argc;

    if (argc > GLES_MAX_ARGS) {
        fprintf(stderr, "[gles] slot %u: argc %u out of range\n",
                a->slot, argc);
        gles_bad_slots++;
        return -1;
    }
    memset(args, 0, sizeof(args));
    if (argc <= QC_GLES_INLINE_ARGS) {
        memcpy(args, a->args, argc * sizeof(uint32_t));
    } else {
        if (!a->spill) {
            fprintf(stderr, "[gles] slot %u: argc %u but no spill pointer\n",
                    a->slot, argc);
            return -1;
        }
        if (cpu_memory_rw_debug(cpu, a->spill, (uint8_t *)args,
                                argc * sizeof(uint32_t), 0) != 0) {
            fprintf(stderr, "[gles] slot %u: cannot read %u spilled args at "
                    "guest 0x%08x\n", a->slot, argc, a->spill);
            return -1;
        }
    }

    return gles_host_call(cpu, a->slot, a->ctx, argc, args);
}

void qc_gles_dump_stats(void)
{
    unsigned i;
    fprintf(stderr, "[gles] %" PRIu64 " calls total, %" PRIu64 " bad slots\n",
            gles_total_calls, gles_bad_slots);
    for (i = 0; i < GLES_MAX_SLOTS; i++) {
        if (gles_slot_calls[i]) {
            fprintf(stderr, "[gles]   slot %4u: %" PRIu64 "\n",
                    i, gles_slot_calls[i]);
        }
    }
}
