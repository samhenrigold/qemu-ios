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
 *
 * ---------------------------------------------------------------------------
 * STATE OF THE GUEST-SIDE AUDIO PATH (measured, 2026-08-03)
 *
 * There is still no sound. Everything below was probed, not inferred -- and it
 * moves the blocker a long way from where this file's comments used to put it.
 *
 * What is PROVEN WORKING:
 *   - The host path. IT_I2S_TONE puts a sine in the TX FIFO and it comes out:
 *     440.0 Hz at amplitude 11999 through -audio driver=wav.
 *   - The sounds need no decoding. All 63 files in /System/Library/Audio/
 *     UISounds are `lpcm`; alarm.caf is mono 16-bit 44100, i.e. 2 bytes per
 *     frame -- which is exactly the `frames << 1` AppleAMC_r2's self test
 *     asserts. No AAC/MP3 decoder is needed for system sounds, ever.
 *   - AppleARMIISAudio's start path runs in full: 0xc0505a00 -> 0xc05054f8 ->
 *     0xc05053ec -> 0xc0505340 -> [dmac vtable+0x358] = 0xc0318b90 (Thumb).
 *     Neither of 0xc05054f8's two silent-success early-outs fires; confirmed
 *     twice over, by frame walk and by reading [this+0x6d] == 0.
 *   - This controller is fully configured. The whole conversation is seven
 *     writes and ZERO reads (enable, clkdiv, txcon, 0x30, txcom, 0x34, txfctl),
 *     so no value this model supplies can be gating anything -- including the
 *     two registers at 0x30/0x34 we do not model.
 *
 * What is NOT the problem, though it long looked like it:
 *   - The AMC's buffer aperture. The driver reads a 16-byte header out of it
 *     and never one byte of the ~32 KB it stages there. Everything in that
 *     window is guest-written self-test material (the Numerical Recipes LCG
 *     constants sit in it), not audio.
 *   - "Nothing upstream produced bytes." That was my inference and it is WRONG.
 *
 * THE MEASUREMENT THAT SETTLED IT. The DMA request 0xc0318b90 builds and
 * submits is fully formed and describes a REAL transfer. Read live, and
 * validated by [req+0x54] matching the DMA controller object it must point to:
 *
 *     [req+0x54] c0bfdb00   the DMA controller
 *     [req+0x58] c0c58900   an IOKit memory object (length 0x10000 at +0x1c)
 *     [req+0x5c] 00000002   direction
 *     [req+0x60] 00001000   4096 -- the same value written to I2S reg 0x30
 *     [req+0x64] 0000f000   61440 bytes, i.e. 15 periods of 4096
 *     [req+0x68] 0000000a
 *     [req+0x7c] c0c6a800   the AppleARMIISAudio instance
 *     [req+0x80] c050601c   completion callback, in that kext's text
 *
 * So the driver believes it has 61440 bytes to send, names the memory holding
 * them, and hands the request to the DMA controller kext with a completion
 * routine attached. The request is then simply never executed: across a whole
 * sound the PL080 sees 139 channel starts and not one targets the TX FIFO at
 * 0x3ca00010 (IT_DMA_TRACE fires on every channel-configuration write, so it
 * would see them).
 *
 * The gap is therefore inside the DMA controller kext (Thumb, ~0xc0318xxx),
 * between accepting a well-formed request and programming a channel. That is a
 * much better place to be than the AMC: it drives hardware we DO model, so if
 * it is waiting on a PL080 state we answer wrongly, that is a gate we could
 * satisfy honestly rather than fake.
 *
 * Instruments: IT_I2S_PC / IT_I2S_DEREF here, IT_AMC_PC / IT_AMC_V2P /
 * IT_AMC_WATCH for the AMC side. Guest VAs must be read out of RAM via QMP
 * pmemsave (VA - 0xB8000000 = PA); the kernelcache in ~/Developer/ipod2g-re is
 * NOT the image that boots.
 * ---------------------------------------------------------------------------
 */

#include "hw/arm/ipod_touch_i2s.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/core/cpu.h"
#include "cpu.h"

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

/*
 * IT_I2S_PC=<hex offset> -- log the guest PC, LR and an r7 frame walk for
 * accesses to that register. Deliberately the same shape as amc_log_caller()
 * in ipod_touch_amc.c, and kept separate rather than shared because two copies
 * of a 40-line debug walker is cheaper than a header both devices must agree
 * on; if a third device ever wants it, extract it then.
 *
 * Why this device: the remaining audio blocker is in AppleARMIISAudio, which
 * decides there is nothing to DMA and reports that as success (0xc05054f8 has
 * two early-outs that return 0 having programmed no channel -- a byte test on
 * [this+0x6d], and a virtual [vtable+0x3a0] returning 0). Working out which one
 * fires needs that driver's `this`, and I2S registers are the only hardware it
 * touches that we model. Each frame prints as lr/r5 because these drivers are
 * ARM, not Thumb, and open with `push {r4-r7, lr}; add r7, sp, #0xc` -- so the
 * caller's r5, which is where an IOService method keeps `this`, is recoverable
 * from the frame even when it has long since been clobbered in the register.
 *
 * IT_I2S_DEREF=<hex>[,...] additionally dumps [r5 + <hex>], following it one
 * level when it looks like a pointer. Print the raw word too: a deref that
 * merely reads zeroes is otherwise indistinguishable from a wrong r5.
 */
