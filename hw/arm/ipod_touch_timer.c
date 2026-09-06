#include "hw/arm/ipod_touch_timer.h"
#include "migration/vmstate.h"

/*
 * freq_out is 10 MHz here while the tick counter at TIMER_TICKSHIGH/LOW runs at
 * SYSCLK/2 = 6 MHz, and they are one hardware block -- so a count the guest
 * derives from that counter and loads into timer 4 expires after only 60% of
 * the time it asked for. The mismatch is real, and the guest's own counts say
 * so: its commonest non-trivial loads are 5945/5939/5933 and 119550, which are
 * 0.99 ms and 19.9 ms at 6 MHz (a 1 ms and a 20 ms deadline, less the time
 * already elapsed) and nothing in particular at 10 MHz. With 6 MHz the guest
 * also re-arms 19% less often, which is what you would expect if the early
 * expiries were being retried.
 *
 * IT DOES NOT FIX ANYTHING VISIBLE, AND IT MADE FRAME DELIVERY WORSE. Measured
 * (2026-08-03, six app-launch/close transitions per arm, paired and
 * interleaved, identical warm overlay, host load 9-11): frames landing within
 * +/-2 ms of the 16.67 ms cadence fell from 93.4%/92.1% to 85.5%/84.1%, and
 * frame interrupts more than 1 ms late rose from 10.0%/13.4% to 18.1%/17.1%.
 * The guest's surplus early wakeups appear to keep QEMU's main loop responsive;
 * take them away and the vsync timer is dispatched later and more raggedly.
 * So the correct clock is left uncorrected on purpose, with the evidence
 * written down, rather than traded for measurably worse animation. Anyone
 * fixing it properly needs to deal with the dispatch latency first.
 */
/*
 * Cached, because the second caller sits on the counter-read path: this file's
 * own accounting records 8.24 million reads of TICKSHIGH/TICKSLOW in one boot,
 * and getenv() scans environ on every one of them -- synchronously on the vCPU
 * thread, so it is guest stall rather than background work. Same pattern as
 * FMSS_ENV_FLAG in ipod_touch_fmss.c.
 */
static bool timer_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_TIMER_TRACE") != NULL;
    }
    return on;
}

/* Dilation scales timer-4 interrupt intervals, preserving the existing counter
 * rate and default scheduling. It is bounded startup configuration. */
static void s5l8900_st_update(IPodTouchTimerState *s)
{
    s->freq_out = 1000000000 / 100;
    s->tick_interval = /* bcount1 * get_ticks / freq  + ((bcount2 * get_ticks / freq)*/
    muldiv64((s->bcount1 < 1000) ? 1000 : s->bcount1, NANOSECONDS_PER_SECOND, s->freq_out);
    s->tick_interval *= s->dilation;
    s->next_planned_tick = 0;
    if (timer_trace()) {
        fprintf(stderr, "[TIMER] update: bcount1=%u bcount2=%u freq_out=%u "
                "-> tick_interval=%" PRIu64 " ns (%.3f Hz)\n",
                s->bcount1, s->bcount2, s->freq_out, s->tick_interval,
                s->tick_interval ? 1e9 / (double)s->tick_interval : 0.0);
    }
}

static void s5l8900_st_set_timer(IPodTouchTimerState *s)
{
    uint64_t last = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->base_time;

    s->next_planned_tick = MIN((uint64_t)INT64_MAX - s->base_time,
                              last + (s->tick_interval - last % s->tick_interval));
    timer_mod(s->st_timer, s->next_planned_tick + s->base_time);
    s->last_tick = last;
}

static void s5l8900_st_tick(void *opaque)
{
    IPodTouchTimerState *s = (IPodTouchTimerState *)opaque;

    if (s->status & TIMER_STATE_START) {
        //fprintf(stderr, "%s: Raising irq\n", __func__);
        qemu_irq_raise(s->irq);

        /* schedule next interrupt */
        if(!(s->status & TIMER_STATE_MANUALUPDATE)) {
            s5l8900_st_set_timer(s);
        }
    } else {
        s->next_planned_tick = 0;
        s->last_tick = 0;
        timer_del(s->st_timer);
    }
}

