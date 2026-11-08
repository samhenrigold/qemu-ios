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
 * port's frame rate (s5l8930_i2s_rate()), as the hardware makes it: the
 * port's own configuration says whether it drives the frame clock
 * (i2s_frame_master()). As the master it divides its MCLK (the A4's PMGR NCO;
 * the S5L8920's audio complex NCO, "nco-base"/"nco-ref-hz") into frames; as
 * the slave it follows the LRCLK of whatever drives the clock, the codec
 * connected to its "lrclk" input (hw/arm/ipod_touch_cs42l58.c). The voice
 * takes the rate when TX starts, when a CDMA chain starts and whenever the
 * codec's LRCLK changes while TX runs.
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
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/arm/s5l8930.h"
#include "hw/arm/ipod_touch_cs42l58.h"
#include "qemu/audio.h"
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
#define I2S_TXCON       0x04
/*
 * The Samsung I2S (the A4's 'i2s-1,samsung', AppleSamsungI2SController; the S5L8900's and S5L8720's
 * AppleS5L8900XI2SController, hw/arm/ipod_touch_i2s.c) takes BCLK and LRCLK from outside while TX config bit 20 is
 * set. Every DT audio-data TX config word (word 2) has it set where a codec or a baseband masters the clock and
 * clear where the port drives it: k48ap 3.2-5.1.1 i2s0 and i2s1 0x03000081 (the port, at its PMGR NCO) and i2s2 (the
 * baseband) 0x03100019/0x03100081; n90ap 4.x-6.x i2s0 and i2s1 0x03000081; n81ap 4.1-6.1.6 i2s0 (the CS42L59's,
 * its rate in the codec's register 0a) 0x03100019/0x03100081; n72ap 3.1.3 (the CS42L58's, 6 MHz MCLK)
 * 0x03100019; m68ap 1.1.4 (the WM8758's, its PLL a 256 fs SYSCLK) 0x01100300. The +0x410 config word the drivers
 * write beside it agrees on every one (bits 30-31 set on the masters, 0xc8000300/0xe8000300, clear on the slaves).
 * Rockbox's drivers for the same block on the S5L8700 and S5L8702 iPods (firmware/target/arm/s5l870x/pcm-*.c) set
 * I2STXCON bit 20 on all three, each with its codec programmed as the clock master (the CS42L55's CLKCTL1 MASTER,
 * the WM8975's AINTFCE MS, the Nano 3G's WM1870).
 */
#define I2S_TXCON_EXT_CLK (1u << 20)
#define I2S_MCLK_DIV    0x408   /* S5L8920: bits 8-15, MCLK per BCLK */
#define I2S_FRAME_BITS  0x410   /* S5L8920: bits 0-7, BCLK per frame */
/*
 * The S5L8920's own I2S ('i2s,s5l8920x', AppleS5L8920XI2SController) drives BCLK and LRCLK while +0x410 bits 30-31
 * are set. Its driver keeps whether the port is the clock master from bit 21 of DT audio-data word 0, and programs
 * the audio complex's MCLK in setSampleRate only for a master (3.1.3 0xc0677988 and 6.1.6 0x8078a744 store
 * !(word 0 bit 21); 6.1.6 setSampleRate 0x8078aac6 skips the MCLK request when that is set). Word 5, which it
 * writes to +0x410, has bits 30-31 set exactly where word 0 has bit 21: the 3GS's i2s0 and i2s1 (0x00600103,
 * 0xc3200020; 3.1.3 and 6.1.6), and clear on the 3GS's baseband port (0x103, 0x22000040) and the iPod touch 3G's
 * CS42L58 port (0x103, 0x20000040; 3.1.1 and 5.1.1). ponytail: which of the written fields the hardware reads for
 * the direction rests on that agreement; no datasheet for this block.
 */
#define I2S_FRAME_MASTER 0xc0000000u
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
    bool audio_in;                  /* MCA capture endpoint, separate from speaker feedback */
    /* property "ctrl-run": the S5L8920's controller, which its driver starts with CTRL bit 0
     * (0x7900101) and stops by clearing it (0x7900000, then 0), leaving +0x08 at its frame
     * format (0x78057805); the A4's starts with TXCOM = 6. */
    bool ctrl_run;
    bool mca;                      /* A6 MCA register layout, shared PCM backend */
    uint32_t frame_rate;            /* virtual external frame clock for MCA */
    unsigned out_channels, out_bytes;
    uint8_t port;                   /* property: i2s<port>, selects its NCO */
    Clock *lrclk;                   /* in: the codec's frame clock, followed while the port is the slave */
    CS42L58State *codec;            /* link: the codec this port feeds, whose output volume the host hears */
    uint64_t nco_base;              /* property, S5L8920: the audio complex's NCO for MCLK */
    uint32_t nco_ref_hz;            /* property, S5L8920: that NCO's reference; 0 = the A4's PMGR NCO */
    unsigned rate;                  /* the voice's current frame rate */

    uint8_t ring[I2S_RING];
    uint32_t head, tail, level;

    /* capture: host input voice -> in_ring -> RX FIFO reads */
    uint32_t tone_hz;               /* property, test-only: synthetic input */
    uint8_t in_ring[I2S_RING];
    uint32_t in_head, in_tail, in_level;
    uint64_t tone_frame;            /* frames of the synthetic tone handed out */
    unsigned in_rate;
    uint8_t rx_frame[32 * 4];        /* up to 32 MCA slots, S16 or padded S32 */
    unsigned rx_pos;                /* bytes of rx_frame already read */

    AudioBackend *audio_be;
    SWVoiceOut *voice;
    SWVoiceIn *voice_in;
};

