#ifndef IPOD_TOUCH_I2S_H
#define IPOD_TOUCH_I2S_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "audio/audio.h"
#include "qemu/timer.h"
#include "hw/arm/ipod_touch_sysic.h"
#include "hw/dma/pl080.h"

/*
 * S5L8720 I2S controller (AppleS5L8900XI2SController).
 *
 * Register window is 0x1000; the driver only ever touches nine offsets, all
 * through two trivial accessors, and never polls (no reads on the hot path):
 *
 *   0x00  block/clock enable (bit 0)
 *   0x04  TX config           (opaque, computed in software)
 *   0x08  TX command          (6 = run, 0 = halt)
 *   0x10  TX FIFO             (DMA target - PCM lands here)
 *   0x30  RX config           (opaque)
 *   0x34  RX command
 *   0x38  RX FIFO
 *   0x3C  TX FIFO/status ctrl (writes 1)
 *   0x40  clock divider / format (opaque)
 *
 * The config words are all computed in software, so the model honours only the
 * enable bit, the TX command values, and the TX FIFO; everything else is stored
 * and echoed back.
 */

#define IT_I2S_ENABLE   0x00
#define IT_I2S_TXCON    0x04
#define IT_I2S_TXCOM    0x08
#define IT_I2S_TXFIFO   0x10
#define IT_I2S_RXCON    0x30
#define IT_I2S_RXCOM    0x34
#define IT_I2S_RXFIFO   0x38
#define IT_I2S_TXFCTL   0x3C
#define IT_I2S_CLKDIV   0x40

#define IT_I2S_CMD_RUN  6
#define IT_I2S_CMD_HALT 0

/*
 * i2s0's `interrupts` property is <0x2c> and its interrupt-parent is the GPIO
 * interrupt controller, not the VIC -- the same numbering the multi-touch
 * (0x6d -> group 3, bit 13) uses. 0x2c is therefore GPIO group 1, bit 12.
 *
 * It is not optional. AppleS5L8720X's DMA-channel start (c0565928 in the
 * running 3.1.3 kernel) enables this very interrupt source and then sleeps on
 * it for the [req+0x68] deadline -- 10 s -- before it will program a PL080
 * channel. Measured with a gdbstub breakpoint: the sleep always ends 10.00 s
 * later in the IOTimerEventSource handler, which sets the wait state to 4, and
 * the start routine then returns its preloaded default of kIOReturnNotReady
 * (0xE00002D8). That is the whole reason no channel is ever pointed at the TX
 * FIFO and the whole reason the device is silent.
 */
#define IT_I2S_GPIO_INT_GROUP 1
#define IT_I2S_GPIO_INT_BIT   12

/* PCM ring in bytes. ~6 s at 44.1 kHz/16-bit/stereo; the DMA free-runs into it
 * in per-buffer bursts and the audio backend drains it, so this only needs to
 * absorb scheduling jitter, not a whole clip. */
#define IT_I2S_RING_SIZE (1 << 20)

/*
 * TX FIFO pacing (see it_i2s_pace_cb).
 *
 * The real block clocks samples out at the bit clock and asserts its DMA
 * request line whenever the TX FIFO has room; the PL080 refills it a burst at a
 * time. Our PL080 used to ignore the request line, so a channel start drained
 * the guest's entire audio ring instantly -- and the guest's audio stack fills
 * that ring in real time, ahead of the play position, so everything we
 * delivered was the silence the ring had been allocated with.
 *
 * The real TX FIFO is a handful of samples; that is far too tight for a host
 * timer, so we model a deeper one. IT_I2S_FIFO_BYTES overrides it. What matters
 * is only that it is small compared to the ring (72 KB), so the transfer takes
 * roughly its own real-time duration rather than none at all.
 */
#define IT_I2S_FIFO_BYTES_DEFAULT 8192
#define IT_I2S_PACE_PERIOD_NS     (2 * 1000 * 1000)

#define TYPE_IPOD_TOUCH_I2S "ipodtouch.i2s"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchI2SState, IPOD_TOUCH_I2S)

typedef struct IPodTouchI2SState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /* The GPIO interrupt controller this device's interrupt is routed through;
     * see IT_I2S_GPIO_INT_GROUP. Set by the machine at init. */
    IPodTouchSYSICState *sysic;
    QEMUTimer *ready_timer;
    uint64_t ready_irqs;  /* how many ready interrupts we have asserted */
    uint32_t ready_ticks; /* ticks left in the current ready-interrupt burst */

    uint32_t enable;
    uint32_t txcon;
    uint32_t txcom;
    uint32_t rxcon;
    uint32_t rxcom;
    uint32_t txfctl;
    uint32_t clkdiv;

    QEMUSoundCard card;
    SWVoiceOut *voice;
    struct audsettings as;

    uint8_t ring[IT_I2S_RING_SIZE];
    uint32_t ring_head;   /* write cursor */
    uint32_t ring_tail;   /* read cursor  */
    uint32_t ring_level;  /* bytes queued */

    bool card_ok;         /* AUD_register_card succeeded */
    bool active;          /* SWVoiceOut is active */
    bool running;         /* TX command == run */

    /* TX FIFO pacing: the DMA request line back to the PL080 that feeds us. */
    struct PL080State *dmac;  /* dmac0, set by the machine at init */
    int dma_req_id;           /* peripheral request line on it (10) */
    bool dma_req;             /* current level of that line */
    QEMUTimer *pace_timer;
    uint32_t fifo_bytes;      /* modelled TX FIFO occupancy */
    uint32_t fifo_depth;      /* IT_I2S_FIFO_BYTES */
    int64_t pace_last_ns;     /* when we last drained the modelled FIFO */

    FILE *dump;           /* IT_I2S_DUMP: raw s16le stereo tap of the FIFO */
    uint64_t total_bytes; /* lifetime PCM bytes seen at the FIFO (debug) */
    uint64_t dropped;     /* bytes dropped on ring overflow (debug) */
} IPodTouchI2SState;

#endif
