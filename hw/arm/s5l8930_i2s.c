/*
 * S5L8930 I2S controller (i2s0 codec, i2s1 voice, i2s2 baseband), as driven
 * by 7B500 AppleS5L8900XI2SController / AppleARMIISAudio.
 *
 * Same register block the S5L8720's I2S has (hw/arm/ipod_touch_i2s.c): +0x00
 * enable, +0x04 TX config (0x03000081), +0x08 TX command (6 = run, 0 =
 * halt), +0x10 TX FIFO, +0x30/+0x34 RX, +0x3C, +0x40; the kernel also
 * writes +0x810. Everything is a plain register file except the TX FIFO.
 *
 * PCM arrives in the TX FIFO from CDMA channel 0x1a (i2s0) as 16-bit writes,
 * paced by the CDMA model at this block's frame rate (s5l8930_cdma.c), so the
 * FIFO just appends to a ring the audio backend drains. Stereo S16LE at
 * 44.1 kHz: the IOAudio2 device publishes "sample rate" 44100 and never
 * changes it for UI sounds.
 * ponytail: fixed rate; read the codec's clock setup when media at other
 * rates (48 kHz video, 8/16 kHz voice) must play at the right pitch.
 *
 * Use `-audio driver=wav,path=out.wav` (or coreaudio): the card resolves
 * its backend from the default audiodev list, as the iPod's I2S does.
 */
#include "qemu/osdep.h"
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
#define I2S_TXCOM       0x08
#define I2S_TXFIFO      0x10
#define I2S_CMD_RUN     6

/* ~370 ms at 44.1 kHz stereo: absorbs host scheduling jitter only; the CDMA
 * pacing keeps the producer at the consumer's rate. */
#define I2S_RING        (64 * 1024)

struct S5L8930I2SState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t regs[I2S_REGS_SIZE / 4];
    bool audio_out;                 /* property: this port reaches the host */

    uint8_t ring[I2S_RING];
    uint32_t head, tail, level;

    QEMUSoundCard card;
    SWVoiceOut *voice;
};

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

static uint64_t i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930I2SState *s = opaque;

    return offset == I2S_TXFIFO ? 0 : s->regs[offset >> 2];
}

static void i2s_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    S5L8930I2SState *s = opaque;

    if (offset == I2S_TXFIFO) {
        if (s->voice && s->regs[I2S_TXCOM >> 2] == I2S_CMD_RUN) {
            i2s_push(s, value, size);
        }
        return;
    }
    s->regs[offset >> 2] = value;
    if (offset == I2S_TXCOM && s->voice) {
        AUD_set_active_out(s->voice, value == I2S_CMD_RUN);
    }
}

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
    struct audsettings as = {
        .freq = 44100, .nchannels = 2, .fmt = AUDIO_FORMAT_S16,
        .endianness = 0,
    };

    if (!s->audio_out) {
        return;
    }
    if (!AUD_register_card("s5l8930-i2s", &s->card, errp)) {
        warn_report("s5l8930 i2s: no audio backend; output dropped");
        return;
    }
    s->voice = AUD_open_out(&s->card, NULL, "s5l8930-i2s.out", s,
                            i2s_out_cb, &as);
    if (s->voice) {
        AUD_set_volume_out(s->voice, 0, 255, 255);
    }
}

static void s5l8930_i2s_reset(DeviceState *dev)
{
    S5L8930I2SState *s = S5L8930_I2S(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->head = s->tail = s->level = 0;
    if (s->voice) {
        AUD_set_active_out(s->voice, 0);
    }
}

static void s5l8930_i2s_init(Object *obj)
{
    S5L8930I2SState *s = S5L8930_I2S(obj);

    memory_region_init_io(&s->iomem, obj, &i2s_ops, s, TYPE_S5L8930_I2S,
                          I2S_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static int s5l8930_i2s_post_load(void *opaque, int version_id)
{
    S5L8930I2SState *s = opaque;

    if (s->voice) {
        AUD_set_active_out(s->voice,
                           s->regs[I2S_TXCOM >> 2] == I2S_CMD_RUN);
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
