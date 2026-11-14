/* hw/block/pflash_cfi02.c: the iPhone's and iPod touch 1G's NOR (a CFI flash on -drive if=pflash) writes its image
 * from a coroutine, so a program's host write waits off the BQL, and the programs made while a write is in flight
 * join the range written next.
 *
 * The coroutine runs when the test says the write in flight has ended. With the change reverted pflash_update writes
 * each word's sector inside the MMIO write (and pflash_update_co, which this test cuts, is not there).
 *
 * SLICE hw/block/pflash_cfi02.c fn pflash_update_co pflash_update
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BDRV_SECTOR_SIZE 512
#define QEMU_ALIGN_DOWN(n, m) ((n) / (m) * (m))
#define QEMU_ALIGN_UP(n, m) QEMU_ALIGN_DOWN((n) + (m) - 1, (m))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define error_report(...) ((void)0)
#define g_autofree __attribute__((cleanup(free_ptr)))
static void free_ptr(void *p) { free(*(void **)p); }
static void *g_memdup2(const void *p, size_t n) { void *q = malloc(n); memcpy(q, p, n); return q; }
typedef int BlockBackend;
typedef struct PFlashCFI02 {
    BlockBackend *blk;
    void *storage;
    bool updating;
    int dirty_start, dirty_end;
} PFlashCFI02;
#define coroutine_fn
typedef struct { void (*fn)(void *); void *arg; } Coroutine;
static Coroutine co;
static int deferred;
static Coroutine *qemu_coroutine_create(void (*fn)(void *), void *arg) { co.fn = fn; co.arg = arg; return &co; }
static void qemu_coroutine_enter(Coroutine *c) { (void)c; deferred++; }
static int writes, written_offset, written_len;
static uint8_t written[4];
static int blk_pwrite(BlockBackend *blk, int64_t offset, int64_t len, const void *buf, int flags)
{
    (void)blk; (void)flags;
    writes++; written_offset = offset; written_len = len;
    memcpy(written, (const uint8_t *)buf + 0x202 - offset, 4);
    return 0;
}
#include "slice.h"

int main(void)
{
    static uint8_t storage[0x10000];
    BlockBackend blk;
    PFlashCFI02 pfl = { .blk = &blk, .storage = storage };

    storage[0x202] = 0x5a;
    pflash_update(&pfl, 0x202, 2);                   /* a word program: its sector waits for the coroutine */
    assert(deferred == 1 && pfl.updating && writes == 0);
    storage[0x203] = 0xa5;
    pflash_update(&pfl, 0x203, 1);                   /* more while the first is in flight: the same range */
    pflash_update(&pfl, 0x1000, 2);                  /* and another sector */
    assert(deferred == 1 && pfl.dirty_start == 0x200 && pfl.dirty_end == 0x1200);
    deferred = 0;
    co.fn(co.arg);
    assert(writes == 1 && written_offset == 0x200 && written_len == 0x1000 && !pfl.updating);
    assert(written[0] == 0x5a && written[1] == 0xa5);
    puts("PASS: pflash_cfi02 writes its image from a coroutine; programs meanwhile join the range written next");
    return 0;
}
