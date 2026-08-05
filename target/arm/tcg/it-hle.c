/*
 * Guest framework hoisting -- host-side high-level emulation of individual
 * guest functions, intercepted in the TRANSLATOR by virtual address.
 *
 * The premise: on this machine the only remaining lever is to make the guest
 * do less work, because there will never be a native JIT in the shipping app
 * and the TCG interpreter is roughly 30x slower than one. Anything the host
 * can do natively on the guest's behalf is paid for once at host speed instead
 * of thousands of interpreted guest instructions.
 *
 * WHY THE TRANSLATOR AND NOT THE GUEST IMAGE. Every framework on 3.1.3 --
 * CoreGraphics, QuartzCore, UIKit, CoreFoundation, libSystem -- lives ONLY
 * inside /System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6. There
 * is no on-disk binary to replace (verified: the .framework directories
 * contain Resources and a code signature and nothing else). The cache is a
 * single 96 MB blob mapped at FIXED virtual addresses, identical in every
 * guest process -- which is exactly what makes a VA hook unambiguous. So
 * rather than patch the cache in the NAND image (possible; it carries no code
 * signature) or lean on dyld's enable-dylibs-to-override-cache path, we
 * intercept where we are already omnipotent: at translation time.
 *
 * That buys three things an image edit does not: no repack of a NAND image, no
 * dependence on dyld semantics or code-signing state, and an A/B that is one
 * environment variable in one binary.
 *
 * THE GUARD. A hook is keyed on (virtual address, first instruction word), not
 * on the address alone. The shared cache is at the same VA in every process,
 * but nothing in the architecture promises no other mapping ever lands there,
 * and a hoist that fires on the wrong code would corrupt guest memory in a way
 * nothing else would report. Checking the instruction word costs nothing (the
 * translator has already fetched it) and turns that whole class of failure
 * into an ordinary miss.
 *
 * Copyright (c) 2026 the qemu-ios contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "internals.h"
#include "exec/helper-proto.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "it-hle.h"

/*
 * Which guest function a hook stands for. The ABI of each is recorded next to
 * its implementation below, read off the guest's own disassembly rather than
 * assumed from the C prototype.
 */
typedef enum {
    HLE_MEMCPY = 0,     /* also _memmove: ONE symbol, one address */
    HLE_MEMSET,
    HLE_BZERO,
    HLE_NUM_OPS,
} ITHleOp;

typedef struct {
    const char *name;
    uint32_t default_va;
    uint32_t guard_insn;
} ITHleOpDef;

/*
 * Provenance of these numbers, so they can be re-derived rather than trusted:
 * they come from the symbol table of /usr/lib/libSystem.B.dylib INSIDE
 * dyld_shared_cache_armv6 as shipped on iOS 3.1.3 build 7E18 (cache image base
 * 0x33904000), and the guard word is the first instruction at that address.
 * `_memcpy` and `_memmove` resolve to the SAME address -- the guest's memcpy
 * has always had memmove semantics, which is why the hoist below is a memmove.
 *
 * They are defaults, not constants: IT_HLE_VA_<name> overrides any of them for
 * a different image, and a wrong address simply fails the guard and does
 * nothing.
 */
static const ITHleOpDef hle_ops[HLE_NUM_OPS] = {
    [HLE_MEMCPY] = { "memcpy", 0x33905ca8, 0xe3520000 },  /* cmp r2, #0     */
    [HLE_MEMSET] = { "memset", 0x33905620, 0xe1a03002 },  /* mov r3, r2     */
    [HLE_BZERO]  = { "bzero",  0x33905638, 0xe3a02000 },  /* mov r2, #0     */
};

typedef struct {
    uint32_t va;
    uint32_t guard;
    ITHleAction action;
    /* Statistics. Kept per op so "is this worth hoisting" is answerable from
     * a count-only run, before any behaviour changes. */
    uint64_t calls;
    uint64_t bytes;
    uint64_t buckets[24];   /* log2 of the length argument */
    uint64_t misses;        /* right address, wrong instruction: see the guard */
} ITHleHook;

static ITHleHook hle_hooks[HLE_NUM_OPS];
static bool hle_any;
static bool hle_inited;
static int64_t hle_next_dump;
static int64_t hle_dump_interval;

static void hle_parse_list(const char *list, ITHleAction action)
{
    char **names = g_strsplit(list, ",", 0);

    for (int i = 0; names[i]; i++) {
        char *name = g_strstrip(names[i]);
        bool known = false;

        if (!*name) {
            continue;
        }
        for (int op = 0; op < HLE_NUM_OPS; op++) {
            if (g_str_equal(name, hle_ops[op].name) ||
                g_str_equal(name, "all")) {
                hle_hooks[op].action = action;
                known = true;
            }
        }
        if (!known) {
            fprintf(stderr, "[hle] unknown function '%s' -- known: ", name);
            for (int op = 0; op < HLE_NUM_OPS; op++) {
                fprintf(stderr, "%s%s", op ? ", " : "", hle_ops[op].name);
            }
            fprintf(stderr, ", all\n");
        }
    }
    g_strfreev(names);
}

