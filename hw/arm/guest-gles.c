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
#include "qemu/main-loop.h"
#include "accel/tcg/probe.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"
#include "hw/arm/guest-services/general.h"

/* The BQL protects guest access and renderer bookkeeping. Native drawing
 * needs neither, but another GL caller must not change the active context or
 * reuse its scratch arrays while it runs. Waiters release the BQL, allowing
 * timers, network I/O and other vCPUs to progress during that native work.
 * Reset and migration stop all vCPUs before touching renderer state. */
static QemuCond gles_execution_cond;
static bool gles_execution_initialized, gles_execution_busy;
static __thread bool gles_execution_owner;

static void gles_execution_enter(void)
{
    assert(bql_locked());
    assert(!gles_execution_owner);
    if (!gles_execution_initialized) {
        qemu_cond_init(&gles_execution_cond);
        gles_execution_initialized = true;
    }
    while (gles_execution_busy) {
        qemu_cond_wait_bql(&gles_execution_cond);
    }
    gles_execution_busy = gles_execution_owner = true;
}

static void gles_execution_leave(void)
{
    assert(bql_locked() && gles_execution_owner);
    gles_execution_busy = gles_execution_owner = false;
    qemu_cond_broadcast(&gles_execution_cond);
}

bool gles_native_begin(void)
{
    /* Direct host-side tests do not enter through the guest executor. */
    if (!gles_execution_owner) {
        return false;
    }
    assert(bql_locked());
    bql_unlock();
    return true;
}

void gles_native_end(bool unlocked)
{
    if (unlocked) {
        bql_lock();
        assert(gles_execution_owner);
    }
}

/*
 * Guest memory behind the pointers a GL call hands the host (client arrays,
 * indices, texels, parameter and output arrays).
 *
 * A debug access walks the page tables and nothing more: a page the guest
 * never touched (a static const vertex table in __TEXT, fresh zero-fill heap,
 * an untouched __DATA or __bss page) is not mapped yet and cannot be read, and
 * a debug write ignores protection, so it would land in a copy-on-write page
 * shared with other processes, or behind the kernel's modified-bit tracking.
 * Real GL reads these pointers with ordinary loads and stores, which fault.
 *
 * So inside a single GL call each access is first probed page by page through
 * the caller's MMU, at the caller's privilege. A page that would fault fails
 * the access and every later one in the call; when the call returns,
 * gles_guest_fault_in raises that fault as a data abort on the trapping mcr.
 * The kernel pages it in (or signals the app, as a bad pointer does on the
 * device) and returns to the mcr, which reissues the call. A call is only
 * armed when it is restartable: batches carry no guest pointers (glishim
 * queues only pointer-free calls) and are never armed. The draw paths check
 * gles_guest_fault_pending so a draw is never issued with an array missing.
 * Needs ARM_CP_RAISES_EXC on the trap register, so the PC is synced first.
 */
static struct {
    bool armed, pending;
    MMUAccessType type;
    vaddr va, last_va;
    unsigned repeats, slot;
} gf;

bool gles_guest_fault_pending(void)
{
    return gf.pending;
}

/*
 * The common case: every page is in the caller's TLB as plain RAM (no MMIO,
 * dirty tracking or watchpoint flags), so the bytes are copied through the
 * host pointers the probe returns, as a guest load or store would. A debug
 * access walks the page tables again for each page: a draw's three client
 * arrays cost ~1 us of walks before. Returns false, having possibly copied
 * part, when any page needs the full path; that path redoes the whole access.
 */
bool guest_rw_ram(CPUState *cpu, vaddr va, uint8_t *buf, size_t len, bool write)
{
    CPUArchState *env = cpu_env(cpu);
    MMUAccessType type = write ? MMU_DATA_STORE : MMU_DATA_LOAD;
    int idx = cpu_mmu_index(cpu, false);
    size_t done = 0;

    if (va + len - 1 < va || va + len - 1 > UINT32_MAX) {
        return false;
    }
    while (done < len) {
        vaddr p = va + done;
        size_t n = MIN(len - done, TARGET_PAGE_SIZE - (p & ~TARGET_PAGE_MASK));
        void *host;

        if (probe_access_flags(env, p, 0, type, idx, true, &host, 0) || !host) {
            return false;
        }
        if (write) {
            memcpy(host, buf + done, n);
        } else {
            memcpy(buf + done, host, n);
        }
        done += n;
    }
    return true;
}

int gles_guest_rw(CPUState *cpu, vaddr va, void *buf, size_t len, bool write)
{
    if (len && !gf.pending && guest_rw_ram(cpu, va, buf, len, write)) {
        return 0;
    }
    /* The debug access below ignores protection, so every page is probed at the
     * caller's privilege first, armed or not: an unarmed call (a batch) fails
     * where the guest's own access would fault rather than raise it. */
    if (len) {
        CPUArchState *env = cpu_env(cpu);
        MMUAccessType type = write ? MMU_DATA_STORE : MMU_DATA_LOAD;
        int idx = cpu_mmu_index(cpu, false);
        vaddr p = va, last = va + len - 1;
        void *host;

        if (gf.pending || last < va || last > UINT32_MAX) {
            return -1;
        }
        for (;;) {
            if (probe_access_flags(env, p, 0, type, idx, true, &host, 0) &
                TLB_INVALID_MASK) {
                if (!gf.armed) {
                    return -1;
                }
                gf.pending = true;
                gf.va = p;
                gf.type = type;
                return -1;
            }
            if ((p & TARGET_PAGE_MASK) == (last & TARGET_PAGE_MASK)) {
                break;
            }
            p = (p & TARGET_PAGE_MASK) + TARGET_PAGE_SIZE;
        }
    }
    return cpu_memory_rw_debug(cpu, va, buf, len, write);
}

