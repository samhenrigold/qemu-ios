/*
 * S5L8720 I2S controller for the iPod touch 2G.
 *
 * The controller itself is tiny (see the register notes in the header). The
 * interesting part is the data path: the guest never writes PCM through MMIO PIO
 * - it hands a descriptor to a PL080 DMA channel whose destination is the TX
 * FIFO at 0x3CA00010. QEMU's PL080 (hw/dma/pl080.c) transfers straight into the
 * system address space, so every DMA element to the FIFO arrives here as an
 * ordinary MMIO write. We accumulate those bytes and feed them to a SWVoiceOut,
 * which the selected audio backend (e.g. wav) then renders/records.
 *
 * Use `-audio driver=wav,path=...`, NOT `-audiodev`. A sound card created in C
 * resolves its backend through the DEFAULT audiodev list, and only `-audio`
 * populates that; `-audiodev` alone leaves this device with nowhere to send
 * samples and looks exactly like a broken audio path. run-ios3.sh --sound.
 *
 * Sample format: the real rate/width live in the codec + the opaque clock
 * divider we don't decode, so we assume the CoreAudio-canonical 44100 Hz,
 * 16-bit, stereo, little-endian. Guessing the rate wrong only changes playback
 * pitch/duration, not whether sound is captured. Set IT_I2S_RATE to override.
 */

#include "hw/arm/ipod_touch_i2s.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"

#include <math.h>

#define IT_I2S_DEBUG_ENV "IT_I2S_DEBUG"

static bool it_i2s_debug(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv(IT_I2S_DEBUG_ENV) != NULL;
    }
    return cached;
}

