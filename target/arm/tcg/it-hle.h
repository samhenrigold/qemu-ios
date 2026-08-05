/*
 * Guest framework hoisting -- host-side high-level emulation of individual
 * guest functions, intercepted in the translator by virtual address.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_ARM_TCG_IT_HLE_H
#define TARGET_ARM_TCG_IT_HLE_H

#include <stdbool.h>
#include <stdint.h>

/* What the translator should do when it reaches a hooked address. */
typedef enum {
    IT_HLE_NONE = 0,    /* not hooked */
    IT_HLE_COUNT,       /* call the counting helper, then run the guest code */
    IT_HLE_HOIST,       /* call the hoisting helper and return to the caller */
} ITHleAction;

/*
 * Cheap gate for the translate path. False (the common case, and always when
 * IT_HLE/IT_HLE_COUNT are unset) means it_hle_match() is never reached.
 */
bool it_hle_active(void);

/*
 * Is there a hook at this address, and does the instruction there still match
 * the one the hook was built against? The instruction word is the guard: a
 * shared-cache VA is the same in every guest process, but nothing guarantees
 * some other mapping does not land there, and a hook that fires on the wrong
 * code corrupts memory silently. Keying on (VA, first instruction word) makes
 * that a no-op instead.
 */
ITHleAction it_hle_match(uint32_t pc, uint32_t insn);

void it_hle_dump_stats(void);

#endif /* TARGET_ARM_TCG_IT_HLE_H */
