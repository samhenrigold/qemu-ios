/*
 * S5L8920/8922 PMGR (DT arm-io/pmgr, pmgr,s5l8920x, 0xbf100000 + 0x2000):
 * clocks and gates, plus the 24 MHz timebase and event timer the kernel's
 * pe_arm_init_timer finds here (the node is device_type "timer").
 *
 * Not the A4's map (s5l8930_pmgr.c has the timer at +0x2000; this window
 * is only 0x2000 long). Offsets are what the N18 8C148 kernel touches;
 * docs/n18/README.md has the derivation.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "target/arm/cpu.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_S5L8920_PMGR "s5l8920.pmgr"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8920PMGRState, S5L8920_PMGR)

#define PMGR_SIZE           0x2000
#define PMGR_TIMEBASE_HZ    24000000

#define PMGR_TICKS_LO       0x200
#define PMGR_TICKS_HI       0x204
#define PMGR_EVT_COUNT(n)   (0x208 + 4 * (n))
#define PMGR_EVT_STATE(n)   (0x220 + 4 * (n))
#define EVT_STATE_START     (1u << 0)
#define EVT_STATE_UPDATE    (1u << 1)

/* Two event timers: the AP's rtclock uses 0 (IRQ 6), the IOP firmware 1 (IRQ 5). */
typedef struct S5L8920EventTimer {
    struct S5L8920PMGRState *s;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t count, state, load;
    uint64_t start;
    bool pending;
} S5L8920EventTimer;

struct S5L8920PMGRState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t regs[PMGR_SIZE / 4];
    int64_t tick_base_ns;
    bool keep_timebase;         /* "keep-timebase": the next reset is the AP's wake, see reset */
    S5L8920EventTimer evt[2];
};

static uint64_t pmgr_ticks(S5L8920PMGRState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->tick_base_ns,
                    PMGR_TIMEBASE_HZ, NANOSECONDS_PER_SECOND);
}

static uint32_t evt_remaining(S5L8920EventTimer *t)
{
    uint64_t elapsed = pmgr_ticks(t->s) - t->start;

    return elapsed < t->load ? t->load - elapsed : 0;
}

static void evt_arm(S5L8920EventTimer *t, uint32_t load)
{
    t->start = pmgr_ticks(t->s);
    t->load = load;
    if (!(t->state & EVT_STATE_START)) {
        timer_del(t->timer);
        return;
    }
    timer_mod(t->timer, t->s->tick_base_ns + 1 +
              muldiv64(t->start + load, NANOSECONDS_PER_SECOND, PMGR_TIMEBASE_HZ));
}

/* Expiry asserts the line and stops until a STATE write with UPDATE acks it. */
static void evt_expire(void *opaque)
{
    S5L8920EventTimer *t = opaque;

    t->pending = true;
    qemu_irq_raise(t->irq);
}

static void pmgr_log(const char *what, hwaddr off, uint32_t val)
{
    qemu_log_mask(LOG_UNIMP, "s5l8920_pmgr: %s 0x%04x 0x%08x pc 0x%08x\n", what,
                  (unsigned)off, val, current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
}

static uint64_t s5l8920_pmgr_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8920PMGRState *s = opaque;

    switch (off) {
    case PMGR_TICKS_LO:
        return (uint32_t)pmgr_ticks(s);
    case PMGR_TICKS_HI:
        return pmgr_ticks(s) >> 32;
    case PMGR_EVT_COUNT(0):
    case PMGR_EVT_COUNT(1): {
        S5L8920EventTimer *t = &s->evt[(off - PMGR_EVT_COUNT(0)) / 4];
        return (t->state & EVT_STATE_START) ? evt_remaining(t) : t->count;
    }
    case PMGR_EVT_STATE(0):
    case PMGR_EVT_STATE(1):
        return s->evt[(off - PMGR_EVT_STATE(0)) / 4].state;
    }
    pmgr_log("read", off, s->regs[off / 4]);
    return s->regs[off / 4];
}