#define IT_I2S_DPRINTF(fmt, ...) \
    do { if (it_i2s_debug()) { \
        printf("[i2s] " fmt, ## __VA_ARGS__); fflush(stdout); } } while (0)

/* Drain the PCM ring into the audio backend, up to `free_bytes` of headroom. */
static void it_i2s_drain(IPodTouchI2SState *s, int free_bytes)
{
    while (free_bytes > 0 && s->ring_level > 0) {
        uint32_t contig = IT_I2S_RING_SIZE - s->ring_tail; /* bytes to buffer end */
        uint32_t chunk = s->ring_level;
        if (chunk > (uint32_t)free_bytes) {
            chunk = free_bytes;
        }
        if (chunk > contig) {
            chunk = contig;
        }
        size_t written = AUD_write(s->voice, s->ring + s->ring_tail, chunk);
        if (written == 0) {
            break;
        }
        s->ring_tail = (s->ring_tail + written) % IT_I2S_RING_SIZE;
        s->ring_level -= written;
        free_bytes -= written;
    }
}

/* SWVoiceOut wants more data. */
static void it_i2s_out_cb(void *opaque, int free_bytes)
{
    IPodTouchI2SState *s = (IPodTouchI2SState *)opaque;
    if (s->ring_level) {
        IT_I2S_DPRINTF("out_cb free=%d level=%u\n", free_bytes, s->ring_level);
    }
    it_i2s_drain(s, free_bytes);

    /* When the guest has halted TX and we've flushed everything, park the
     * voice so the backend stops pulling silence. */
    if (!s->running && s->ring_level == 0 && s->active) {
        AUD_set_active_out(s->voice, 0);
        s->active = 0;
    }
}

static void it_i2s_activate(IPodTouchI2SState *s)
{
    if (s->card_ok && s->voice && !s->active) {
        AUD_set_active_out(s->voice, 1);
        s->active = 1;
    }
}

static void it_i2s_push(IPodTouchI2SState *s, const uint8_t *buf, unsigned len)
{
    unsigned i;

    s->total_bytes += len;

    /* Raw tap: everything that reaches the FIFO, regardless of the backend.
     * Play it back with e.g.
     *   ffplay -f s16le -ar 44100 -ch_layout stereo <path>
     * This is the ground truth for "did PCM arrive", separate from "was it
     * audible" - the backend can be missing and this file still fills. */
    if (s->dump) {
        fwrite(buf, 1, len, s->dump);
    }

    for (i = 0; i < len; i++) {
        if (s->ring_level >= IT_I2S_RING_SIZE) {
            s->dropped += (len - i);
            break;
        }
        s->ring[s->ring_head] = buf[i];
        s->ring_head = (s->ring_head + 1) % IT_I2S_RING_SIZE;
        s->ring_level++;
    }

    it_i2s_activate(s);
}

static uint64_t ipod_touch_i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchI2SState *s = (IPodTouchI2SState *)opaque;

    switch (offset) {
    case IT_I2S_ENABLE: return s->enable;
    case IT_I2S_TXCON:  return s->txcon;
    case IT_I2S_TXCOM:  return s->txcom;
    case IT_I2S_RXCON:  return s->rxcon;
    case IT_I2S_RXCOM:  return s->rxcom;
    case IT_I2S_TXFCTL: return s->txfctl;
    case IT_I2S_CLKDIV: return s->clkdiv;
    case IT_I2S_RXFIFO: return 0;
    default:            return 0;
    }
}

static void ipod_touch_i2s_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    IPodTouchI2SState *s = (IPodTouchI2SState *)opaque;

    if (offset == IT_I2S_TXFIFO) {
        /* PCM element, native (little) endian; store raw bytes in order. */
        uint8_t bytes[8];
        unsigned n = size > sizeof(bytes) ? sizeof(bytes) : size;
        for (unsigned i = 0; i < n; i++) {
            bytes[i] = (value >> (8 * i)) & 0xff;
        }
        it_i2s_push(s, bytes, n);
        return;
    }

    switch (offset) {
    case IT_I2S_ENABLE:
        s->enable = value;
        IT_I2S_DPRINTF("enable <- 0x%" PRIx64 "\n", value);
        break;
    case IT_I2S_TXCON:
        s->txcon = value;
        break;
    case IT_I2S_TXCOM:
        s->txcom = value;
        if ((value & 0x7) == IT_I2S_CMD_RUN) {
            s->running = true;
            it_i2s_activate(s);
            IT_I2S_DPRINTF("TX run (total=%" PRIu64 " dropped=%" PRIu64 ")\n",
                           s->total_bytes, s->dropped);
        } else if (value == IT_I2S_CMD_HALT) {
            s->running = false;
            if (s->dump) {
                fflush(s->dump);
            }
            IT_I2S_DPRINTF("TX halt (total=%" PRIu64 " dropped=%" PRIu64 ")\n",
                           s->total_bytes, s->dropped);
        }
        break;
    case IT_I2S_RXCON:
        s->rxcon = value;
        break;
    case IT_I2S_RXCOM:
        s->rxcom = value;
        break;
    case IT_I2S_TXFCTL:
        s->txfctl = value;
        break;
    case IT_I2S_CLKDIV:
        s->clkdiv = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ipod_touch_i2s_ops = {
    .read = ipod_touch_i2s_read,
    .write = ipod_touch_i2s_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * IT_I2S_TONE=<seconds> feeds a synthetic sine straight into the TX FIFO at
 * reset, as if the DMA had delivered it. It exercises exactly the same path
 * the guest's PCM will take (push -> ring -> AUD_write -> backend), so it tells
 * you whether the host side works without waiting on the AMC handshake.
 * IT_I2S_TONE_HZ overrides the pitch (default 440).
 */
static void it_i2s_test_tone(IPodTouchI2SState *s)
{
    const char *secs_env = getenv("IT_I2S_TONE");
    const char *hz_env = getenv("IT_I2S_TONE_HZ");
    double hz = hz_env ? atof(hz_env) : 440.0;
    double secs = secs_env ? atof(secs_env) : 0.0;
    uint32_t frames, i;

    if (secs <= 0.0) {
        return;
    }
    /* The ring is the whole budget; a longer request is simply truncated. */
    frames = (uint32_t)(secs * s->as.freq);
    if (frames > IT_I2S_RING_SIZE / 4) {
        frames = IT_I2S_RING_SIZE / 4;
    }

    for (i = 0; i < frames; i++) {
        int16_t v = (int16_t)(12000.0 * sin(2.0 * M_PI * hz * i / s->as.freq));
        uint8_t frame[4] = { v & 0xff, (v >> 8) & 0xff, v & 0xff, (v >> 8) & 0xff };
        it_i2s_push(s, frame, sizeof(frame));
    }
    s->running = true;
    it_i2s_activate(s);
    IT_I2S_DPRINTF("test tone: %u frames @ %.1f Hz queued\n", frames, hz);
}

static void ipod_touch_i2s_reset(DeviceState *dev)
{
    IPodTouchI2SState *s = IPOD_TOUCH_I2S(dev);

    s->enable = 0;
    s->txcon = s->txcom = 0;
    s->rxcon = s->rxcom = 0;
    s->txfctl = s->clkdiv = 0;
    s->ring_head = s->ring_tail = s->ring_level = 0;
    s->running = false;
    if (s->active && s->voice) {
        AUD_set_active_out(s->voice, 0);
    }
    s->active = false;

    /* After the state wipe, not before: reset runs once after realize and would
     * otherwise throw the queued tone away. */
    it_i2s_test_tone(s);
}

static void ipod_touch_i2s_realize(DeviceState *dev, Error **errp)
{
    IPodTouchI2SState *s = IPOD_TOUCH_I2S(dev);
    const char *dump_path = getenv("IT_I2S_DUMP");

    if (dump_path) {
        s->dump = fopen(dump_path, "wb");
        if (!s->dump) {
            warn_report("ipod i2s: cannot open IT_I2S_DUMP=%s", dump_path);
        }
    }

    s->as.freq = 44100;
    const char *rate = getenv("IT_I2S_RATE");
    if (rate) {
        int r = atoi(rate);
        if (r > 0) {
            s->as.freq = r;
        }
    }
    s->as.nchannels = 2;
    s->as.fmt = AUDIO_FORMAT_S16;
    s->as.endianness = 0; /* little endian */

    s->card_ok = AUD_register_card("ipod-i2s", &s->card, errp);
    if (!s->card_ok) {
        /* No audio backend registered: run silent but keep the machine alive. */
        warn_report("ipod i2s: no audio card; output will be dropped");
        return;
    }

    s->voice = AUD_open_out(&s->card, s->voice, "ipod-i2s.out", s,
                            it_i2s_out_cb, &s->as);
    if (!s->voice) {
        warn_report("ipod i2s: could not open output voice");
        s->card_ok = false;
        return;
    }
    AUD_set_volume_out(s->voice, 0, 255, 255);
    AUD_set_active_out(s->voice, 0);
}

static void ipod_touch_i2s_init(Object *obj)
{
    IPodTouchI2SState *s = IPOD_TOUCH_I2S(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_i2s_ops, s,
                          "ipod_touch_i2s", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void ipod_touch_i2s_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = ipod_touch_i2s_realize;
    dc->reset = ipod_touch_i2s_reset;
}

static const TypeInfo ipod_touch_i2s_type_info = {
    .name = TYPE_IPOD_TOUCH_I2S,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchI2SState),
    .instance_init = ipod_touch_i2s_init,
    .class_init = ipod_touch_i2s_class_init,
};

static void ipod_touch_i2s_register_types(void)
{
    type_register_static(&ipod_touch_i2s_type_info);
}

type_init(ipod_touch_i2s_register_types)
