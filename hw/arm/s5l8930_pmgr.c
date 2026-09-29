/*
 * S5L8930 ("A4") PMGR: PLLs, clock-config and clock-gate registers, the
 * 24 MHz timebase and event timers (+0x2000), the watchdog (+0x2020) and
 * POWER_ID (+0x6000). One block because the kernel maps it as one: the DT
 * pmgr node is both device_type "timer" (pe_arm_init_timer) and
 * AppleS5L8930XPerformanceController's register window.
 *
 * Register contract: docs/research/gap-kernel-platform-mmio.md §1.1-1.3.
 * Nothing here decodes clock frequencies; the kernel takes those from the DT.
 * The PLL/clock-config values only have to satisfy the kernel's polls.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"
#include "hw/qdev-properties.h"
#include "system/runstate.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930PMGRState, S5L8930_PMGR)

#define PMGR_TIMEBASE_HZ    24000000

/* PLL CON0 at 0x00/0x08/0x10/0x20 (A/M/E/VPLL), CON1 at +4. */
#define PLL_CON0_ENABLE     (1u << 31)
#define PLL_CON0_LOCKED     (1u << 29)
#define PLL_CON0_UPDATE     (1u << 27)
#define PLL_END             0x28

/* 48 clock-config regs; bit 30 is the "change pending" bit the kernel polls. */
#define CLKCFG_START        0x40
#define CLKCFG_END          0x100
#define CLKCFG_BUSY         (1u << 30)

/* DPSM slots 0x100 + 0x10*i, i = 0..3; the kernel polls these two bits clear. */
#define DPSM_START          0x100
#define DPSM_END            0x140
#define DPSM_BUSY           (0x40000000u | 0x200u)

/* Gate n at 0x1010 + 4n: bits 0-3 request, 4-7 actual, bit 31 reset pulse. */
#define GATE_START          0x1010
#define GATE_END            (GATE_START + 4 * 0x40)
#define GATE_RESET          (1u << 31)

#define PMGR_GATE_CTL       0x1200
#define PMGR_TICKS_LO       0x2000
#define PMGR_TICKS_HI       0x2004
#define PMGR_EVT_COUNT(n)   (0x2008 + 4 * (n))
#define PMGR_EVT_STATE(n)   (0x2010 + 4 * (n))
#define EVT_STATE_START     (1u << 0)
#define EVT_STATE_UPDATE    (1u << 1)

/* Watchdog; the names follow iBoot's rPMGR_WDOG_TMR/RST/INTR/CTL. */
#define PMGR_WDOG_TMR       0x2020
#define PMGR_WDOG_RST       0x2024
#define PMGR_WDOG_INTR      0x2028
#define PMGR_WDOG_CTL       0x202C
#define WDOG_CTL_RESET_EN   (1u << 2)

#define PMGR_CLOCK_CON1     0x5030
#define CLOCK_CON1_BUSY     (1u << 16)
#define PMGR_POWER_ID       0x6000

typedef struct S5L8930EventTimer {
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t count;     /* count buffer, as last written */
    uint32_t state;
    uint64_t start;     /* timebase tick the countdown began at */
    uint32_t load;      /* value it counts down from */
    bool pending;
} S5L8930EventTimer;

struct S5L8930PMGRState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t regs[S5L8930_PMGR_SIZE / 4];
    uint8_t security_epoch;     /* POWER_ID[31:24] LLB would latch; 0 = measured */
    int64_t tick_base_ns;
    S5L8930EventTimer evt[2];
};

/*
 * Reset values stand in for iBoot's state, since the kernel is booted
 * without it. PLLs are iEmu's (APLL 1000 MHz, EPLL 1026 MHz, VPLL 48 MHz;
 * IEMU/s5l8930.c:388-398), except MPLL, which iEmu leaves off: 400 MHz here
 * (M=100 P=3 S=2), since the A4's memory clock comes from somewhere.
 * POWER_ID: epoch 1, board-id 2, read from a real K48AP (iEmu had epoch 2).
 */