static void gles_guest_fault_in(CPUState *cpu)
{
    gf.armed = false;
    if (!gf.pending) {
        gf.repeats = 0;
        return;
    }
    gf.pending = false;
    if (gf.va != gf.last_va) {
        gf.last_va = gf.va;
        gf.repeats = 0;
    } else if (++gf.repeats > 4) {
        /* The kernel resolved it but the probe still fails: give up on
         * this call (it fails as it always did) rather than loop. */
        if (gles_host_refuse("guest-read:fault-dropped")) {
            fprintf(stderr, "[gles] guest page 0x%08" PRIx64 " still not "
                    "accessible after 4 faults (slot %u, %s, page table: %s); call dropped\n",
                    (uint64_t)gf.va, gf.slot, gf.type == MMU_DATA_STORE ? "store" : "load",
                    cpu_get_phys_page_debug(cpu, gf.va) == -1 ? "unmapped" : "mapped");
        }
        gf.repeats = 0;
        return;
    }
    /* Raises the data abort and does not return (unless it is mapped now). */
    probe_access(cpu_env(cpu), gf.va, 1, gf.type, cpu_mmu_index(cpu, false), 0);
}

static bool gles_null_render(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_GLES_NULL") != NULL;
    }
    return on;
}

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

/* GLES_OP_BATCH: run a guest command buffer, one record per queued call. */
static int64_t gles_run_batch(CPUState *cpu, qc_gles_args_t *a)
{
    static uint32_t buf[GLES_BATCH_MAX_WORDS];
    uint32_t n = a->args[1], i = 0;

    if (a->argc != 2 || n > GLES_BATCH_MAX_WORDS ||
        gles_guest_rw(cpu, a->args[0], (uint8_t *)buf, n * 4, 0) != 0) {
        gles_bad_slots++;
        gles_host_refuse("bad-call:batch");
        return -1;
    }
    while (i < n) {
        uint32_t slot = buf[i] & 0xffff, argc = buf[i] >> 16;
        uint32_t args[GLES_MAX_ARGS] = { 0 };

        if (argc > GLES_MAX_ARGS || argc > n - i - 1 || slot >= GLES_OP_BASE) {
            gles_bad_slots++;
            gles_host_refuse("bad-call:batch-record");
            return -1;
        }
        memcpy(args, &buf[i + 1], argc * sizeof(uint32_t));
        gles_slot_calls[slot]++;
        gles_total_calls++;
        if (!gles_null_render()) {
            gles_host_call(cpu, slot, a->ctx, argc, args);
        }
        i += 1 + argc;
    }
    return 0;
}

static int64_t gles_handle_call(CPUState *cpu, qc_gles_args_t *a)
{
    if (a->slot == GLES_OP_BATCH) {
        return gles_run_batch(cpu, a);
    }
    if (a->slot >= GLES_MAX_SLOTS) {
        gles_bad_slots++;
        gles_host_refuse("bad-call:slot");
        return -1;
    }

    gles_slot_calls[a->slot]++;
    gles_total_calls++;

    /* Trace only the first sighting of each slot. A GL stream is tens of
     * thousands of calls a second; anything per-call would drown the log and
     * slow the guest enough to change what we are trying to measure. */
    if (gles_slot_calls[a->slot] == 1 && getenv("IT_GLES_VERBOSE")) {
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
        gles_bad_slots++;
        gles_host_refuse("bad-call:argc:%u", a->slot);
        return -1;
    }
    memset(args, 0, sizeof(args));
    gf.armed = true;
    gf.slot = a->slot;
    if (argc <= QC_GLES_INLINE_ARGS) {
        memcpy(args, a->args, argc * sizeof(uint32_t));
    } else {
        if (!a->spill) {
            gles_host_refuse("bad-call:no-spill:%u", a->slot);
            gf.armed = false;
            return -1;
        }
        if (gles_guest_rw(cpu, a->spill, args, argc * sizeof(uint32_t),
                          false) != 0) {
            if (!gf.pending) {
                gles_host_refuse("guest-read:spill:%u", a->slot);
            }
            return -1;
        }
    }

    /*
     * IT_GLES_NULL: accept every GL call and do nothing.
     *
     * This separates the two halves of a 3D workload's cost. The guest keeps
     * doing all its CPU work -- game logic, scene traversal, matrix math,
     * issuing calls -- while the rendering underneath costs nothing, so what
     * remains is the CPU-only frame rate. If that already clears the guest's
     * intended cadence, then offloading rendering to the host GPU is the whole
     * problem and the CPU emulation is fast enough as it stands.
     *
     * Reports success rather than the -1 that means "host refused", because a
     * refusal sends the guest down its SOFTWARE renderer -- which is more CPU
     * work, not less, and would measure the opposite of what is wanted.
     */
    if (gles_null_render()) {
        gf.armed = false;
        return 0;
    }

    int64_t r = gles_host_call(cpu, a->slot, a->ctx, argc, args);
    return r;
}

int64_t qc_handle_gles(CPUState *cpu, qc_gles_args_t *a)
{
    gles_execution_enter();
    gf.armed = gf.pending = false;
    int64_t result = gles_handle_call(cpu, a);
    gles_execution_leave();
    /* A guest data abort can longjmp out of this helper. Release the renderer
     * gate first, while still holding the BQL, so that fault retries cannot
     * strand the gate or let another caller overwrite the pending fault. */
    gles_guest_fault_in(cpu);
    return result;
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
