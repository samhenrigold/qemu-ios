/* MBX interrupt registers (the shared model, both boards): mask read-back, the
 * software interrupt, write-1-to-clear, and the line following status & mask.
 *
 * hw/arm/ipod_touch_mbx.c wires the GLES completion path 1.x's AppleMBX drives:
 * 0x130 is the interrupt mask (and reads back), a write to 0x12c raises status
 * bits (the driver's own software interrupt), 0x134 clears them W1C, and the IRQ
 * line is (status & mask) != 0. This is the gles-1x fix: before it the line did
 * not track the mask, so the fourth LayerKit swap under LK_ENABLE_OGL waited
 * forever. Driven here with no emulator.
 *
 * Mutation (named, must fail this test): in ipod_touch_mbx_update_irq change
 *     qemu_set_irq(s->irq, (s->status & s->int_mask) != 0);
 * to
 *     qemu_set_irq(s->irq, s->status != 0);
 * so an unmasked status bit still raises the line.
 *
 * SLICE include/hw/arm/mbx_fill.h file
 * SLICE hw/arm/mbx_fill.c define RING_|HEADER|SUBMIT|PAGES
 * SLICE hw/arm/mbx_fill.c fn load_le32 reject mbx_fill_reset mbx_fill_write
 * SLICE hw/arm/ipod_touch_mbx.c define MBX_[A-Z0-9_]+[[:space:]]
 * SLICE include/hw/arm/ipod_touch_mbx.h typedef IPodTouchMBXState
 * SLICE hw/arm/ipod_touch_mbx.c fn ipod_touch_mbx1_read ipod_touch_mbx_update_irq ipod_touch_mbx1_write
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t hwaddr;
typedef int SysBusDevice, MemoryRegion;
typedef int *qemu_irq;
typedef struct { bool pending; int64_t deadline; } QEMUTimer;
#define IPOD_TOUCH_MBX(s) ((IPodTouchMBXState *)(s))
#define QEMU_CLOCK_VIRTUAL 0
#define MBX_TRACE(...) ((void)0)
static int64_t now;
static int64_t qemu_clock_get_ns(int c) { return now; }
static void qemu_set_irq(int *irq, int v) { if (irq) *irq = !!v; }
static void qemu_irq_lower(int *irq) { if (irq) *irq = 0; }
static void qemu_irq_raise(int *irq) { if (irq) *irq = 1; }
static void timer_mod(QEMUTimer *t, int64_t at) { t->pending = true; t->deadline = at; }
static void timer_del(QEMUTimer *t) { t->pending = false; }
static uint8_t *mbx_fill_guest_ram(void *o, uint32_t p, uint32_t n) { return NULL; }
static void mbx_fill_guest_write(void *o, uint32_t p, uint32_t v) {}
#include "slice.h"
#define RD(a)   ipod_touch_mbx1_read(&s, (a), 4)
#define WR(a,v) ipod_touch_mbx1_write(&s, (a), (v), 4)
int main(void) {
    int line = 0;
    IPodTouchMBXState s = { .irq = &line };  /* complete_shim off, irq_enabled off */

    /* 0x130 is the mask and reads back verbatim; arming it with status 0 keeps the line low. */
    WR(0x130, 0x4c);
    assert(RD(0x130) == 0x4c && line == 0);

    /* 0x12c software interrupt: a masked bit raises the line. */
    WR(0x12c, 0x40);
    assert(line == 1);

    /* An UNmasked status bit does not raise it (the gles-1x contract). */
    WR(0x134, 0x40);           /* back to status 0, line low */
    assert(line == 0);
    WR(0x12c, 0x02);           /* 0x02 is not in mask 0x4c */
    assert(line == 0);

    /* With both a masked and an unmasked bit set, the line follows the masked one... */
    WR(0x12c, 0x08);           /* status = 0x0a, 0x08 in mask */
    assert(line == 1);
    /* ...and clearing only the masked bit (W1C) drops it, the unmasked bit remaining. */
    WR(0x134, 0x08);           /* status = 0x02, unmasked */
    assert(line == 0);

    /* Narrowing the mask to exclude a set status bit lowers the line without touching status. */
    WR(0x12c, 0x40);           /* status = 0x42, 0x40 masked -> line high */
    assert(line == 1);
    WR(0x130, 0x01);           /* mask now excludes every set status bit (0x42) */
    assert(RD(0x130) == 0x01 && line == 0);

    /* STATUS observes pending events; only the explicit W1C acknowledges
     * the enabled subset sampled by the stock ISR. Exercise both options. */
    for (unsigned mode = 0; mode < 2; mode++) {
        s = (IPodTouchMBXState){ .irq = &line, .irq_enabled = mode };
        line = 0;
        WR(0x130, 0x400);
        WR(0x12c, 0x408);
        assert(line == 1);
        assert(RD(0x12c) == 0x548 && s.status == 0x408 && line == 1);
        assert(RD(0x12c) == 0x548 && s.status == 0x408 && line == 1);
        WR(0x130, 0);
        assert(line == 0);
        assert(RD(0x12c) == 0x548 && s.status == 0x408 && line == 0);
        WR(0x130, 0x400);
        assert(line == 1);
        uint32_t observed = RD(0x12c);
        WR(0x134, observed & 0x400);
        assert(s.status == 8 && line == 0);
        WR(0x130, 8);
        assert(line == 1);
        assert(RD(0x12c) == 0x148 && s.status == 8 && line == 1);
        WR(0x134, 8);
        assert(s.status == 0 && line == 0);
        assert(RD(0x12c) == 0x140); /* Existing compatibility bits unchanged. */
    }
    puts("PASS: MBX mask/readback, STATUS observation, software interrupt, selective W1C and IRQ redrive (both options)");
}