/* Each port's block, for the CDMA's pacing. ponytail: one machine per process;
 * a link property from the CDMA if that ever changes. */
static S5L8930I2SState *i2s_ports[8];

/* Original AppleAE2MCA programs +8 with TX/RX padding and +24/+38
 * with the active channel masks (11A465, 80bd5ace/5c7c/5d00). */
static unsigned i2s_channels(S5L8930I2SState *s, bool input)
{
    return s->mca ? MAX(1, ctpop32(s->regs[(input ? 0x38 : 0x24) >> 2])) : 2;
}

static unsigned i2s_sample_bytes(S5L8930I2SState *s, bool input)
{
    return s->mca && ((s->regs[8 >> 2] >> (input ? 8 : 0)) & 0xff) < 16 ? 4 : 2;
}

unsigned s5l8930_i2s_frame_bytes(unsigned port, bool input)
{
    S5L8930I2SState *s = port < ARRAY_SIZE(i2s_ports) ? i2s_ports[port] : NULL;
    return s ? i2s_channels(s, input) * i2s_sample_bytes(s, input) : 4;
}

/*
 * The S5L8920's frame rate: the audio complex's NCO makes MCLK from its
 * reference, ncoref * A / (A - B) / 2 for A = +0x18 and B = +0x1c
 * (AppleS5L8920XAudioComplex writes A = 2 MCLK, B = 2 MCLK - ncoref), and the
 * I2S divides MCLK by +0x408 bits 8-15 into BCLK and BCLK by the +0x410 low
 * byte's bit clocks per frame. 6.1.6 AppleS5L8920XI2SController::setSampleRate
 * (0x8078aab8) asks for MCLK = those two (its DT audio-data words 7 and 5,
 * 0x20f and 0xc3200020: 2 and 32) times the rate, so 64 fs: 2.8224 MHz for
 * 44.1 kHz, 1.4112 MHz for the 22.05 kHz VoiceOver's speech runs at.
 * 44.1 kHz until the guest has programmed a rate in range.
 */
static unsigned i2s_nco_mclk_rate(S5L8930I2SState *s)
{
    uint32_t a = address_space_ldl_le(&address_space_memory, s->nco_base + 0x18,
                                      MEMTXATTRS_UNSPECIFIED, NULL);
    uint32_t b = address_space_ldl_le(&address_space_memory, s->nco_base + 0x1c,
                                      MEMTXATTRS_UNSPECIFIED, NULL);
    unsigned per_frame = (s->regs[I2S_MCLK_DIV >> 2] >> 8 & 0xff) *
                         (s->regs[I2S_FRAME_BITS >> 2] & 0xff);
    uint64_t mclk, rate;

    if (!a || a == b || !per_frame) {
        return 44100;
    }
    mclk = (uint64_t)s->nco_ref_hz * a / (uint32_t)(a - b) / 2;
    rate = mclk / per_frame;
    return rate >= 8000 && rate <= 192000 ? rate : 44100;
}

/* Whether the port drives its frame clock, from its own configuration (I2S_TXCON_EXT_CLK, I2S_FRAME_MASTER). */
static bool i2s_frame_master(S5L8930I2SState *s)
{
    return s->ctrl_run ? s->regs[I2S_FRAME_BITS >> 2] & I2S_FRAME_MASTER
                       : !(s->regs[I2S_TXCON >> 2] & I2S_TXCON_EXT_CLK);
}

