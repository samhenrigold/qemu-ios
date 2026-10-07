/*
 * S5L8930 I2S controller (i2s0 codec, i2s1 voice, i2s2 baseband), as driven
 * by 7B500 AppleS5L8900XI2SController / AppleARMIISAudio.
 *
 * Same register block the S5L8720's I2S has (hw/arm/ipod_touch_i2s.c): +0x00
 * enable (bit 1 reads back "TX drained"), +0x04 TX config (0x03000081),
 * +0x08 TX command (6 = run, 0 = halt), +0x10 TX FIFO, +0x30/+0x34 RX, +0x3C,
 * +0x40; the kernel also writes +0x810. The rest is a plain register file.
 *
 * PCM arrives in the TX FIFO from CDMA channel 0x1a (i2s0) as 16-bit writes,
 * paced by the CDMA model at this block's frame rate (s5l8930_cdma.c), so the
 * FIFO just appends to a ring the audio backend drains. Stereo S16LE at the
 * port's frame rate, read from its PMGR NCO bit clock (s5l8930_i2s_rate())
 * each time the TX side is started.
 *
 * Capture is the mirror image: CDMA channel 0x1b reads the RX FIFO (+0x38) at
 * the same paced rate and the FIFO hands out bytes a QEMU input voice (the
 * Mac's microphone under coreaudio) has queued, or silence. The test-only
 * "tone-hz" property replaces the input with a synthetic stereo sine at the
 * port's frame rate, so capture can be checked headless.
 *
 * Use `-audio driver=wav,path=out.wav` (or coreaudio): the card resolves
 * its backend from the default audiodev list, as the iPod's I2S does.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "audio/audio.h"
#include "migration/vmstate.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930I2SState, S5L8930_I2S)

#define I2S_REGS_SIZE   0x1000
#define I2S_CTRL        0x00
#define I2S_CTRL_TX_IDLE (1u << 1)
#define I2S_TXCOM       0x08
#define I2S_TXFIFO      0x10
#define I2S_RXCOM       0x34
#define I2S_RXFIFO      0x38
#define I2S_CMD_RUN     6
#define TONE_AMPLITUDE  8000

/* ~370 ms at 44.1 kHz stereo: absorbs host scheduling jitter only; the CDMA
 * pacing keeps the producer at the consumer's rate. */
#define I2S_RING        (64 * 1024)

struct S5L8930I2SState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion fifo;          /* MMIO 1: the TX FIFO alone (the S5L8920 DMAs to 0x84500000) */

    uint32_t regs[I2S_REGS_SIZE / 4];
    bool audio_out;                 /* property: this port reaches the host (out and in) */
    /* property "ctrl-run": the S5L8920's controller, which its driver starts with CTRL bit 0
     * (0x7900101) and stops by clearing it (0x7900000, then 0), leaving +0x08 at its frame
     * format (0x78057805); the A4's starts with TXCOM = 6. */
    bool ctrl_run;
    uint8_t port;                   /* property: i2s<port>, selects its NCO */
    unsigned rate;                  /* the voice's current frame rate */

    uint8_t ring[I2S_RING];
    uint32_t head, tail, level;

    /* capture: host input voice -> in_ring -> RX FIFO reads */
    uint32_t tone_hz;               /* property, test-only: synthetic input */
    uint8_t in_ring[I2S_RING];
    uint32_t in_head, in_tail, in_level;
    uint64_t tone_frame;            /* frames of the synthetic tone handed out */
    unsigned in_rate;
    uint8_t rx_frame[4];            /* the stereo S16 frame being read out */
    unsigned rx_pos;                /* bytes of rx_frame already read */

    QEMUSoundCard card;
    SWVoiceOut *voice;
    SWVoiceIn *voice_in;
};

static void i2s_in_cb(void *opaque, int avail)
{
    S5L8930I2SState *s = opaque;

    while (avail > 0 && s->in_level < I2S_RING) {
        uint32_t chunk = MIN(MIN((uint32_t)avail, I2S_RING - s->in_level),
                             I2S_RING - s->in_head);
        size_t got = AUD_read(s->voice_in, s->in_ring + s->in_head, chunk);

        if (!got) {
            break;
        }
        s->in_head = (s->in_head + got) % I2S_RING;
        s->in_level += got;
        avail -= got;
    }
}

