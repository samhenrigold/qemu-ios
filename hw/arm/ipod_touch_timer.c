#include "hw/arm/ipod_touch_timer.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"

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
    s->freq_out = s->freq_hz ? s->freq_hz : 1000000000 / 100;
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
        if (timer_trace()) {
            fprintf(stderr, "[TIMER] fire at %" PRId64 " ns (planned %" PRIu64 ")\n",
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->next_planned_tick + s->base_time);
        }
        qemu_irq_raise(s->irq);

        /*
         * The timer reloads from its count buffer and keeps interrupting until
         * the guest re-arms or stops it. MANUALUPDATE (bit 1) only latches the
         * buffer at START; it is not a one-shot mode. xnu-1504 (4.2.1) arms
         * with START|MANUALUPDATE and, after the FIQ, arms again only for a
         * deadline sooner than the running period: as a one-shot the timer went
         * quiet and no timed sleep ever woke (the FMSS driver's IOSleep(10)).
         */
        s5l8900_st_set_timer(s);
    } else {
        s->next_planned_tick = 0;
        s->last_tick = 0;
        timer_del(s->st_timer);
    }
}

/* The pin waveform a channel's latched registers give from start_ns on. */
static IPodTouchTimerOutput timer_channel_output(const IPodTouchTimerChannel *c,
                                                 uint32_t input_hz)
{
    static const uint8_t prediv[8] = { 2, 4, 16, 64, 1, 1, 1, 1 };
    IPodTouchTimerOutput o = { 0 };

    if (!c->start_ns || !c->lcb2 || !input_hz) {
        return o;
    }
    uint64_t ticks = (uint64_t)prediv[(c->config >> 8) & 7] * (c->lpre + 1);
    o.start_ns = c->start_ns;
    o.period_ns = muldiv64(ticks * c->lcb2, NANOSECONDS_PER_SECOND, input_hz);
    o.high_ns = muldiv64(ticks * MIN(c->lcb, c->lcb2), NANOSECONDS_PER_SECOND, input_hz);
    o.end_ns = ((c->config >> 4) & 3) == 2 ? o.start_ns + o.period_ns : INT64_MAX;
    return o;
}

static void timer_channel_write(IPodTouchTimerChannel *c, hwaddr reg,
                                uint32_t value, int64_t now)
{
    switch (reg) {
    case TIMER_CONFIG:
        c->config = value;
        break;
    case TIMER_STATE:
        if (value & TIMER_STATE_MANUALUPDATE) {
            c->lcb = c->cb;
            c->lcb2 = c->cb2;
            c->lpre = c->prescaler;
        }
        if (!(value & TIMER_STATE_START)) {
            c->start_ns = 0;
        } else if (!c->start_ns) {
            c->start_ns = now ? now : 1;
        }
        c->state = value;
        break;
    case TIMER_COUNT_BUFFER:
        c->cb = value;
        break;
    case TIMER_COUNT_BUFFER2:
        c->cb2 = value;
        break;
    case TIMER_PRESCALER:
        c->prescaler = value;
        break;
    }
}

static uint32_t timer_channel_read(const IPodTouchTimerChannel *c, hwaddr reg)
{
    switch (reg) {
    case TIMER_CONFIG:        return c->config;
    case TIMER_STATE:         return c->state;
    case TIMER_COUNT_BUFFER:  return c->cb;
    case TIMER_COUNT_BUFFER2: return c->cb2;
    case TIMER_PRESCALER:     return c->prescaler;
    }
    return 0;
}