static void s5l8920_pmgr_write(void *opaque, hwaddr off, uint64_t val64, unsigned size)
{
    S5L8920PMGRState *s = opaque;
    uint32_t val = val64;

    switch (off) {
    case PMGR_EVT_COUNT(0):
    case PMGR_EVT_COUNT(1): {
        S5L8920EventTimer *t = &s->evt[(off - PMGR_EVT_COUNT(0)) / 4];
        t->count = val;
        if (!t->pending) {
            evt_arm(t, val);
        }
        return;
    }
    case PMGR_EVT_STATE(0):
    case PMGR_EVT_STATE(1): {
        S5L8920EventTimer *t = &s->evt[(off - PMGR_EVT_STATE(0)) / 4];
        t->state = val;
        if (val & EVT_STATE_UPDATE) {
            t->pending = false;
            qemu_irq_lower(t->irq);
            evt_arm(t, t->count);
        } else if (!(val & EVT_STATE_START)) {
            timer_del(t->timer);
        } else if (!t->pending && !timer_pending(t->timer)) {
            evt_arm(t, evt_remaining(t));
        }
        return;
    }
    }
    pmgr_log("write", off, val);
    s->regs[off / 4] = val;
}

static const MemoryRegionOps s5l8920_pmgr_ops = {
    .read = s5l8920_pmgr_read,
    .write = s5l8920_pmgr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8920_pmgr_reset(DeviceState *dev)
{
    S5L8920PMGRState *s = S5L8920_PMGR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /*
     * The 24 MHz timebase keeps counting across the AP's hibernate: xnu's
     * mach_absolute_time must not go back, or every timer armed before the sleep
     * (launchd's intervals, configd's ThermalMonitor) waits until the counter
     * catches up with the uptime at sleep. The machine sets "keep-timebase"
     * before the wake's reset (kboot boards; no LLB restores it).
     */
    if (!s->keep_timebase) {
        s->tick_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->keep_timebase = false;
    for (int i = 0; i < ARRAY_SIZE(s->evt); i++) {
        S5L8920EventTimer *t = &s->evt[i];

        timer_del(t->timer);
        qemu_irq_lower(t->irq);
        t->count = t->state = t->load = 0;
        t->start = 0;
        t->pending = false;
    }
}

static bool pmgr_get_keep_timebase(Object *obj, Error **errp)
{
    return (((S5L8920PMGRState *)obj))->keep_timebase;
}

static void pmgr_set_keep_timebase(Object *obj, bool value, Error **errp)
{
    ((S5L8920PMGRState *)obj)->keep_timebase = value;
}

static void s5l8920_pmgr_init(Object *obj)
{
    object_property_add_bool(obj, "keep-timebase", pmgr_get_keep_timebase, pmgr_set_keep_timebase);
    S5L8920PMGRState *s = S5L8920_PMGR(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8920_pmgr_ops, s, TYPE_S5L8920_PMGR, PMGR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (int i = 0; i < ARRAY_SIZE(s->evt); i++) {
        s->evt[i].s = s;
        s->evt[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, evt_expire, &s->evt[i]);
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->evt[i].irq);
    }
}

static const VMStateDescription vmstate_s5l8920_evt = {
    .name = "s5l8920.pmgr.evt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, S5L8920EventTimer),
        VMSTATE_UINT32(count, S5L8920EventTimer),
        VMSTATE_UINT32(state, S5L8920EventTimer),
        VMSTATE_UINT32(load, S5L8920EventTimer),
        VMSTATE_UINT64(start, S5L8920EventTimer),
        VMSTATE_BOOL(pending, S5L8920EventTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_s5l8920_pmgr = {
    .name = TYPE_S5L8920_PMGR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8920PMGRState, PMGR_SIZE / 4),
        VMSTATE_INT64(tick_base_ns, S5L8920PMGRState),
        VMSTATE_STRUCT_ARRAY(evt, S5L8920PMGRState, 2, 1, vmstate_s5l8920_evt,
                             S5L8920EventTimer),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8920_pmgr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8920_pmgr;
    device_class_set_legacy_reset(dc, s5l8920_pmgr_reset);
}

static const TypeInfo s5l8920_pmgr_info = {
    .name          = TYPE_S5L8920_PMGR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8920PMGRState),
    .instance_init = s5l8920_pmgr_init,
    .class_init    = s5l8920_pmgr_class_init,
};

static void s5l8920_pmgr_register_types(void)
{
    type_register_static(&s5l8920_pmgr_info);
}

type_init(s5l8920_pmgr_register_types)