static void hle_init(void)
{
    const char *hoist = getenv("IT_HLE");
    const char *count = getenv("IT_HLE_COUNT");
    const char *interval = getenv("IT_HLE_STATS");

    hle_inited = true;

    for (int op = 0; op < HLE_NUM_OPS; op++) {
        g_autofree char *var = g_strdup_printf("IT_HLE_VA_%s",
                                               hle_ops[op].name);
        const char *over = getenv(var);

        hle_hooks[op].va = over ? (uint32_t)strtoul(over, NULL, 0)
                                : hle_ops[op].default_va;
        hle_hooks[op].guard = hle_ops[op].guard_insn;
        hle_hooks[op].action = IT_HLE_NONE;
    }

    /* Counting is the safe mode and hoisting the one that changes behaviour,
     * so hoisting is applied second and wins if a name appears in both. */
    if (count) {
        hle_parse_list(count, IT_HLE_COUNT);
    }
    if (hoist) {
        hle_parse_list(hoist, IT_HLE_HOIST);
    }

    for (int op = 0; op < HLE_NUM_OPS; op++) {
        if (hle_hooks[op].action != IT_HLE_NONE) {
            hle_any = true;
            fprintf(stderr, "[hle] %s %s at guest 0x%08x (guard 0x%08x)\n",
                    hle_hooks[op].action == IT_HLE_HOIST ? "HOIST" : "count",
                    hle_ops[op].name, hle_hooks[op].va, hle_hooks[op].guard);
        }
    }

    hle_dump_interval = interval ? strtoll(interval, NULL, 0) : 0;
    if (hle_dump_interval > 0) {
        hle_next_dump = qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                        + hle_dump_interval * 1000;
    }
}

bool it_hle_active(void)
{
    if (!hle_inited) {
        hle_init();
    }
    return hle_any;
}

ITHleAction it_hle_match(uint32_t pc, uint32_t insn)
{
    for (int op = 0; op < HLE_NUM_OPS; op++) {
        if (hle_hooks[op].action == IT_HLE_NONE || hle_hooks[op].va != pc) {
            continue;
        }
        if (insn != hle_hooks[op].guard) {
            /* Same address, different code. Report it once -- silence here
             * would look exactly like "the hook never fired". */
            if (hle_hooks[op].misses++ == 0) {
                fprintf(stderr, "[hle] %s: guard failed at 0x%08x "
                        "(insn 0x%08x, expected 0x%08x) -- not hooking\n",
                        hle_ops[op].name, pc, insn, hle_hooks[op].guard);
            }
            return IT_HLE_NONE;
        }
        return hle_hooks[op].action;
    }
    return IT_HLE_NONE;
}

void it_hle_dump_stats(void)
{
    if (!hle_any) {
        return;
    }
    for (int op = 0; op < HLE_NUM_OPS; op++) {
        if (!hle_hooks[op].calls) {
            continue;
        }
        fprintf(stderr, "[hle] %-8s %12" PRIu64 " calls %14" PRIu64
                " bytes (mean %" PRIu64 ")\n",
                hle_ops[op].name, hle_hooks[op].calls, hle_hooks[op].bytes,
                hle_hooks[op].bytes / hle_hooks[op].calls);
        fprintf(stderr, "[hle]          size:");
        for (int b = 0; b < 24; b++) {
            if (hle_hooks[op].buckets[b]) {
                fprintf(stderr, " %u:%" PRIu64, 1u << b,
                        hle_hooks[op].buckets[b]);
            }
        }
        fprintf(stderr, "\n");
    }
}

static void hle_account(ITHleOp op, uint32_t len)
{
    int b = len ? 31 - __builtin_clz(len) : 0;

    hle_hooks[op].calls++;
    hle_hooks[op].bytes += len;
    hle_hooks[op].buckets[MIN(b, 23)]++;

    if (hle_dump_interval > 0 && (hle_hooks[op].calls & 0xfff) == 0) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (now >= hle_next_dump) {
            hle_next_dump = now + hle_dump_interval * 1000;
            it_hle_dump_stats();
        }
    }
}

/* Which op is at this pc. Only ever called on a hook that already matched. */
static ITHleOp hle_op_at(uint32_t pc)
{
    for (int op = 0; op < HLE_NUM_OPS; op++) {
        if (hle_hooks[op].va == pc && hle_hooks[op].action != IT_HLE_NONE) {
            return op;
        }
    }
    g_assert_not_reached();
}

/*
 * Count-only. The guest then executes its own code exactly as before, so this
 * mode cannot change behaviour -- it exists so the size of the opportunity can
 * be measured before anything is put at risk.
 *
 * Argument registers are read straight out of env: r0-r2 at the function's
 * first instruction still hold the arguments, and the counting helper is
 * emitted before the instruction runs.
 */
void HELPER(it_hle_count)(CPUARMState *env, uint32_t pc)
{
    ITHleOp op = hle_op_at(pc);

    /* bzero's length is its SECOND argument; memcpy's and memset's the third.
     * Getting this wrong would silently report a plausible-looking histogram
     * of the wrong register. */
    hle_account(op, op == HLE_BZERO ? env->regs[1] : env->regs[2]);
}