unsigned s5l8930_i2s_rate(unsigned port)
{
    S5L8930I2SState *s;
    uint32_t bclk;

    if (port >= ARRAY_SIZE(i2s_ports)) {
        return 44100;
    }
    s = i2s_ports[port];
    if (!s) {
        return 44100;
    }
    if (s->mca) {
        unsigned hz = clock_get_hz(s->lrclk);
        return hz ? hz : s->frame_rate;
    }
    if (!i2s_frame_master(s) || s->nco_ref_hz) {
        /* ponytail: no clock connected or none driven yet plays at 44.1 kHz, where the hardware would stall.
         * Both clocks come from guest register bits: outside audio rates, 44.1 kHz too. */
        unsigned hz = !i2s_frame_master(s) ? clock_get_hz(s->lrclk) : i2s_nco_mclk_rate(s);
        return hz >= 8000 && hz <= 192000 ? hz : 44100;
    }
    bclk = address_space_ldl_le(&address_space_memory, S5L8930_NCO_BCLK(port),
                                MEMTXATTRS_UNSPECIFIED, NULL);
    return bclk >= 64 * 8000 && bclk <= 64 * 192000 ? bclk / 64 : 44100;
}

static void i2s_in_cb(void *opaque, int avail)
{
    S5L8930I2SState *s = opaque;

    while (avail > 0 && s->in_level < I2S_RING) {
        uint32_t chunk = MIN(MIN((uint32_t)avail, I2S_RING - s->in_level),
                             I2S_RING - s->in_head);
        size_t got = audio_be_read(s->audio_be, s->voice_in, s->in_ring + s->in_head, chunk);

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
        .freq = rate, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .big_endian = false,
    };

    if (rate == s->in_rate || s->tone_hz) {
        s->in_rate = rate;
        return;
    }
    s->voice_in = audio_be_open_in(s->audio_be, s->voice_in, "s5l8930-i2s.in", s,
                              i2s_in_cb, &as);
    if (s->voice_in) {
        s->in_rate = rate;
    }
}

/* Capture stays stereo S16 at the shared QEMU backend. The virtual codec
 * mirrors that pair into additional MCA receive slots; this supplies a host
 * microphone, not a model of the physical device's microphone array. */
static void i2s_next_frame(S5L8930I2SState *s)
{
    uint8_t stereo[4] = {0};
    if (s->tone_hz) {
        double t = (double)s->tone_frame++ / (s->in_rate ? s->in_rate : 44100);
        int16_t v = TONE_AMPLITUDE * sin(2 * M_PI * s->tone_hz * t);

        stw_le_p(stereo, v);
        stw_le_p(stereo + 2, v);
    } else if (s->in_level >= 4) {
        for (int i = 0; i < 4; i++) {
            stereo[i] = s->in_ring[s->in_tail];
            s->in_tail = (s->in_tail + 1) % I2S_RING;
        }
        s->in_level -= 4;
    }
    unsigned bytes = i2s_sample_bytes(s, true);
    for (unsigned ch = 0; ch < i2s_channels(s, true); ch++) {
        uint16_t sample = lduw_le_p(stereo + (ch % 2) * 2);
        if (bytes == 4) {
            /* Native Codec mono input advertises 16 valid bits in a four-byte
             * word, signed, without the aligned-high format flag. */
            stl_le_p(s->rx_frame + ch * bytes, (uint32_t)(int32_t)(int16_t)sample);
        } else {
            stw_le_p(s->rx_frame + ch * bytes, sample);
        }
    }
}

static uint64_t i2s_rx_read(S5L8930I2SState *s, unsigned size)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        if (s->rx_pos == 0) {
            i2s_next_frame(s);
        }
        v |= (uint64_t)s->rx_frame[s->rx_pos] << (8 * i);
        s->rx_pos = (s->rx_pos + 1) %
                    (i2s_channels(s, true) * i2s_sample_bytes(s, true));
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