static const struct {
    hwaddr off;
    uint32_t val;
} pmgr_defaults[] = {
    { 0x00, 0xA00187D1 }, { 0x04, 0x00380960 },
    { 0x08, 0xA000C322 }, { 0x0C, 0x00380960 },
    { 0x10, 0xA0010559 }, { 0x14, 0x00380960 },
    { 0x20, 0xA0008205 }, { 0x24, 0x00380960 },
    /* Clock muxes/dividers reconstructed from the real K48 clock-frequencies
     * property and iBoot's PMGR decoder. Zero divisors mean an unavailable
     * clock; leaving all registers zero made iBoot publish a zero FMI clock. */
    { 0x40, 0x00802000 },
    { 0x44, 0x00000005 },
    { 0x48, 0x2000000a },
    { 0x4c, 0x20000001 },
    { 0x50, 0x00000006 },
    { 0x54, 0x2000000f },
    { 0x5c, 0x20000013 },
    { 0x60, 0x20000003 },
    { 0x64, 0x00000000 },
    { 0x68, 0x20000015 },
    { 0x70, 0x00000002 },
    { 0x74, 0x10000002 },
    { 0x78, 0x00000005 },
    { 0x80, 0x00000000 },
    { 0x84, 0x2000000f },
    { 0x88, 0x20000003 },
    { 0x8c, 0x20000006 },
    { 0x90, 0x30000001 },
    { 0x94, 0x2000000f },
    { 0x98, 0x00000000 },
    { 0x9c, 0x00000000 },
    { 0xa0, 0x00000000 },
    { 0xa4, 0x10000000 },
    { 0xa8, 0x30000000 },
    { 0xac, 0x10000000 },
    { 0xb0, 0x10000000 },
    { 0xb4, 0x10000000 },
    { 0xb8, 0x00000000 },
    { 0xbc, 0x00000000 },
    { 0xc4, 0x0000000c },
    { 0xc8, 0x00000000 },
    { 0xcc, 0x00000000 },
    { 0xd0, 0x10000000 },
    { 0xd4, 0x00000002 },
    { 0xdc, 0x20000000 },
    { 0xe0, 0x20000000 },
    { 0xe4, 0x20000000 },
    { 0xe8, 0x20000000 },
    { 0xec, 0x20000000 },
    { 0xf0, 0x00000002 },
    { 0xf4, 0x00000002 },
    { 0xf8, 0x00000002 },
    { 0xfc, 0x00000000 },
    { PMGR_POWER_ID, 0x01020001 },   /* measured on a real K48AP (epoch 1, board 2) */
};

static bool pmgr_modelled(hwaddr off)
{
    return off < PLL_END ||
           (off >= CLKCFG_START && off < DPSM_END) ||
           (off >= GATE_START && off < GATE_END) ||
           off == PMGR_GATE_CTL ||
           (off >= PMGR_TICKS_LO && off <= PMGR_WDOG_CTL) ||
           off == PMGR_CLOCK_CON1 || off == PMGR_POWER_ID;
}

static uint64_t pmgr_ticks(S5L8930PMGRState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->tick_base_ns,
                    PMGR_TIMEBASE_HZ, NANOSECONDS_PER_SECOND);
}

static uint32_t evt_remaining(S5L8930PMGRState *s, S5L8930EventTimer *t)
{
    uint64_t elapsed = pmgr_ticks(s) - t->start;

    return elapsed < t->load ? t->load - elapsed : 0;
}

/* (Re)start the countdown from `load` ticks, now. */
static void evt_arm(S5L8930PMGRState *s, S5L8930EventTimer *t, uint32_t load)
{
    t->start = pmgr_ticks(s);
    t->load = load;
    if (!(t->state & EVT_STATE_START)) {
        timer_del(t->timer);
        return;
    }
    /* +1 ns: muldiv64 rounds down, and expiring before the counter gets
     * there would hand rtclock a deadline that has not yet passed. */
    timer_mod(t->timer, s->tick_base_ns + 1 +
              muldiv64(t->start + load, NANOSECONDS_PER_SECOND,
                       PMGR_TIMEBASE_HZ));
}

/* Expiry asserts the line and stops; it stays up until a STATE write with
 * UPDATE, which is how the kernel's FIQ entry acks it (3 then 1). */
static void evt_expire(void *opaque)
{
    S5L8930EventTimer *t = opaque;

    t->pending = true;
    qemu_irq_raise(t->irq);
}

