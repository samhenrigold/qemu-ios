#ifndef IPOD_TOUCH_I2S_H
#define IPOD_TOUCH_I2S_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "audio/audio.h"

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

/* PCM ring in bytes. ~6 s at 44.1 kHz/16-bit/stereo; the DMA free-runs into it
 * in per-buffer bursts and the audio backend drains it, so this only needs to
 * absorb scheduling jitter, not a whole clip. */
#define IT_I2S_RING_SIZE (1 << 20)

#define TYPE_IPOD_TOUCH_I2S "ipodtouch.i2s"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchI2SState, IPOD_TOUCH_I2S)

typedef struct IPodTouchI2SState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

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

    FILE *dump;           /* IT_I2S_DUMP: raw s16le stereo tap of the FIFO */
    uint64_t total_bytes; /* lifetime PCM bytes seen at the FIFO (debug) */
    uint64_t dropped;     /* bytes dropped on ring overflow (debug) */
} IPodTouchI2SState;

#endif
