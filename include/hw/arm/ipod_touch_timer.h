#ifndef IPOD_TOUCH_TIMER_H
#define IPOD_TOUCH_TIMER_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/clock.h"

#define TYPE_IPOD_TOUCH_TIMER                "ipodtouch.timer"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchTimerState, IPOD_TOUCH_TIMER)

#define TIMER_IRQSTAT 0x10000
#define TIMER_IRQLATCH 0x118   /* S5L8720; the S5L8900 latch is at 0xF8 ("irqlatch" property) */
#define TIMER_TICKSHIGH 0x80
#define TIMER_TICKSLOW 0x84
#define TIMER_STATE_START 1
#define TIMER_STATE_STOP 0
#define TIMER_STATE_MANUALUPDATE 2
#define NUM_TIMERS 7
/* Bounds the largest guest count * 100 ns * dilation below INT64_MAX. */
#define IT_TIMER_MAX_DILATION 1000000
#define TIMER_4 0xA0
#define TIMER_CONFIG 0 
#define TIMER_STATE 0x4
#define TIMER_COUNT_BUFFER 0x8
#define TIMER_COUNT_BUFFER2 0xC
#define TIMER_PRESCALER 0x10
#define TIMER_NUM_CHANNELS 4     /* timers 0-3 at 0x00/0x20/0x40/0x60 */
#define TIMER_STRIDE 0x20

/*
 * One of timers 0-3. The register meanings are the S5L8900 kernel's own
 * (AppleS5L8900XTimer's tone routine, 3A101a c049049c): CONFIG bits 10:8
 * pick a pre-divider of the input clock (0 /2, 1 /4, 2 /16, 3 /64), bits 5:4
 * the mode (2 = one shot), PRESCALER divides by n + 1, COUNT_BUFFER2 is the
 * period and COUNT_BUFFER the point in it where the output pin toggles. STATE
 * bit 1 latches the buffers, bit 0 runs.
 */
typedef struct IPodTouchTimerChannel {
    uint32_t config, state, cb, cb2, prescaler;
    uint32_t lcb, lcb2, lpre;    /* latched by STATE bit 1 */
    int64_t start_ns;            /* the START edge; 0 when stopped */
} IPodTouchTimerChannel;

/*
 * The output pin's waveform, as the timer is programmed from start_ns on:
 * high for high_ns of every period_ns, until end_ns (INT64_MAX while it runs;
 * start_ns + period_ns for a one shot). period_ns == 0: the pin is idle.
 */
typedef struct IPodTouchTimerOutput {
    int64_t start_ns, end_ns, period_ns, high_ns;
} IPodTouchTimerOutput;

typedef struct IPodTouchTimerState
{
    SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t    ticks_high;
    uint32_t    ticks_low;
    uint32_t    status;
    uint32_t    config;
    uint32_t    bcount1;
    uint32_t    bcount2;
    uint32_t    prescaler;
    uint32_t    irqstat;
    QEMUTimer *st_timer;
    Clock *sysclk;
    uint32_t bcreload;
    uint32_t freq_out;
    uint32_t dilation;
    uint32_t irqlatch;     /* "irqlatch" property */
    uint32_t freq_hz;      /* "freq-hz" property: timer-4 count rate, 0 = 10 MHz */
    /* Board hook run at every timer-4 CONFIG write. On the S5L8900 iBoot
     * writes it once at start and the kernel once at rtclock init, i.e. after
     * iBoot has left the device tree in RAM and before IOKit reads it. */
    void (*first_config_hook)(void *opaque);
    void *first_config_opaque;
    IPodTouchTimerChannel chan[TIMER_NUM_CHANNELS];
    uint32_t input_hz;     /* "input-hz" property: the timers' input clock (nclk) */
    /* Board hook run whenever a channel's output waveform changes (a piezo on it). */
    void (*output_hook)(void *opaque, unsigned ch, const IPodTouchTimerOutput *out);
    void *output_opaque;
    uint64_t tick_interval;
    uint64_t last_tick;
    uint64_t next_planned_tick;
    uint64_t base_time;
    qemu_irq    irq;

} IPodTouchTimerState;

#endif