static void evt_write_state(S5L8930PMGRState *s, S5L8930EventTimer *t,
                            uint32_t val)
{
    t->state = val;
    if (val & EVT_STATE_UPDATE) {
        t->pending = false;
        qemu_irq_lower(t->irq);
        evt_arm(s, t, t->count);
    } else if (!(val & EVT_STATE_START)) {
        timer_del(t->timer);
    } else if (!t->pending && !timer_pending(t->timer)) {
        /* START without UPDATE on a stopped timer resumes what was left. */
        evt_arm(s, t, evt_remaining(s, t));
    }
}

/*
 * The kernel only ever uses the watchdog to reset: WDTr writes CTL = arg|4,
 * RST = 0, TMR = 0 and spins (AppleS5L8930X c0645180, 7B500). So fire when
 * reset is enabled and TMR has reached RST.
 * ponytail: TMR does not count, so a guest arming a real timeout is never
 * reset; make it tick at the timebase if anything ever does that.
 */
static void wdog_check(S5L8930PMGRState *s)
{
    if ((s->regs[PMGR_WDOG_CTL / 4] & WDOG_CTL_RESET_EN) &&
        s->regs[PMGR_WDOG_TMR / 4] >= s->regs[PMGR_WDOG_RST / 4]) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t s5l8930_pmgr_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8930PMGRState *s = opaque;
    uint32_t val = s->regs[off / 4];

    if (!pmgr_modelled(off)) {
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled read 0x%04x\n",
                      __func__, (unsigned)off);
        return val;
    }

    switch (off) {
    /* The kernel reads HI, LO, HI and retries on a mismatch, so both halves
     * can be sampled live. */
    case PMGR_TICKS_LO:
        return (uint32_t)pmgr_ticks(s);
    case PMGR_TICKS_HI:
        return pmgr_ticks(s) >> 32;
    case PMGR_EVT_COUNT(0):
    case PMGR_EVT_COUNT(1): {
        S5L8930EventTimer *t = &s->evt[(off - PMGR_EVT_COUNT(0)) / 4];
        return (t->state & EVT_STATE_START) ? evt_remaining(s, t) : t->count;
    }
    case PMGR_EVT_STATE(0):
    case PMGR_EVT_STATE(1):
        return s->evt[(off - PMGR_EVT_STATE(0)) / 4].state;
    case PMGR_CLOCK_CON1:
        return val & ~CLOCK_CON1_BUSY;
    }

    if (off < PLL_END && !(off & 4)) {
        return (val & PLL_CON0_ENABLE) ? val | PLL_CON0_LOCKED
                                       : val & ~PLL_CON0_LOCKED;
    }
    if (off >= CLKCFG_START && off < CLKCFG_END) {
        return val & ~CLKCFG_BUSY;
    }
    if (off >= DPSM_START && off < DPSM_END) {
        return val & ~DPSM_BUSY;
    }
    return val;
}

static void s5l8930_pmgr_write(void *opaque, hwaddr off, uint64_t val64,
                               unsigned size)
{
    S5L8930PMGRState *s = opaque;
    uint32_t val = val64;

    if (!pmgr_modelled(off)) {
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled write 0x%04x <- 0x%08x\n",
                      __func__, (unsigned)off, val);
        s->regs[off / 4] = val;
        return;
    }

    switch (off) {
    case PMGR_TICKS_LO:
    case PMGR_TICKS_HI:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to timebase 0x%04x\n",
                      __func__, (unsigned)off);
        return;
    case PMGR_EVT_COUNT(0):
    case PMGR_EVT_COUNT(1): {
        /* rtclock's set_decrementer writes only this while running, so it
         * has to restart the countdown by itself. */
        S5L8930EventTimer *t = &s->evt[(off - PMGR_EVT_COUNT(0)) / 4];
        t->count = val;
        if (!t->pending) {
            evt_arm(s, t, val);
        }
        return;
    }
    case PMGR_EVT_STATE(0):
    case PMGR_EVT_STATE(1):
        evt_write_state(s, &s->evt[(off - PMGR_EVT_STATE(0)) / 4], val);
        return;
    case PMGR_WDOG_TMR:
    case PMGR_WDOG_RST:
    case PMGR_WDOG_CTL:
        s->regs[off / 4] = val;
        wdog_check(s);
        return;
    }

    if (getenv("S5L8930_PMGR_TRACE") && off < GATE_START) {
        fprintf(stderr, "[PMGR] W %04x <- %08x\n", (unsigned)off, val);
    }
    if (off < PLL_END && !(off & 4)) {
        val &= ~PLL_CON0_UPDATE;
    } else if (off >= GATE_START && off < GATE_END) {
        /* Settle at once: actual (4-7) follows request (0-3), and the
         * reset pulse is over as soon as it is written. */
        val = (val & ~(GATE_RESET | 0xF0)) | ((val & 0xF) << 4);
    }
    s->regs[off / 4] = val;
}