static void s5l8900_timer1_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    IPodTouchTimerState *s = (struct IPodTouchTimerState *) opaque;

    if (timer_trace() && addr != s->irqlatch) {
        fprintf(stderr, "[TIMER] W 0x%03x <- 0x%08x at %" PRId64 " ns\n",
                (unsigned)addr, (unsigned)value, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    /* The interrupt-latch register moved between SoCs ("irqlatch" property:
     * 0xF8 on the S5L8900, 0x118 on the S5L8720). */
    if (addr == s->irqlatch) {
        qemu_irq_lower(s->irq);
        return;
    }

    switch(addr){

        case TIMER_IRQSTAT:
            s->irqstat = value;
            return;
        case TIMER_4 + TIMER_CONFIG:
            if (s->first_config_hook) {
                s->first_config_hook(s->first_config_opaque);
            }
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
         * Timers 0-3 (0x00-0x7f; 0x80 is the 64-bit counter block): registers
         * and the output pin, which is what the S5L8900 kernel uses them for
         * (timer 1 is the N45's buzzer, the DT's timer/buzzer, device_type
         * pwm). Their interrupts are not modeled: no kernel seen enables one
         * (3A101a only ever writes STATE 0 at boot and drives timer 1 as PWM).
         */
        if (addr < TIMER_NUM_CHANNELS * TIMER_STRIDE) {
            unsigned n = addr / TIMER_STRIDE;
            IPodTouchTimerChannel *c = &s->chan[n];

            timer_channel_write(c, addr % TIMER_STRIDE, value,
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            if (addr % TIMER_STRIDE == TIMER_STATE && s->output_hook) {
                IPodTouchTimerOutput o = timer_channel_output(c, s->input_hz);
                s->output_hook(s->output_opaque, n, &o);
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
      default:
        if (addr == s->irqlatch) {
            return 0xffffffff;
        }
        if (addr < TIMER_NUM_CHANNELS * TIMER_STRIDE) {
            return timer_channel_read(&s->chan[addr / TIMER_STRIDE], addr % TIMER_STRIDE);
        }
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
    memset(s->chan, 0, sizeof(s->chan));
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

static const VMStateDescription vmstate_timer_channel = {
    .name = "ipod_touch_timer/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(config, IPodTouchTimerChannel),
        VMSTATE_UINT32(state, IPodTouchTimerChannel),
        VMSTATE_UINT32(cb, IPodTouchTimerChannel),
        VMSTATE_UINT32(cb2, IPodTouchTimerChannel),
        VMSTATE_UINT32(prescaler, IPodTouchTimerChannel),
        VMSTATE_UINT32(lcb, IPodTouchTimerChannel),
        VMSTATE_UINT32(lcb2, IPodTouchTimerChannel),
        VMSTATE_UINT32(lpre, IPodTouchTimerChannel),
        VMSTATE_INT64(start_ns, IPodTouchTimerChannel),
        VMSTATE_END_OF_LIST()
    }
};

/* Only when a guest has written a channel, so older snapshots (and boards
 * whose guest never does) keep loading and saving as before. */
static bool timer_channels_needed(void *opaque)
{
    IPodTouchTimerState *s = opaque;
    static const IPodTouchTimerChannel zero;

    for (int i = 0; i < TIMER_NUM_CHANNELS; i++) {
        if (memcmp(&s->chan[i], &zero, sizeof(zero))) {
            return true;
        }
    }
    return false;
}

static const VMStateDescription vmstate_timer_channels = {
    .name = "ipod_touch_timer/channels",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = timer_channels_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(chan, IPodTouchTimerState, TIMER_NUM_CHANNELS, 1,
                             vmstate_timer_channel, IPodTouchTimerChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ipod_touch_timer = {
    .name = "ipod_touch_timer",
    .version_id = 2,
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
        /* Reprogramming must use the same multiplier as the saved interval. */
        VMSTATE_UINT32_EQUAL_V(dilation, IPodTouchTimerState, 2,
                               "time-dilation differs from snapshot"),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_timer_channels,
        NULL
    }
};

static const Property ipod_touch_timer_properties[] = {
    DEFINE_PROP_UINT32("irqlatch", IPodTouchTimerState, irqlatch, TIMER_IRQLATCH),
    /* Rate timer 4 counts down at. 0 keeps the S5L8720 model's 10 MHz; the
     * S5L8900 kernel (xnu-933) loads 120000 for its 10 ms tick, i.e. 12 MHz. */
    DEFINE_PROP_UINT32("freq-hz", IPodTouchTimerState, freq_hz, 0),
    /* Timers 0-3's input clock: the nclk the guest computes (N45 24 MHz:
     * 3A101a loads 6382 at /2 for Celestial's 1880 Hz key click). 0 = no
     * output waveform. */
    DEFINE_PROP_UINT32("input-hz", IPodTouchTimerState, input_hz, 0),
};

static void s5l8900_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_timer;
    device_class_set_props(dc, ipod_touch_timer_properties);
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
