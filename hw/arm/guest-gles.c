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

/* Slot numbers we care about first; see GATE1_slotmap.txt for the full map.
 * These four are the Step 3 target: clear the buffer, point at some vertices,
 * draw, and see it. */
#define GLES_SLOT_CLEAR         10   /* 0x0038 glClear        */
#define GLES_SLOT_CLEAR_COLOR   12   /* 0x0040 glClearColor   */
#define GLES_SLOT_DRAW_ARRAYS   65   /* 0x0114 glDrawArrays   */
#define GLES_SLOT_VERTEX_PTR    334  /* 0x0548 glVertexPointer */

/* One counter per slot. The framework's table is 0xCE4 bytes, so slots run to
 * 0x320; round up and never index out of it -- a bad slot from the guest must
 * not be able to scribble on the host. */
#define GLES_MAX_SLOTS 1024

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

    switch (a->slot) {
    case GLES_SLOT_CLEAR:
    case GLES_SLOT_CLEAR_COLOR:
    case GLES_SLOT_DRAW_ARRAYS:
    case GLES_SLOT_VERTEX_PTR:
        /* Recognised, but there is no host GL context yet. Returning 0 keeps
         * the guest running so the call stream can be observed end to end;
         * this is deliberately not an error path. */
        return 0;
    default:
        return 0;
    }
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