/* The voice wants more data. The codec's output volume applies on the way out, as the codec applies it after the port. */
static void i2s_out_cb(void *opaque, int free_bytes)
{
    S5L8930I2SState *s = opaque;
    double gain = s->codec ? cs42l58_output_gain(s->codec) : 1.0;
    uint8_t scaled[4096];

    while (free_bytes > 0 && s->level > 0) {
        uint32_t chunk = MIN(MIN((uint32_t)free_bytes, s->level),
                             I2S_RING - s->tail);
        const uint8_t *data = s->ring + s->tail;

        if (gain != 1.0 && chunk >= 2) {
            chunk = MIN(chunk, sizeof(scaled)) & ~1u;     /* whole S16 samples */
            for (uint32_t i = 0; i < chunk; i += 2) {
                long v = lrint((int16_t)lduw_le_p(data + i) * gain);

                stw_le_p(scaled + i, CLAMP(v, INT16_MIN, INT16_MAX));
            }
            data = scaled;
        }
        size_t done = audio_be_write(s->audio_be, s->voice, (void *)data, chunk);

        if (!done) {
            break;
        }
        s->tail = (s->tail + done) % I2S_RING;
        s->level -= done;
        free_bytes -= done;
    }
}

static bool i2s_tx_running(S5L8930I2SState *s)
{
    if (s->mca) {
        return s->regs[0x18 >> 2] & 1;
    }
    return s->ctrl_run ? s->regs[I2S_CTRL >> 2] & 1 : s->regs[I2S_TXCOM >> 2] == I2S_CMD_RUN;
}

static void i2s_set_rate(S5L8930I2SState *s, unsigned rate)
{
    unsigned channels = i2s_channels(s, false);
    unsigned bytes = i2s_sample_bytes(s, false);
    struct audsettings as = {
        .freq = rate, .nchannels = channels,
        .fmt = bytes == 4 ? AUDIO_FORMAT_S32 : AUDIO_FORMAT_S16, .big_endian = false,
    };

    if (rate == s->rate && channels == s->out_channels && bytes == s->out_bytes) {
        return;
    }
    s->voice = audio_be_open_out(s->audio_be, s->voice, "s5l8930-i2s.out", s,
                            i2s_out_cb, &as);
    if (s->voice) {
        audio_be_set_volume_out_lr(s->audio_be, s->voice, 0, 255, 255);
        /* Reopened, the voice is inactive: a change while TX runs left it silent. */
        audio_be_set_active_out(s->audio_be, s->voice, i2s_tx_running(s));
        s->rate = rate;
        s->out_channels = channels;
        s->out_bytes = bytes;
    }
}

static uint64_t i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930I2SState *s = opaque;

    if (s->mca) {
        if (offset == 0x3c) {
            return s->audio_in && (s->regs[0x2c >> 2] & 1) ? i2s_rx_read(s, size) : 0;
        }
        return offset == 0x28 ? 0 : s->regs[offset >> 2];
    }

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

/* The codec switched rates (the iPod touch 3G and 4 go to 22.05 kHz for VoiceOver's speech). */
static void i2s_lrclk_update(void *opaque, ClockEvent event)
{
    S5L8930I2SState *s = opaque;

    if (s->voice && i2s_tx_running(s)) {
        i2s_set_rate(s, s5l8930_i2s_rate(s->port));
    }
}

/*
 * The CDMA starts each chain at the port's rate as it is then, and the voice
 * plays at that rate too. 5.x and 6.x's AppleARMIISAudio sets the PMGR NCO for
 * VoiceOver's 22.05 kHz speech, and back, while TX runs: read only at TX
 * start, the voice kept the old rate while the CDMA paced the new one, and the
 * speech played twice as fast, then the next sounds half as fast.
 */
unsigned s5l8930_i2s_sync_rate(unsigned port)
{
    unsigned rate = s5l8930_i2s_rate(port);
    S5L8930I2SState *s;

    if (port >= ARRAY_SIZE(i2s_ports)) {
        return rate;
    }
    s = i2s_ports[port];
    if (s && s->voice && i2s_tx_running(s)) {
        i2s_set_rate(s, rate);
    }
    return rate;
}