static const MemoryRegionOps s5l8930_pmgr_ops = {
    .read = s5l8930_pmgr_read,
    .write = s5l8930_pmgr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8930_pmgr_reset(DeviceState *dev)
{
    S5L8930PMGRState *s = S5L8930_PMGR(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < ARRAY_SIZE(pmgr_defaults); i++) {
        s->regs[pmgr_defaults[i].off / 4] = pmgr_defaults[i].val;
    }
    /* iboot= skips LLB, which writes the boot epoch here on hardware; the
     * machine hands over what that LLB would write (it_iboot_find_miu_epoch). */
    if (s->security_epoch) {
        s->regs[PMGR_POWER_ID / 4] = (s->regs[PMGR_POWER_ID / 4] & 0x00ffffff) |
                                     (uint32_t)s->security_epoch << 24;
    }
    /* Every clock source enabled and every gate on: the kernel builds its
     * enabled masks from these, and enabling a power gate panics unless its
     * clock gate already reads 0xF. */
    for (i = CLKCFG_START; i < CLKCFG_END; i += 4) {
        s->regs[i / 4] |= 0x80000000;
    }
    for (i = GATE_START; i < GATE_END; i += 4) {
        s->regs[i / 4] = 0xFF;
    }

    s->tick_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (i = 0; i < ARRAY_SIZE(s->evt); i++) {
        S5L8930EventTimer *t = &s->evt[i];

        timer_del(t->timer);
        qemu_irq_lower(t->irq);
        t->count = t->state = t->load = 0;
        t->start = 0;
        t->pending = false;
    }
}

static void s5l8930_pmgr_init(Object *obj)
{
    S5L8930PMGRState *s = S5L8930_PMGR(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &s5l8930_pmgr_ops, s,
                          TYPE_S5L8930_PMGR, S5L8930_PMGR_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (i = 0; i < ARRAY_SIZE(s->evt); i++) {
        sysbus_init_irq(sbd, &s->evt[i].irq);
        s->evt[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, evt_expire,
                                       &s->evt[i]);
    }
}

/* All times are QEMU_CLOCK_VIRTUAL, which migration carries, so the counter
 * and pending deadlines resume where they were. The IRQ line level lives
 * in the VIC's state. */
static const VMStateDescription vmstate_s5l8930_evt = {
    .name = "s5l8930.pmgr.evt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, S5L8930EventTimer),
        VMSTATE_UINT32(count, S5L8930EventTimer),
        VMSTATE_UINT32(state, S5L8930EventTimer),
        VMSTATE_UINT64(start, S5L8930EventTimer),
        VMSTATE_UINT32(load, S5L8930EventTimer),
        VMSTATE_BOOL(pending, S5L8930EventTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_s5l8930_pmgr = {
    .name = TYPE_S5L8930_PMGR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8930PMGRState, S5L8930_PMGR_SIZE / 4),
        VMSTATE_INT64(tick_base_ns, S5L8930PMGRState),
        VMSTATE_STRUCT_ARRAY(evt, S5L8930PMGRState, 2, 1,
                             vmstate_s5l8930_evt, S5L8930EventTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8930_pmgr_props[] = {
    DEFINE_PROP_UINT8("security-epoch", S5L8930PMGRState, security_epoch, 0),
};

static void s5l8930_pmgr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, s5l8930_pmgr_props);

    dc->vmsd = &vmstate_s5l8930_pmgr;
    device_class_set_legacy_reset(dc, s5l8930_pmgr_reset);
}

static const TypeInfo s5l8930_pmgr_info = {
    .name          = TYPE_S5L8930_PMGR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930PMGRState),
    .instance_init = s5l8930_pmgr_init,
    .class_init    = s5l8930_pmgr_class_init,
};

static void s5l8930_pmgr_register_types(void)
{
    type_register_static(&s5l8930_pmgr_info);
}

type_init(s5l8930_pmgr_register_types)
