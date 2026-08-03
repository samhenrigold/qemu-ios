/*
 * OpenGL ES 1.1 high-level emulation -- guest/host request format.
 *
 * The guest side of this is a drop-in replacement for
 * /System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle. The stock
 * bundle talks to the AppleMBX kext through IOKit; ours talks to QEMU instead,
 * so the whole IOKit/MBX rendezvous is bypassed and the host renders with real
 * OpenGL.
 *
 * Every gl* entry point in OpenGLES.framework is the same six-instruction
 * trampoline: it loads the per-thread GC out of TSD key 30, indexes a
 * framework-owned dispatch table with a fixed slot, and tail-calls through it
 * with the GC as arg0. So a single request shape covers all 178 of them: the
 * slot says which call it was, and the arguments follow. Slot numbers are the
 * table's byte offset / 4 -- a numbering the framework already owns, so there
 * is no table of ours that has to be kept in sync with it.
 *
 * WHY THIS STRUCT IS EXACTLY 32 BYTES
 *
 * It sits in qemu_call_t's args union, and that union's size decides where
 * retval lands. contrib/it-kbd-agent hardcodes the layout as
 * "call_number(4) + args(32) + retval(8) + error(8)", and -- worse -- it is
 * *compiled into NAND images that already exist*. Widening the union would
 * shift retval out from under every agent binary already injected into every
 * image, with no build error anywhere to catch it. So this struct is capped at
 * 32 bytes, and general.h static-asserts the total.
 *
 * That cap is why arguments are split: four inline, and anything longer spilled
 * to a guest buffer. glTexImage2D and glCompressedTexSubImage2D take nine
 * scalars, which was never going to fit inline.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 */

#ifndef HW_ARM_GUEST_SERVICES_GLES_H
#define HW_ARM_GUEST_SERVICES_GLES_H

#include <stdint.h>

#define QC_GLES_INLINE_ARGS 4

typedef struct __attribute__((packed)) {
    /* Dispatch-table slot, i.e. the framework's table offset / 4. */
    uint32_t slot;
    /* The engine's GC handle -- arg0 of every trampoline. Opaque to the guest;
     * the host uses it to pick which GL context the call belongs to. */
    uint32_t ctx;
    /* How many 32-bit arguments this call carries, GC excluded. */
    uint32_t argc;
    /* Guest VA of the full argument array, used when argc > QC_GLES_INLINE_ARGS.
     * Zero when the inline array below carries everything. */
    uint32_t spill;
    /* Arguments widened to 32 bits. Floats travel as their bit patterns;
     * pointers are guest virtual addresses the host reads with
     * cpu_memory_rw_debug. */
    uint32_t args[QC_GLES_INLINE_ARGS];
} qc_gles_args_t;

#ifndef OUT_OF_TREE_BUILD
int64_t qc_handle_gles(CPUState *cpu, qc_gles_args_t *a);
void qc_gles_dump_stats(void);
#endif

#endif /* HW_ARM_GUEST_SERVICES_GLES_H */
