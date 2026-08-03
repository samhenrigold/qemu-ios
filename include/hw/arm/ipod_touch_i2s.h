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
 * timer, so we model a deeper one. IT_I2S_FIFO_BYTES overrides it.
 *
 * THE DEPTH IS AUDIBLE, so do not raise it casually. It is exactly how far the
 * DMA's read head runs ahead of the sound leaving the device, and the guest's
 * audio engine refills the ring in real time just behind that head. At 8192
 * bytes (46 ms, two of the guest's 4096-byte periods) the head sits on top of
 * the producer: measured, single samples were read torn out of a page the
 * engine was still writing, and after a clip ended the engine's silence-fill
 * lost the race and whole pages of the clip were played a second time. At 2048
 * (11.6 ms) the shutter comes out sample-exact, six times out of six.
 *
 * The floor is the pace timer: one tick must be refillable, i.e. the depth must
 * comfortably exceed IT_I2S_PACE_PERIOD_NS worth of audio (2 ms = 352 bytes).
 */
/*
 * Host-side prebuffer, in bytes of s16 stereo. See it_i2s_prefill_ready().
 *
 * This is NOT the TX FIFO and must not be confused with it: the FIFO depth is
 * the guest's clock and 2048 is the only value that keeps the DMA read head
 * behind the guest's producer. This is the lead handed to the HOST sink, on the
 * far side of the ring, and it exists because CoreAudio's IOProc plays nothing
 * at all in any period where less than one device buffer is queued.
 *
 * 32768 bytes is 185.8 ms. It was 8192 (46.4 ms, exactly what QEMU's own
 * pipeline holds for this backend at the default buffer-count of 4) and that
 * was measured to be not enough on the machine this runs on: on nand-appsync3
 * with the user's own warm overlay, under load, the sink still starved inside
 * the clip in 3 of 3 runs and the host added 209 ms of holes.
 *
 * THE PREBUFFER ALONE CANNOT FIX IT, and this is the part that took a second
 * round to see. The reservoir lives in OUR ring, and while the QEMU main loop is
 * stalled nothing moves out of that ring either -- so what a main-loop stall
 * actually eats is the buffering DOWNSTREAM of us, and that is capped at
 * hw->samples = out.buffer-count x 512 frames = 46 ms at the default count of 4.
 * The prebuffer's job is to FILL that capacity; the capacity itself has to come
 * from the audiodev. Both are needed and the measurement says so -- three
 * matched pairs, live CoreAudio device, one variable at a time:
 *
 *     defaults (8192 / count 4)          3 of 3 clips damaged by the host,
 *                                        209 ms inserted, 18 starvations
 *     out.buffer-count=16 alone          0 and 8 starvations across two runs
 *     prebuffer 32768 alone              1 hole, 1 starvation
 *     32768 + out.buffer-count=16        0 of 3 damaged, 0 starvations
 *
 * So run-ios3.sh --sound must pass `-audio driver=coreaudio,out.buffer-count=16`,
 * and it now does. The cost of this constant is that much added output latency,
 * once at the start of each sound.
 *
 * IT_I2S_PREBUFFER_BYTES overrides it; 0 restores the old just-in-time
 * behaviour for A/B.
 */
#define IT_I2S_PREBUFFER_BYTES_DEFAULT 32768

/* Never let the prebuffer be the reason a sound does not play. */
#define IT_I2S_PREFILL_DEADLINE_NS (250 * 1000 * 1000)

#define IT_I2S_FIFO_BYTES_DEFAULT 2048
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
    uint32_t prebuffer;       /* IT_I2S_PREBUFFER_BYTES: host-side lead */
    bool prefilled;           /* the current sound has passed the prebuffer */
    int64_t prefill_start_ns; /* when this sound's prebuffer began filling */
    int64_t pace_last_ns;     /* when we last drained the modelled FIFO */

    FILE *dump;           /* IT_I2S_DUMP: raw s16le stereo tap of the FIFO */
    uint64_t total_bytes; /* lifetime PCM bytes seen at the FIFO (debug) */
    uint64_t dropped;     /* bytes dropped on ring overflow (debug) */
} IPodTouchI2SState;

#endif
