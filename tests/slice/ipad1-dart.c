/* DART IOMMU register block: segment-table program/clear round-trip and the never-busy TLB_OP.
 *
 * The shared S5L8930Dart register file (hw/arm/s5l8930_dart.c) backs both dart1 (TYPE_S5L8930_DART) and the
 * display's dart2. AppleS5L8930XDART clears every stream's segment table at start by writing DATA then
 * TLB_OP = seg<<22|sid<<8|5, and spins reading TLB_OP for the busy bit; this pins that contract with no QEMU.
 *
 * Mutation (named, must fail this test): in dart_write's TLB_OP case, delete
 *     d->ste[sid][seg] = d->regs[DART_DATA / 4];   (the op-5 STE store)
 * so programming a segment table stores nothing and the op-4 read-back returns 0.
 *
 * SLICE include/hw/arm/s5l8930.h define S5L8930_DART_
 * SLICE include/hw/arm/s5l8930.h typedef S5L8930Dart
 * SLICE hw/arm/s5l8930_dart.c define DART_
 * SLICE hw/arm/s5l8930_dart.c fn dart_read dart_write
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t hwaddr;
#include "slice.h"

#define RD(a)   dart_read(&d, (a), 4)
#define WR(a,v) dart_write(&d, (a), (v), 4)
static uint32_t tlb_op(unsigned sid, unsigned seg, unsigned op) {
    return (seg << 22) | (sid << 8) | op;
}
int main(void) {
    S5L8930Dart d = {0};

    /* TLB_OP never reports busy: the driver's spin-on-bit3 exits at once. */
    assert(RD(DART_TLB_OP) == 0);

    /* Program a stream's segment: DATA = STE, then op 5 stores it; op 4 reads back. */
    WR(DART_DATA, 0xdead1000);
    WR(DART_TLB_OP, tlb_op(2, 7, 5));
    WR(DART_DATA, 0);                    /* clear the data latch before the read-back */
    WR(DART_TLB_OP, tlb_op(2, 7, 4));
    assert(RD(DART_DATA) == 0xdead1000);
    assert(RD(DART_TLB_OP) == 0);        /* still never busy after an op */

    /* Distinct (sid, seg) slots do not alias. */
    WR(DART_DATA, 0xbeef2000);
    WR(DART_TLB_OP, tlb_op(3, 63, 5));
    WR(DART_DATA, 0);
    WR(DART_TLB_OP, tlb_op(2, 7, 4));
    assert(RD(DART_DATA) == 0xdead1000);
    WR(DART_TLB_OP, tlb_op(3, 63, 4));
    assert(RD(DART_DATA) == 0xbeef2000);

    /* Segment-table clear: DATA = 0, op 5 -> the entry is zero again. */
    WR(DART_DATA, 0);
    WR(DART_TLB_OP, tlb_op(2, 7, 5));
    WR(DART_DATA, 0x11111111);
    WR(DART_TLB_OP, tlb_op(2, 7, 4));
    assert(RD(DART_DATA) == 0);

    /* An out-of-range stream id is dropped, not an OOB write (ASan would trip). */
    WR(DART_DATA, 0x22223000);
    WR(DART_TLB_OP, tlb_op(S5L8930_DART_SIDS, 0, 5));

    /* ERROR_STATUS is write-1-to-clear. */
    d.regs[DART_ERROR_STATUS / 4] = 0xf;
    WR(DART_ERROR_STATUS, 0x5);
    assert(RD(DART_ERROR_STATUS) == 0xa);

    puts("PASS: DART segment-table program/clear round-trip, no aliasing, never-busy TLB_OP, W1C errors");
}