static void it_i2s_log_caller(hwaddr offset, uint32_t val)
{
    static int64_t want = -2;
    static int budget = 64;
    CPUARMState *env;
    uint32_t fp;

    if (want == -2) {
        const char *v = getenv("IT_I2S_PC");
        want = v ? strtoll(v, NULL, 16) : -1;
    }
    if (want < 0 || (hwaddr)want != offset || budget <= 0 || !current_cpu) {
        return;
    }
    budget--;
    env = &ARM_CPU(current_cpu)->env;
    fprintf(stderr, "[I2S] %04x <- %08x  pc=%08x lr=%08x  stack:",
            (unsigned)offset, val, env->regs[15], env->regs[14]);
    fp = env->regs[7];
    for (int i = 0; i < 8 && fp; i++) {
        uint32_t frame[5] = { 0 };      /* r4, r5, r6, r7, lr */
        if (cpu_memory_rw_debug(current_cpu, fp - 12, (uint8_t *)frame,
                                sizeof(frame), false) != 0) {
            break;
        }
        /* r4, r5 and r6: these IOService methods keep `this` in r4 as often as
         * in r5 (0xc0505340 and 0xc05053ec both do `mov r4, r0`), and the DMA
         * builder at 0xc0318b90 keeps the REQUEST it is filling in r6 -- which
         * is the only handle on that object, since it is freshly allocated and
         * has no static address. Thumb frames here use the same convention
         * (`push {r4-r7,lr}` ... `add r7, sp, #0x18`), so the walk is valid for
         * both instruction sets. */
        fprintf(stderr, " %08x/r4=%08x/r5=%08x/r6=%08x",
                frame[4], frame[0], frame[1], frame[2]);
        if (frame[3] <= fp) {
            break;
        }
        fp = frame[3];
    }
    fprintf(stderr, "\n");

    const char *deref = getenv("IT_I2S_DEREF");
    if (deref) {
        fprintf(stderr, "[I2S] r4=%08x r5=%08x r6=%08x",
                env->regs[4], env->regs[5], env->regs[6]);
        for (const char *p = deref; p && *p; ) {
            uint32_t off = strtoul(p, NULL, 16), word = 0;
            uint8_t buf[16];
            if (cpu_memory_rw_debug(current_cpu, env->regs[5] + off,
                                    (uint8_t *)&word, 4, false) != 0) {
                fprintf(stderr, "  [r5+%x]=<unreadable>", off);
            } else if (word >= 0xc0000000 &&
                       cpu_memory_rw_debug(current_cpu, word, buf,
                                           sizeof(buf), false) == 0) {
                fprintf(stderr, "  [r5+%x]=%08x ->", off, word);
                for (unsigned i = 0; i < sizeof(buf); i++) {
                    fprintf(stderr, "%02x", buf[i]);
                }
            } else {
                fprintf(stderr, "  [r5+%x]=%08x", off, word);
            }
            p = strchr(p, ',');
            if (p) {
                p++;
            }
        }
        fprintf(stderr, "\n");
    }
}

/*
 * Emit I2S accesses into the same stderr stream as IT_DMAC_TRACE, so a single
 * ordered log shows where in the DMAC conversation the audio driver brings this
 * controller up. Correlating two separately-buffered logs was the previous
 * approach and it could not answer "what did the DMAC do NEXT".
 */
bool it_dmac_trace_on(void);

static void it_i2s_dmac_mark(hwaddr offset, uint32_t val, bool write)
{
    uint32_t pc = 0;

    if (!it_dmac_trace_on() || offset == IT_I2S_TXFIFO) {
        return;
    }
    if (current_cpu) {
        pc = ARM_CPU(current_cpu)->env.regs[15];
    }
    fprintf(stderr, "[i2s  ] %c %03x                %08x  pc=%08x\n",
            write ? 'W' : 'R', (unsigned)offset, val, pc);
}

static uint64_t ipod_touch_i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchI2SState *s = (IPodTouchI2SState *)opaque;

    uint32_t val;

    it_i2s_log_caller(offset, 0);

    switch (offset) {
    case IT_I2S_ENABLE: val = s->enable; break;
    case IT_I2S_TXCON:  val = s->txcon;  break;
    case IT_I2S_TXCOM:  val = s->txcom;  break;
    case IT_I2S_RXCON:  val = s->rxcon;  break;
    case IT_I2S_RXCOM:  val = s->rxcom;  break;
    case IT_I2S_TXFCTL: val = s->txfctl; break;
    case IT_I2S_CLKDIV: val = s->clkdiv; break;
    case IT_I2S_RXFIFO: val = 0;         break;
    default:            val = 0;         break;
    }

    /*
     * Log every register touch, not just the FIFO. A register we do not model
     * answers 0, and 0 is a perfectly plausible value -- so a driver that polls
     * one and gives up is indistinguishable from one that never asked. Seeing
     * the whole conversation is the only way to tell those apart.
     */
    IT_I2S_DPRINTF("R %02x -> %08x\n", (unsigned)offset, val);
    it_i2s_dmac_mark(offset, val, false);
    return val;
}

static void ipod_touch_i2s_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    IPodTouchI2SState *s = (IPodTouchI2SState *)opaque;

    it_i2s_log_caller(offset, (uint32_t)value);
    it_i2s_dmac_mark(offset, (uint32_t)value, true);
    if (offset != IT_I2S_TXFIFO) {
        IT_I2S_DPRINTF("W %02x <- %08x\n", (unsigned)offset, (uint32_t)value);
    }

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