static void i2s_set_in_rate(S5L8930I2SState *s, unsigned rate)
{
    struct audsettings as = {
        .freq = rate, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .endianness = 0,
    };

    if (rate == s->in_rate || s->tone_hz) {
        s->in_rate = rate;
        return;
    }
    s->voice_in = AUD_open_in(&s->card, s->voice_in, "s5l8930-i2s.in", s,
                              i2s_in_cb, &as);
    if (s->voice_in) {
        s->in_rate = rate;
    }
}

/* Next captured stereo S16 frame: the tone, the host's input, or silence. */
static void i2s_next_frame(S5L8930I2SState *s)
{
    if (s->tone_hz) {
        double t = (double)s->tone_frame++ / (s->in_rate ? s->in_rate : 44100);
        int16_t v = TONE_AMPLITUDE * sin(2 * M_PI * s->tone_hz * t);

        stw_le_p(s->rx_frame, v);
        stw_le_p(s->rx_frame + 2, v);
        return;
    }
    if (s->in_level < 4) {
        memset(s->rx_frame, 0, 4);
        return;
    }
    for (int i = 0; i < 4; i++) {
        s->rx_frame[i] = s->in_ring[s->in_tail];
        s->in_tail = (s->in_tail + 1) % I2S_RING;
    }
    s->in_level -= 4;
}

static uint64_t i2s_rx_read(S5L8930I2SState *s, unsigned size)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        if (s->rx_pos == 0) {
            i2s_next_frame(s);
        }
        v |= (uint64_t)s->rx_frame[s->rx_pos] << (8 * i);
        s->rx_pos = (s->rx_pos + 1) % 4;
    }
    return v;
}

static void i2s_push(S5L8930I2SState *s, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        if (s->level == I2S_RING) {
            return;                 /* host stalled: drop, never block the guest */
        }
        s->ring[s->head] = value >> (8 * i);
        s->head = (s->head + 1) % I2S_RING;
        s->level++;
    }
}

static void i2s_out_cb(void *opaque, int free_bytes)
{
    S5L8930I2SState *s = opaque;

    while (free_bytes > 0 && s->level > 0) {
        uint32_t chunk = MIN(MIN((uint32_t)free_bytes, s->level),
                             I2S_RING - s->tail);
        size_t done = AUD_write(s->voice, s->ring + s->tail, chunk);

        if (!done) {
            break;
        }
        s->tail = (s->tail + done) % I2S_RING;
        s->level -= done;
        free_bytes -= done;
    }
}

static void i2s_set_rate(S5L8930I2SState *s, unsigned rate)
{
    struct audsettings as = {
        .freq = rate, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .endianness = 0,
    };

    if (rate == s->rate) {
        return;
    }
    s->voice = AUD_open_out(&s->card, s->voice, "s5l8930-i2s.out", s,
                            i2s_out_cb, &as);
    if (s->voice) {
        AUD_set_volume_out(s->voice, 0, 255, 255);
        s->rate = rate;
    }
}

static uint64_t i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930I2SState *s = opaque;

    if (offset == I2S_CTRL) {
        /* Bit 1: TX drained. The stop path (7B500 c086f5ae) writes 0x300 to
         * +0x810 and spins on it; our FIFO never holds anything. The power-down
         * path (c086f524: +0x00 |= 0x20, spin until bit 1) waits on the same bit;
         * unanswered, it pinned a CPU from the first audio idle and took boot to
         * the lock screen from ~4 min to 32 s once fixed (a4-kbd). */
        return s->regs[0] | I2S_CTRL_TX_IDLE;
    }
    if (offset == I2S_RXFIFO) {
        return i2s_rx_read(s, size);
    }
    return offset == I2S_TXFIFO ? 0 : s->regs[offset >> 2];
}

static bool i2s_tx_running(S5L8930I2SState *s)
{
    return s->ctrl_run ? s->regs[I2S_CTRL >> 2] & 1 : s->regs[I2S_TXCOM >> 2] == I2S_CMD_RUN;
}

