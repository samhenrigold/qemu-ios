/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

/*
 * 16K entries (128 page groups of 128 under softmmu) per address space. An
 * iOS app's indirect-branch working set (objc_msgSend, UIKit, the app)
 * thrashes upstream's 4K.
 */
#ifndef TB_JMP_CACHE_BITS
#define TB_JMP_CACHE_BITS 14
#endif
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCacheBank CPUJumpCacheBank;

typedef struct CPUJumpCache {
    CPUJumpCacheBank *bank;
    /* The bank epoch this cache was last emptied in; stale if behind. */
    uint64_t epoch;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

/*
 * One jump cache per guest address space (an ARM ASID), so that a context
 * switch, which must flush the softmmu TLB, can switch caches instead of
 * emptying the only one. A guest switching processes hundreds of times a
 * second otherwise sends every indirect branch after each switch to the QHT.
 *
 * Entries map a virtual PC to a TB, so a cache is valid only for its own
 * address space, and only until a mapping in it changes. Every operation
 * that may change mappings still clears: a page flush clears that page in
 * every cache, and a full flush (TLBIALL, tb_flush, reset, a CF_PCREL
 * invalidation) bumps @epoch, empties the current cache now and the others
 * when they are next switched to. Like the TLB, this relies on the guest
 * invalidating an ASID (or the page, or everything) before reusing it.
 *
 * cpu->tb_jmp_cache always points at the current member. Only the owning
 * vCPU switches; other threads use the pointer only to reach the bank, and
 * a stale entry they miss is harmless: an invalidated TB has CF_INVALID in
 * its cflags, so tb_lookup never matches it.
 *
 * 64 of them (16 MB of caches per vCPU, each touched only once used): iOS
 * runs a hundred and more processes, and a boot switches among more than 16
 * often enough that each switch back to an evicted one emptied it.
 */
#define TB_JMP_CACHE_CONTEXTS 64

struct CPUJumpCacheBank {
    struct rcu_head rcu;
    uint64_t epoch;                         /* atomic */
    uint64_t clock;                         /* owner vCPU only */
    uint64_t key[TB_JMP_CACHE_CONTEXTS];    /* owner vCPU only */
    uint64_t used[TB_JMP_CACHE_CONTEXTS];   /* owner vCPU only; 0: unkeyed */
    CPUJumpCache cache[TB_JMP_CACHE_CONTEXTS];
};

static inline void tb_jmp_cache_empty(CPUJumpCache *jc)
{
    for (int i = 0; i < TB_JMP_CACHE_SIZE; i++) {
        qatomic_set(&jc->array[i].tb, NULL);
    }
}

/*
 * The cache is two-way: a hash's set is its pair of slots, @h & ~1 and
 * @h | 1, and a lookup checks both. Code that returns to a page holds a TB
 * per call site in it, and a page group has only 128 slots, so a direct-mapped
 * cache lost about as many lookups to two of them sharing a slot as to empty
 * slots. A fill takes the first slot and moves what it held to the second.
 * Only the owning vCPU fills (other threads only clear @tb), so the move's
 * window, the second slot's new pc with its old tb, is never looked up.
 */
static inline void tb_jmp_cache_fill(CPUJumpCache *jc, uint32_t h, vaddr pc,
                                     TranslationBlock *tb)
{
    h &= ~1;
    jc->array[h + 1].pc = jc->array[h].pc;
    qatomic_set(&jc->array[h + 1].tb, qatomic_read(&jc->array[h].tb));
    jc->array[h].pc = pc;
    qatomic_set(&jc->array[h].tb, tb);
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