static void s5l8900_timer1_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    IPodTouchTimerState *s = (struct IPodTouchTimerState *) opaque;

    switch(addr){

        case TIMER_IRQSTAT:
            s->irqstat = value;
            return;
        case TIMER_IRQLATCH:
            //fprintf(stderr, "%s: lowering irq\n", __func__);
            qemu_irq_lower(s->irq);     
            return;
        case TIMER_4 + TIMER_CONFIG:
            s5l8900_st_update(s);
            s->config = value;
            break;
        case TIMER_4 + TIMER_STATE:
            if (value & TIMER_STATE_START) {
                s->base_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                s5l8900_st_update(s);
                s5l8900_st_set_timer(s);
            } else if (value == TIMER_STATE_STOP) {
                timer_del(s->st_timer);
            }
            s->status = value;
            break;
        case TIMER_4 + TIMER_COUNT_BUFFER:
            s->bcount1 = s->bcreload = value;
            break;
        case TIMER_4 + TIMER_COUNT_BUFFER2:
            s->bcount2 = value;
            break;
      default:
        /*
         * Only timer 4 is decoded (block base TIMER_4 = 0xA0, stride 0x20, so
         * 0x20/0x40/0x60/0x80 are timers 0-3). A guest arming one of those got
         * no interrupt AND no diagnostic, which is the combination that makes
         * a missing timer indistinguishable from a guest bug: the deadline
         * simply never arrives and nothing anywhere says why. Say it once per
         * timer -- once, because this is an MMIO write handler.
         */
        if (addr >= 0x20 && addr < TIMER_4) {
            static uint32_t said;
            unsigned n = (unsigned)((addr - 0x20) / 0x20);

            if (!(said & (1u << n))) {
                said |= 1u << n;
                fprintf(stderr, "[TIMER] guest touched UNMODELLED timer %u "
                        "(reg 0x%03x <- 0x%08x); no interrupt can ever be "
                        "delivered from it\n",
                        n, (unsigned)addr, (unsigned)value);
            }
        }
        break;
    }
}

/*
 * IT_TIMER_TRACE: is the guest's 64-bit counter read ever torn?
 *
 * TIMER_TICKSHIGH latches BOTH halves as a side effect of reading high, so a
 * guest that reads LOW first and HIGH second recombines two words sampled at
 * different instants -- and one that reads only LOW gets a word from whenever
 * HIGH was last read, which on a free-running counter is arbitrarily stale.
 * Either would make every deadline the guest computes off mach_absolute_time
 * unreliable. Rather than assume, count it: `torn` is every read of LOW that
 * was not immediately preceded by a read of HIGH.
 *
 * ANSWER: never. 8.24 million counter reads across a boot, an unlock and a run
 * of home-screen transitions on 3.1.3 gave torn=0, in a steady 2:1 ratio of
 * high to low -- the guest reads high, low, high, the classic rollover-safe
 * idiom, and since reading high latches both halves the pair it gets is always
 * coherent. The latch is ugly but it is not a source of bad guest timekeeping.
 */
static void timer_count_order(bool high)
{
    static bool prev_high;
    static uint64_t nhigh, nlow, torn;

    if (high) {
        nhigh++;
    } else {
        nlow++;
        if (!prev_high) {
            torn++;
        }
    }
    prev_high = high;
    if (((nhigh + nlow) % 20000) == 0) {
        fprintf(stderr, "[TIMER] tickshigh=%" PRIu64 " tickslow=%" PRIu64
                " torn=%" PRIu64 "\n", nhigh, nlow, torn);
    }
}

static uint64_t s5l8900_timer1_read(void *opaque, hwaddr addr, unsigned size)
{
    //fprintf(stderr, "%s: read from location 0x%08x\n", __func__, addr);
    IPodTouchTimerState *s = (struct IPodTouchTimerState *) opaque;
    uint64_t elapsed_ns, ticks;

    if (timer_trace() &&
        (addr == TIMER_TICKSHIGH || addr == TIMER_TICKSLOW)) {
        timer_count_order(addr == TIMER_TICKSHIGH);
    }

    switch (addr) {
        case TIMER_TICKSHIGH:    // needs to be fixed so that read from low first works as well

            /* SYSCLK / 2 = 6 MHz. NOT the rate freq_out counts the timer-4
             * deadline down at; see the note above s5l8900_st_update(). */
            elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 2;
            ticks = clock_ns_to_ticks(s->sysclk, elapsed_ns);
            //printf("TICKS: %lld\n", ticks);
            s->ticks_high = (ticks >> 32);
            s->ticks_low = (ticks & 0xFFFFFFFF);
            return s->ticks_high;
        case TIMER_TICKSLOW:
            return s->ticks_low;
        case TIMER_IRQSTAT:
            return s->irqstat; // ~0; // s->irqstat;
        case TIMER_IRQLATCH:
            return 0xffffffff;

      default:
        break;
    }
    return 0;
}