static void i2s_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    S5L8930I2SState *s = opaque;

    if (s->mca) {
        if (offset == 0x28) {
            if (s->voice && i2s_tx_running(s)) {
                i2s_push(s, value, size);
            }
            return;
        }
        if (offset == 0x40) { /* native interrupt handler uses W1C */
            s->regs[offset >> 2] &= ~value;
            return;
        }
        if (offset == 0x44 || offset == 0x48) {
            /* Native setup writes 301 to +44 and waits for zero; TX stop
             * writes 1 (80bd60ba). Reset the FIFO, retaining format words. */
            if ((offset == 0x48 && (value & 1)) ||
                (offset == 0x44 && (value & 0x101))) {
                s->regs[0x18 >> 2] &= ~1u;
                s->head = s->tail = s->level = 0;
                if (s->voice) {
                    audio_be_set_active_out(s->audio_be, s->voice, false);
                }
            }
            if ((offset == 0x48 && (value & 1)) ||
                (offset == 0x44 && (value & 0x202))) {
                s->regs[0x2c >> 2] &= ~1u;
                s->in_head = s->in_tail = s->in_level = s->rx_pos = 0;
                if (s->voice_in) {
                    audio_be_set_active_in(s->audio_be, s->voice_in, false);
                }
            }
            return;
        }
        s->regs[offset >> 2] = value;
        if (offset == 0x2c && s->audio_in) {
            bool run = value & 1;
            if (run) {
                i2s_set_in_rate(s, s5l8930_i2s_rate(s->port));
                s->rx_pos = 0;
            }
            if (s->voice_in) {
                audio_be_set_active_in(s->audio_be, s->voice_in, run);
            }
        }
        if (offset == 0x18 && s->audio_out) {
            if (i2s_tx_running(s)) {
                i2s_set_rate(s, s5l8930_i2s_rate(s->port));
            }
            if (s->voice) {
                audio_be_set_active_out(s->audio_be, s->voice, i2s_tx_running(s));
            }
        }
        return;
    }

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
            audio_be_set_active_in(s->audio_be, s->voice_in, run);
        }
    }
    if (offset == (s->ctrl_run ? I2S_CTRL : I2S_TXCOM) && s->voice) {
        if (i2s_tx_running(s)) {
            i2s_set_rate(s, s5l8930_i2s_rate(s->port));
        }
        audio_be_set_active_out(s->audio_be, s->voice, i2s_tx_running(s));
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

    if (s->mca && (s->frame_rate < 8000 || s->frame_rate > 192000)) {
        error_setg(errp, "MCA frame-rate must be between 8000 and 192000 Hz");
        return;
    }
    if (s->port < ARRAY_SIZE(i2s_ports)) {
        i2s_ports[s->port] = s;
    }
    if (!s->audio_out && !s->audio_in) {
        return;
    }
    if (!audio_be_check(&s->audio_be, errp)) {
        warn_report("s5l8930 i2s: no audio backend; output dropped");
        return;
    }
    if (s->audio_out) {
        i2s_set_rate(s, 44100);
    }
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
        audio_be_set_active_out(s->audio_be, s->voice, 0);
    }
    if (s->voice_in) {
        audio_be_set_active_in(s->audio_be, s->voice_in, 0);
    }
}

static void s5l8930_i2s_init(Object *obj)
{
    S5L8930I2SState *s = S5L8930_I2S(obj);

    s->lrclk = qdev_init_clock_in(DEVICE(obj), "lrclk", i2s_lrclk_update, s, ClockUpdate);
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
        audio_be_set_active_out(s->audio_be, s->voice, 1);
    }
    return 0;
}

/* The PCM ring is transient host-side buffering and is not migrated. */
static const VMStateDescription vmstate_s5l8930_i2s = {
    .name = TYPE_S5L8930_I2S,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = s5l8930_i2s_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8930I2SState, I2S_REGS_SIZE / 4),
        VMSTATE_CLOCK_V(lrclk, S5L8930I2SState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8930_i2s_properties[] = {
    DEFINE_PROP_BOOL("audio-out", S5L8930I2SState, audio_out, false),
    DEFINE_PROP_BOOL("audio-in", S5L8930I2SState, audio_in, false),
    DEFINE_PROP_BOOL("ctrl-run", S5L8930I2SState, ctrl_run, false),
    DEFINE_PROP_BOOL("mca", S5L8930I2SState, mca, false),
    DEFINE_PROP_UINT32("frame-rate", S5L8930I2SState, frame_rate, 48000),
    DEFINE_PROP_UINT8("port", S5L8930I2SState, port, 0),
    DEFINE_PROP_UINT64("nco-base", S5L8930I2SState, nco_base, 0),
    DEFINE_PROP_UINT32("nco-ref-hz", S5L8930I2SState, nco_ref_hz, 0),
    DEFINE_PROP_UINT32("tone-hz", S5L8930I2SState, tone_hz, 0),
    DEFINE_PROP_LINK("codec", S5L8930I2SState, codec, TYPE_CS42L58, CS42L58State *),
};

static void s5l8930_i2s_class_init(ObjectClass *klass, const void *data)
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
