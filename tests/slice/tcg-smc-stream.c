/* A sequential run of stores into a code page invalidates to that end of the page at once, so a kernel zeroing a
 * recycled page stops taking the slow path for each of its 1024 words. Scattered stores invalidate only themselves.
 *
 * SLICE accel/tcg/tb-maint.c define SMC_STREAM_RUN
 * SLICE accel/tcg/tb-maint.c fn tb_invalidate_phys_range_fast
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_MASK ((uint64_t)-1 << TARGET_PAGE_BITS)
typedef uint64_t ram_addr_t;
typedef int CPUState, PageDesc;
struct page_collection { int unused; };
static PageDesc page;
static struct page_collection collection;
static ram_addr_t from, to;
static PageDesc *page_find(uint64_t index) { return &page; }
static struct page_collection *page_collection_lock(ram_addr_t start, ram_addr_t last) { return &collection; }
static void page_collection_unlock(struct page_collection *pages) { }
static void tb_invalidate_phys_page_range__locked(CPUState *cpu, struct page_collection *pages, PageDesc *p,
                                                  ram_addr_t start, ram_addr_t last, uintptr_t ra)
{
    from = start, to = last;
}
#include "slice.h"

int main(void)
{
    enum { BASE = 0x40000000, PAGE = 0x1000 };

    /* Upward: the first seven stores cover themselves, and the eighth run on to the page's end. */
    tb_invalidate_phys_range_fast(NULL, BASE, 4, 0);
    assert(from == BASE && to == BASE + 3);
    for (unsigned i = 1; i < SMC_STREAM_RUN; i++) {
        tb_invalidate_phys_range_fast(NULL, BASE + i * 4, 4, 0);
        assert(from == BASE + i * 4 && to == BASE + i * 4 + 3);
    }
    tb_invalidate_phys_range_fast(NULL, BASE + SMC_STREAM_RUN * 4, 4, 0);
    assert(from == BASE + SMC_STREAM_RUN * 4 && to == BASE + PAGE - 1);

    /* A store elsewhere ends the run. */
    tb_invalidate_phys_range_fast(NULL, BASE + 2 * PAGE + 0x100, 4, 0);
    assert(from == BASE + 2 * PAGE + 0x100 && to == BASE + 2 * PAGE + 0x103);

    /* Downward: on to the page's start. */
    ram_addr_t at = BASE + 4 * PAGE + 0x800;
    tb_invalidate_phys_range_fast(NULL, at, 4, 0);
    for (unsigned i = 1; i < SMC_STREAM_RUN; i++) {
        tb_invalidate_phys_range_fast(NULL, at - i * 4, 4, 0);
        assert(from == at - i * 4 && to == at - i * 4 + 3);
    }
    tb_invalidate_phys_range_fast(NULL, at - SMC_STREAM_RUN * 4, 4, 0);
    assert(from == BASE + 4 * PAGE && to == at - SMC_STREAM_RUN * 4 + 3);
    puts("PASS: a run of stores into a code page invalidates to its end; scattered stores only themselves");
    return 0;
}