static const MemoryRegionOps timer1_ops = {
    .read = s5l8900_timer1_read,
    .write = s5l8900_timer1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_timer_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchTimerState *s = IPOD_TOUCH_TIMER(dev);

    memory_region_init_io(&s->iomem, obj, &timer1_ops, s, "timer1", 0x10001);
    sysbus_init_irq(sbd, &s->irq);

    s->base_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->dilation = 1;
    s->st_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, s5l8900_st_tick, s);
}

/*
 * A second boot used to inherit the first boot's counter base, so the guest's
 * very first read of the tick counter returned a value from before its own
 * reset vector ran. base_time is re-taken here; the periodic tick is disarmed
 * because the guest re-programs bcount/prescaler before re-enabling it.
 */
static void ipod_touch_timer_reset(DeviceState *dev)
{
    IPodTouchTimerState *s = IPOD_TOUCH_TIMER(dev);

    s->ticks_high = 0;
    s->ticks_low = 0;
    s->status = 0;
    s->config = 0;
    s->bcount1 = 0;
    s->bcount2 = 0;
    s->prescaler = 0;
    s->irqstat = 0;
    s->bcreload = 0;
    s->tick_interval = 0;
    s->last_tick = 0;
    s->next_planned_tick = 0;
    s->base_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->st_timer) {
        timer_del(s->st_timer);
    }
    if (s->irq) {
        qemu_irq_lower(s->irq);
    }
}

/* st_timer carries its own expiry through VMSTATE_TIMER_PTR; the tick
 * bookkeeping below is all in QEMU_CLOCK_VIRTUAL nanoseconds, which migration
 * carries too, so the guest's notion of elapsed time survives the restore.
 * sysclk is a Clock wired by the machine and is not per-snapshot state. */
static int ipod_touch_timer_post_load(void *opaque, int version_id)
{
    IPodTouchTimerState *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->base_time > now || s->tick_interval > INT64_MAX ||
        s->next_planned_tick > (uint64_t)INT64_MAX - s->base_time ||
        ((s->status & TIMER_STATE_START) && !s->tick_interval)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_timer = {
    .name = "ipod_touch_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ipod_touch_timer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ticks_high, IPodTouchTimerState),
        VMSTATE_UINT32(ticks_low, IPodTouchTimerState),
        VMSTATE_UINT32(status, IPodTouchTimerState),
        VMSTATE_UINT32(config, IPodTouchTimerState),
        VMSTATE_UINT32(bcount1, IPodTouchTimerState),
        VMSTATE_UINT32(bcount2, IPodTouchTimerState),
        VMSTATE_UINT32(prescaler, IPodTouchTimerState),
        VMSTATE_UINT32(irqstat, IPodTouchTimerState),
        VMSTATE_UINT32(bcreload, IPodTouchTimerState),
        VMSTATE_UINT32(freq_out, IPodTouchTimerState),
        VMSTATE_UINT64(tick_interval, IPodTouchTimerState),
        VMSTATE_UINT64(last_tick, IPodTouchTimerState),
        VMSTATE_UINT64(next_planned_tick, IPodTouchTimerState),
        VMSTATE_UINT64(base_time, IPodTouchTimerState),
        VMSTATE_TIMER_PTR(st_timer, IPodTouchTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_timer;
    device_class_set_legacy_reset(dc, ipod_touch_timer_reset);

}

static const TypeInfo ipod_touch_timer_info = {
    .name          = TYPE_IPOD_TOUCH_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchTimerState),
    .instance_init = s5l8900_timer_init,
    .class_init    = s5l8900_timer_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_timer_info);
}

type_init(ipod_touch_machine_types)