/*
 * Make the whole of [addr, addr+len) faultable before touching any of it.
 *
 * This ordering is load-bearing, not defensive. probe_access longjmps out to
 * deliver a guest page fault, and when the guest kernel returns, the FAULTING
 * INSTRUCTION re-executes -- which here is the hook, so the entire hoisted
 * call runs again from the start. That is only safe if nothing has been
 * written yet. iOS demand-pages both the shared cache and ordinary file
 * mappings, so this is a routine occurrence, not a corner case.
 */
static void hle_probe(CPUARMState *env, uint32_t addr, uint32_t len,
                      MMUAccessType type, int mmu_idx, uintptr_t ra)
{
    while (len) {
        uint32_t off = addr & ~TARGET_PAGE_MASK;
        uint32_t n = MIN(len, TARGET_PAGE_SIZE - off);

        probe_access(env, addr, n, type, mmu_idx, ra);
        addr += n;
        len -= n;
    }
}

/*
 * Copy len bytes guest->guest at host speed.
 *
 * probe_access returns a host pointer for RAM and NULL for anything that needs
 * to go through the I/O path; the byte loop is the fallback for the latter. It
 * should never fire for these three functions (nobody memcpys through a device
 * aperture), but "nothing in this tree can fail" is the project's most
 * expensive habit, so the case is handled rather than assumed away.
 */
static void hle_move(CPUARMState *env, uint32_t dst, uint32_t src, uint32_t len,
                     int mmu_idx, uintptr_t ra)
{
    while (len) {
        uint32_t n = MIN(len, TARGET_PAGE_SIZE - (dst & ~TARGET_PAGE_MASK));
        void *hd, *hs;

        n = MIN(n, TARGET_PAGE_SIZE - (src & ~TARGET_PAGE_MASK));
        hd = probe_access(env, dst, n, MMU_DATA_STORE, mmu_idx, ra);
        hs = probe_access(env, src, n, MMU_DATA_LOAD, mmu_idx, ra);

        if (hd && hs) {
            memmove(hd, hs, n);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                cpu_stb_data_ra(env, dst + i,
                                cpu_ldub_data_ra(env, src + i, ra), ra);
            }
        }
        dst += n;
        src += n;
        len -= n;
    }
}

static void hle_fill(CPUARMState *env, uint32_t dst, uint8_t c, uint32_t len,
                     int mmu_idx, uintptr_t ra)
{
    while (len) {
        uint32_t n = MIN(len, TARGET_PAGE_SIZE - (dst & ~TARGET_PAGE_MASK));
        void *hd = probe_access(env, dst, n, MMU_DATA_STORE, mmu_idx, ra);

        if (hd) {
            memset(hd, c, n);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                cpu_stb_data_ra(env, dst + i, c, ra);
            }
        }
        dst += n;
        len -= n;
    }
}

/*
 * Hoist. Performs the call host-side and returns to the caller, so the guest
 * never executes the function body at all.
 *
 * Return is a full BX of lr: the shared cache's callers are a mix of ARM and
 * Thumb, so bit 0 of lr selects the state and dropping it would land the CPU
 * in the wrong decoder one call in two.
 *
 * Registers: r0 keeps the destination, which is what all three of these
 * functions return. r1-r3 and r12 are left as they were rather than clobbered
 * the way the real code leaves them -- caller-saved either way, and strictly
 * the safer direction.
 */
void HELPER(it_hle_hoist)(CPUARMState *env, uint32_t pc)
{
    uintptr_t ra = GETPC();
    int mmu_idx = arm_to_core_mmu_idx(arm_mmu_idx(env));
    ITHleOp op = hle_op_at(pc);
    uint32_t lr = env->regs[14];
    uint32_t dst = env->regs[0];
    uint32_t len;

    switch (op) {
    case HLE_MEMCPY:
        len = env->regs[2];
        hle_account(op, len);
        if (len && dst != env->regs[1]) {
            hle_probe(env, env->regs[1], len, MMU_DATA_LOAD, mmu_idx, ra);
            hle_probe(env, dst, len, MMU_DATA_STORE, mmu_idx, ra);
            hle_move(env, dst, env->regs[1], len, mmu_idx, ra);
        }
        break;

    case HLE_MEMSET:
        len = env->regs[2];
        hle_account(op, len);
        if (len) {
            hle_probe(env, dst, len, MMU_DATA_STORE, mmu_idx, ra);
            hle_fill(env, dst, env->regs[1] & 0xff, len, mmu_idx, ra);
        }
        break;

    case HLE_BZERO:
        len = env->regs[1];
        hle_account(op, len);
        if (len) {
            hle_probe(env, dst, len, MMU_DATA_STORE, mmu_idx, ra);
            hle_fill(env, dst, 0, len, mmu_idx, ra);
        }
        break;

    default:
        g_assert_not_reached();
    }

    env->regs[0] = dst;
    env->thumb = lr & 1;
    env->regs[15] = lr & ~(uint32_t)1;
}