static void i2s_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    S5L8930I2SState *s = opaque;

    if (offset == I2S_TXFIFO) {
        if (s->voice && i2s_tx_running(s)) {
            i2s_push(s, value, size);
        }
        return;
    }
    s->regs[offset >> 2] = value;
    if (offset == I2S_RXCOM && s->audio_out) {
        bool run = value == I2S_CMD_RUN;

        if (run) {
            i2s_set_in_rate(s, s5l8930_i2s_rate(s->port));
            s->rx_pos = 0;
        }
        if (s->voice_in) {
            AUD_set_active_in(s->voice_in, run);
        }
    }
    if (offset == (s->ctrl_run ? I2S_CTRL : I2S_TXCOM) && s->voice) {
        if (i2s_tx_running(s)) {
            i2s_set_rate(s, s5l8930_i2s_rate(s->port));
        }
        AUD_set_active_out(s->voice, i2s_tx_running(s));
    }
}

static uint64_t i2s_fifo_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void i2s_fifo_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    i2s_write(opaque, I2S_TXFIFO, value, size);
}

static const MemoryRegionOps i2s_fifo_ops = {
    .read = i2s_fifo_read,
    .write = i2s_fifo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps i2s_ops = {
    .read = i2s_read,
    .write = i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void s5l8930_i2s_realize(DeviceState *dev, Error **errp)
{
    S5L8930I2SState *s = S5L8930_I2S(dev);

    if (!s->audio_out) {
        return;
    }
    if (!AUD_register_card("s5l8930-i2s", &s->card, errp)) {
        warn_report("s5l8930 i2s: no audio backend; output dropped");
        return;
    }
    i2s_set_rate(s, 44100);
}

static void s5l8930_i2s_reset(DeviceState *dev)
{
    S5L8930I2SState *s = S5L8930_I2S(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->head = s->tail = s->level = 0;
    s->in_head = s->in_tail = s->in_level = 0;
    s->tone_frame = 0;
    s->rx_pos = 0;
    if (s->voice) {
        AUD_set_active_out(s->voice, 0);
    }
    if (s->voice_in) {
        AUD_set_active_in(s->voice_in, 0);
    }
}

static void s5l8930_i2s_init(Object *obj)
{
    S5L8930I2SState *s = S5L8930_I2S(obj);

    memory_region_init_io(&s->iomem, obj, &i2s_ops, s, TYPE_S5L8930_I2S,
                          I2S_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    memory_region_init_io(&s->fifo, obj, &i2s_fifo_ops, s, "s5l8930.i2s-fifo", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->fifo);
}

static int s5l8930_i2s_post_load(void *opaque, int version_id)
{
    S5L8930I2SState *s = opaque;

    if (s->voice && i2s_tx_running(s)) {
        i2s_set_rate(s, s5l8930_i2s_rate(s->port));
        AUD_set_active_out(s->voice, 1);
    }
    return 0;
}

/* The PCM ring is transient host-side buffering and is not migrated. */
static const VMStateDescription vmstate_s5l8930_i2s = {
    .name = TYPE_S5L8930_I2S,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8930_i2s_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8930I2SState, I2S_REGS_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8930_i2s_properties[] = {
    DEFINE_PROP_BOOL("audio-out", S5L8930I2SState, audio_out, false),
    DEFINE_PROP_BOOL("ctrl-run", S5L8930I2SState, ctrl_run, false),
    DEFINE_PROP_UINT8("port", S5L8930I2SState, port, 0),
    DEFINE_PROP_UINT32("tone-hz", S5L8930I2SState, tone_hz, 0),
};

static void s5l8930_i2s_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8930_i2s_realize;
    dc->vmsd = &vmstate_s5l8930_i2s;
    device_class_set_legacy_reset(dc, s5l8930_i2s_reset);
    device_class_set_props(dc, s5l8930_i2s_properties);
}

static const TypeInfo s5l8930_i2s_info = {
    .name          = TYPE_S5L8930_I2S,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930I2SState),
    .instance_init = s5l8930_i2s_init,
    .class_init    = s5l8930_i2s_class_init,
};

static void s5l8930_i2s_register_types(void)
{
    type_register_static(&s5l8930_i2s_info);
}

type_init(s5l8930_i2s_register_types)
