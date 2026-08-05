/*
 * Get host pc for helper unwinding.
 *
 * Copyright (c) 2003 Fabrice Bellard
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef ACCEL_TCG_GETPC_H
#define ACCEL_TCG_GETPC_H

#ifndef CONFIG_TCG
#error Can only include this header with TCG
#endif

/* GETPC is the true target of the return instruction that we'll execute.  */
#ifdef CONFIG_TCG_INTERPRETER
extern __thread uintptr_t tci_tb_ptr;
# define GETPC() tci_tb_ptr
#elif defined(CONFIG_TCG_THREADED_INTERPRETER)
/*
 * A TCTI gadget calls the helper, so the helper's own return address points
 * back into the gadget, not into the translation. The gadget stashes the
 * address it will resume the bytecode at here.
 */
extern __thread uintptr_t tcti_call_return_address;
# define GETPC() tcti_call_return_address
#else
# define GETPC() \
    ((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)))
#endif

#endif /* ACCEL_TCG_GETPC_H */
