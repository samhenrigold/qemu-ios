/*
 * AMC -- the S5L8720's audio media codec ("amc,s5l8720x").
 *
 * IT_AMC_DECODE=1 enables HLE of the 7E18 AAC/HE-AAC/MP3/ALAC programs, with linked DMA,
 * bounded decoding, real PCM output and completion ownership. See
 * docs/ipod-media.md for validation and the remaining limitations.
 * The register-only bring-up history below describes the original stubs.
 *
 * Device tree: /device-tree/arm-io/amc,
 *     reg = <0x00500000 0x00003000  0x1a000000 0x00030000>, interrupts = <0x12>.
 * arm-io's ranges translate those to 0x38500000 (registers, 12 KB) and
 * 0x22000000 (a 192 KB buffer window, which the machine already backs with
 * RAM). Driven by AppleAMC_r2, which owns the IOAudio2 output transformer
 * streams -- i.e. it is on the path of *every* sound the system plays.
 *
 * Why this exists: the register window was not mapped at all. The first access
 * from the driver therefore hit unassigned memory, which QEMU reports as
 * MEMTX_DECODE_ERROR and the ARM core turns into an external data abort -- a
 * kernel-mode abort, so XNU panics and the guest stops dead the moment any
 * sound is played. Even with the window merely present and reading zero the
 * driver still never returns: AppleAMC_r2 waits for its work to finish with an
 * unbounded
 *
 *     do { IOSleep(0); status = read(AMC + 0xa98); }
 *     while ((status & this->pending_mask) == 0);
 *
 * (VA 0xc060c6dc in the 3.1.3 kernelcache; the same shape appears four more
 * times in the driver's self test). There is no timeout and no bail-out, and it
 * runs on the thread that started playback.
 *
 * Historical register-only path (still the default): it is a plain register file plus one
 * piece of behaviour -- an interrupt source reports itself pending as soon as
 * the driver enables it. That is what lets the wait above complete and the
 * driver unwind normally. No audio is produced; the AMC is a hardware AAC/MP3
 * decode/encode engine with its own DE program, MMU and linked-list DMA, and
 * emulating that is a much larger job. Not hanging is the point here.
 *
 * IT_AMC_STATE=1 goes further and answers the handshake. The whole AMC stack
 * starts lazily on the first sound -- a boot with no sound played produces zero
 * AMC accesses -- and what it then runs is the driver's SELF TEST, not a stream.
 * (The kick at 0x984 has lr = 0xc060c6d4, inside the self-test routine, and the
 * 0x938-0x97c descriptor is byte-identical on every cycle and holds no buffer
 * addresses.) Measured against a real sound, Clock > Timer > When Timer Ends >
 * tap a sound, the driver:
 *
 *   - starts engines in banks 0x000 and 0x180 (command 4 -> state 7, 0x10, 0x60)
 *   - fills the descriptor, writes the command at 0x99c, kicks 0x984 then 0x988
 *   - takes the completion interrupt, reads 0xa98/0xb18, acknowledges bit 2 at
 *     0xc48, and tears the job down again
 *   - does that four times, then settles into an IOAudio2 workloop tick writing
 *     the 4-bit selector at 0x1000 (a symptom, not the blocker: ~14.5k writes on
 *     the real arm1176, versus ~870k under -cpu max, which NOPs WFI)
 *
 * and brings the I2S controller up (`enable <- 1`) for the first time, then
 * halts TX. Nothing plays, and the guest's own kernel log says why -- read it
 * with imgtools/klog.py, since -serial only ever carries iBoot:
 *
 *     Assertion failed in ".../AppleAMCDriver_r2.cpp" at line 934   (x4)
 *                                                        line 1095  (x4)
 *                                                        line 1751  (x4)
 *                                                        line 2678  (x4)
 *     AppleEmbeddedAudioDevice: could not start DMA: device is not ready
 *
 * Line 934 is `sp[0x54] == halfword[[this+0x48c]+4] << 1` (935 repeats it for
 * sp[0x58]), and its failure path returns 0xe00002bc (kIOReturnError). The
 * embedded-audio layer then refuses to start DMA, which is precisely why no
 * PL080 channel is ever pointed at the I2S TX FIFO at 0x3ca00010 and no PCM is
 * ever produced.
 *
 * DO NOT try to satisfy that by making a register report a count. That plan was
 * pursued and disproven three ways:
 *
 *   1. The virtual call at [vtable+0x41c] (0xc060c788) that fills the compared
 *      counts reads no AMC register at all -- a full trace of a cycle shows the
 *      last reads before the assert are one R 0a98 and one R 0b18. The position
 *      registers at 0xa44 + n*0x14 are not the input to this assertion.
 *   2. The expected count is ZERO. this = 0xc0ac6800 (cross-checked:
 *      [this+0x40c] = 4, the bit-2 mask the driver acks), [this+0x48c] =
 *      0xea744000, and all 16 bytes there read zero on every one of the four
 *      cycles. Advancing a position register moves AWAY from the expected value.
 *   3. Neither compared slot comes from hardware. 0xc060c788 resolves to
 *      0xc06089d8, which is small enough to read exhaustively, and in BOTH of
 *      its branches it stores the same thing into both slots:
 *      sp[0x54] = sp[0x58] = table[[this+0x7c]], a constant out of the driver's
 *      own __DATA (table at 0xc0626ebc, index 1 here, so 0x1000). The engine
 *      contributes nothing to the left-hand side of either assertion.
 *      (An earlier note here claimed these slots held stale kernel pointers the
 *      results call never wrote. That was wrong -- the values read off the stack
 *      were a later cycle's overwrite, not the value at the assert.)
 *
 * The real contract is MEMORY-SIDE. The engine is expected to write its results
 * into memory it already owns -- see AMC_RESULT_OFFSET in the header: the block
 * at [this+0x48c] is a mapping of physical 0x22028000, a fixed offset inside the
 * buffer aperture, which is why no register ever carries its address. This model
 * wrote nothing anywhere, and that block staying all-zero is the first failure.
 * Line 934 is only where the driver notices.
 *
 * Build from this: the guest does stage real data. The 0x22000000 aperture holds
 * 37672 non-zero bytes -- four 8 KB blocks at +0x10000/+0x14000/+0x18000/
 * +0x1c000 plus ~18 KB over +0x25400..+0x2d500 -- the engine's program and
 * working buffers. (Those +0x10000 offsets match the position accessor's other
 * two out-fields, which resolve to [table+4]+0x10000 and [table+8]+0x10000 on
 * AMC 2.0.) And the consumer at 0xc060e36c is a ring-buffer PCM copy: it takes
 * the 0xa44 position, computes a source address into the aperture, and copies
 * word pairs with an 8/0x10 stride (stereo deinterleave). So a model that
 * genuinely moves bytes and writes a real result block can be honest, where
 * satisfying a comparison would only buy silence.
 *
 * One more correction: the 106 reads of 0xa44 before the kick are NOT a spin.
 * They are a bounded loop (0xc0613344-0xc0613378) over 106 items, the count
 * coming from a call to 0xc0612df4, one read per item, building a scatter list.
 *
 * Two traps that cost real time here: AppleAMC_r2 is ARM (A32), not Thumb --
 * Thumb-decoding these VAs yields plausible nonsense. And the kernelcache in
 * ~/Developer/ipod2g-re is NOT the one that boots (4.5% byte match over this
 * address range; its AppleAMC_r2 is at 0xc02ac000, not 0xc060c000), so every
 * runtime VA must be read out of guest RAM via QMP pmemsave, VA - 0xB8000000.
 */

#include "hw/arm/ipod_touch_amc.h"
#include "hw/core/cpu.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "qemu/timer.h"
#ifdef IT_HAVE_AVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#endif

#define AMC_REG(off) (s->regs[(off) / 4])

static bool amc_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_AMC_TRACE") != NULL;
    }
    return on;
}

#define AMCT(fmt, ...) do { if (amc_trace()) { \
    fprintf(stderr, "[AMC] " fmt "\n", ##__VA_ARGS__); } } while (0)

#ifdef IT_HAVE_AVCODEC
/* ponytail: HLE of the 7E18 AAC-LC/HE-AAC, MP3 and ALAC decoder programs. Other DE
 * programs need their buffer/parameter contract mapped before use. */
typedef enum AMCProgram {
    AMC_UNKNOWN = -1, AMC_MP3, AMC_AAC, AMC_HEAAC, AMC_ALAC,
} AMCProgram;

typedef struct AMCDecoder {
    AVCodecContext *codec;
    AVFrame *frame;
    bool input_pending;
    bool dma_pending;
    bool failed;
    bool error_reported;
    GByteArray *pcm;
    GQueue output_sizes;
    size_t cursor;
    unsigned slot;
    unsigned rate;
    unsigned channels;
    unsigned capacity;
    unsigned buffers;
} AMCDecoder;

static AMCProgram amc_program(IPodTouchAMCState *s)
{
    /* These are DE program entry/configuration words, not app identifiers.
     * Initialization runs before the stream's parameter block is populated.
     * Match the complete nonzero program descriptor, rather than guessing
     * a codec from the first compressed input or the previous stream. */
    if (AMC_REG(0x940) == 0x84007e00 && AMC_REG(0x944) == 0xa000ac40 &&
        AMC_REG(0x960) == 0xc6008000 && AMC_REG(0x964) == 0x84fc8241 &&
        AMC_REG(0x968) == 0xc600c043 && AMC_REG(0x96c) == 0x8464e464 &&
        AMC_REG(0x970) == 0xbf212e73 && AMC_REG(0x974) == 0xc013f7fb) {
        return AMC_HEAAC;
    }
    if (AMC_REG(0x968) != 0xc013f7fb) {
        return AMC_UNKNOWN;
    }
    if (AMC_REG(0x940) == 0x84006e00 && AMC_REG(0x960) == 0xc600b800 &&
        AMC_REG(0x964) == 0x848cba5d) {
        return AMC_AAC;
    }
    if (AMC_REG(0x940) == 0x84004600 && AMC_REG(0x960) == 0xc6005c00 &&
        AMC_REG(0x964) == 0x8544602f) {
        return AMC_MP3;
    }
    if (AMC_REG(0x940) == 0x84007e00 && AMC_REG(0x944) == 0x85808240 &&
        AMC_REG(0x960) == 0xc6008800 && AMC_REG(0x964) == 0x84d49848) {
        return AMC_ALAC;
    }
    return AMC_UNKNOWN;
}

static unsigned amc_output_capacity(AMCProgram codec)
{
    switch (codec) {
    case AMC_AAC: return 4096;
    case AMC_HEAAC: return 8192;
    case AMC_MP3: return 4608;
    case AMC_ALAC: return 16384;
    default: return 0;
    }
}

static void amc_decoder_close(IPodTouchAMCState *s)
{
    AMCDecoder *d = s->decoder;
    if (d) {
        avcodec_free_context(&d->codec);
        av_frame_free(&d->frame);
        g_byte_array_unref(d->pcm);
        g_queue_clear(&d->output_sizes);
        g_free(d);
        s->decoder = NULL;
    }
}

static bool amc_dram(uint32_t addr, size_t size)
{
    return addr >= 0x08000000 && addr < 0x10000000 &&
           size <= 0x10000000 - addr;
}

static void amc_decode_fail(IPodTouchAMCState *s)
{
    AMCDecoder *d = s->decoder;
    if (!d) {
        AMCProgram program = amc_program(s);
        d = g_new0(AMCDecoder, 1);
        d->pcm = g_byte_array_new();
        d->capacity = amc_output_capacity(program);
        d->buffers = program == AMC_ALAC ? 1 : 2;
        s->decoder = d;
    }
    d->failed = true;
    d->input_pending = false;
    d->dma_pending = d->capacity != 0;
    avcodec_free_context(&d->codec);
    av_frame_free(&d->frame);
    /* Output reports failure once the guest releases a buffer. As with a
     * successful input, queued output must precede end-of-input completion.
     * An unknown program has no output buffer through which to report it. */
    AMC_REG(0x100) = 0;
    if (!d->capacity) s->pending |= 0x40000;
}

static bool amc_decode_dma(IPodTouchAMCState *s, uint32_t head)
{
    AMCDecoder *d = s->decoder;
    AVPacket *packet = NULL;
    g_autoptr(GByteArray) input = g_byte_array_new();
    bool ok = false;

    /* Bank 4 linked DMA: next, control (byte count in high half), source.
     * Bound both descriptor walking and total input before touching memory. */
    for (unsigned links = 0; head; links++) {
        uint32_t words[3];
        head &= ~3u;
        if (links == 64 || !amc_dram(head, sizeof(words)) ||
            address_space_read(&address_space_memory, head,
                MEMTXATTRS_UNSPECIFIED, words, sizeof(words))) {
            return false;
        }
        uint32_t next = le32_to_cpu(words[0]);
        uint32_t control = le32_to_cpu(words[1]);
        uint32_t src = le32_to_cpu(words[2]);
        size_t size = control >> 16;
        if (!size || !amc_dram(src, size) ||
            input->len + size > 1024 * 1024) {
            return false;
        }
        unsigned offset = input->len;
        g_byte_array_set_size(input, offset + size);
        if (address_space_read(&address_space_memory, src,
            MEMTXATTRS_UNSPECIFIED, input->data + offset, size)) {
            return false;
        }
        head = next;
    }
    if (!input->len) {
        return false;
    }
    if (!d) {
        AMCProgram program = amc_program(s);
        enum AVCodecID codec_id = program == AMC_MP3 ? AV_CODEC_ID_MP3 :
            program == AMC_ALAC ? AV_CODEC_ID_ALAC :
            program == AMC_UNKNOWN ? AV_CODEC_ID_NONE : AV_CODEC_ID_AAC;
        const AVCodec *codec = avcodec_find_decoder(codec_id);
        d = g_new0(AMCDecoder, 1);
        s->decoder = d;
        d->pcm = g_byte_array_new();
        d->codec = avcodec_alloc_context3(codec);
        d->frame = av_frame_alloc();
        if (!codec || !d->codec || !d->frame) {
            goto done;
        }
        uint8_t config[12];
        static const unsigned rates[] = { 96000, 88200, 64000, 48000, 44100,
            32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
        if (address_space_read(&address_space_memory, AMC_BUF_BASE + 0x2ff00,
            MEMTXATTRS_UNSPECIFIED, config, sizeof(config))) {
            goto done;
        }
        d->capacity = amc_output_capacity(program);
        /* ALAC aliases both client buffers to one hardware output region
         * (7E18 GetOutputBuffers, c0608a34); a second region would overwrite
         * the parameter block at 0x2ff00. */
        d->buffers = program == AMC_ALAC ? 1 : 2;
        if (codec_id == AV_CODEC_ID_AAC) {
            /* Ignoring SBR/PS outside the selected DE program is expected.
             * Keep library diagnostics in the trace; returned decode errors
             * still go through our guest-visible failure path. */
            d->codec->log_level_offset = amc_trace() ? 0 : AV_LOG_DEBUG - AV_LOG_ERROR;
            unsigned frequency = lduw_le_p(config + 6);
            if (program == AMC_HEAAC) {
                /* HE-AAC's program receives the core rate as two halfwords;
                 * SBR doubles it. The raw FIL elements carry SBR side data. */
                unsigned core_rate = ((uint32_t)lduw_le_p(config + 4) << 16) |
                                     frequency;
                for (frequency = 0; frequency < ARRAY_SIZE(rates); frequency++) {
                    if (rates[frequency] == core_rate) {
                        break;
                    }
                }
                if (lduw_le_p(config) != 0x1f || lduw_le_p(config + 8) ||
                    lduw_le_p(config + 10) || frequency == ARRAY_SIZE(rates)) {
                    goto done;
                }
                d->rate = core_rate * 2;
            } else {
                if (lduw_le_p(config) != 7 || frequency >= ARRAY_SIZE(rates)) {
                    goto done;
                }
                d->rate = rates[frequency];
            }
            unsigned config_size = program == AMC_HEAAC ? 7 : 5;
            d->codec->extradata = av_mallocz(config_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!d->codec->extradata) {
                goto done;
            }
            d->codec->extradata_size = config_size;
            d->codec->extradata[0] = (2 << 3) | (frequency >> 1);
            /* libavcodec promotes CPE streams to stereo from their syntax.
             * Starting with stereo gives mono a spurious silent channel. */
            d->codec->extradata[1] = (frequency << 7) | (1 << 3);
            if (program == AMC_HEAAC) {
                unsigned output_frequency;
                for (output_frequency = 0; output_frequency < ARRAY_SIZE(rates);
                     output_frequency++) {
                    if (rates[output_frequency] == d->rate) {
                        break;
                    }
                }
                if (output_frequency == ARRAY_SIZE(rates)) {
                    goto done;
                }
                /* 7E18 negotiates the core channel count even for HE-AAC v2
                 * (mono in the captured PS stream). Explicit SBR on / PS off
                 * keeps the PCM layout consistent with that guest format;
                 * implicit PS instead played interleaved stereo as mono.
                 * Sync extension: 2b7, AOT 5, SBR=1, rate, 548, PS=0. */
                uint64_t extension = (UINT64_C(0x2b7) << 29) |
                    (UINT64_C(5) << 24) | (UINT64_C(1) << 23) |
                    ((uint64_t)output_frequency << 19) | (UINT64_C(0x548) << 8);
                for (unsigned i = 0; i < 5; i++) {
                    d->codec->extradata[2 + i] = extension >> (32 - 8 * i);
                }
            } else {
                /* AudioQueue can select the LC program for an HE stream.
                 * That program decodes only the core; implicit SBR/PS would
                 * change its negotiated rate and output-buffer contract.
                 * Sync extension: 2b7, AOT 5, SBR=0. */
                d->codec->extradata[2] = 0x56;
                d->codec->extradata[3] = 0xe5;
                d->codec->extradata[4] = 0;
            }
        } else if (codec_id == AV_CODEC_ID_ALAC) {
            unsigned element = input->data[0] >> 5;
            unsigned depth = lduw_le_p(config + 4);
            if (lduw_le_p(config) != 0x1f || element > 1 ||
                lduw_le_p(config + 6) > 255 || lduw_le_p(config + 8) > 31 ||
                lduw_le_p(config + 10) > 255 ||
                (depth != 16 && depth != 20 && depth != 24 && depth != 32)) {
                goto done;
            }
            /* The DE parameters carry depth and Rice coding settings. ALAC's
             * first element identifies mono/stereo. Its rate is irrelevant to
             * decompression and absent from this hardware interface: 1 below
             * is only the positive metadata value required by libavcodec.
             * I2S's independent codec clock determines playback speed. */
            d->codec->extradata = av_mallocz(36 + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!d->codec->extradata) {
                goto done;
            }
            uint8_t *cookie = d->codec->extradata;
            d->codec->extradata_size = 36;
            stl_be_p(cookie, 36);
            memcpy(cookie + 4, "alac", 4);
            stl_be_p(cookie + 12, 4096);
            cookie[17] = depth;
            cookie[18] = lduw_le_p(config + 6);
            cookie[19] = lduw_le_p(config + 10);
            cookie[20] = lduw_le_p(config + 8);
            cookie[21] = element + 1;
            stl_be_p(cookie + 32, 1);
        } else {
            if (lduw_le_p(config) != 1) {
                goto done;
            }
        }
        if (avcodec_open2(d->codec, codec, NULL) < 0) {
            goto done;
        }
    }
    packet = av_packet_alloc();
    if (!packet || av_new_packet(packet, input->len) < 0) {
        goto done;
    }
    memcpy(packet->data, input->data, input->len);
    if (avcodec_send_packet(d->codec, packet) < 0) {
        goto done;
    }
    d->input_pending = true;
    ok = true;
    AMCT("decode DMA consumed %u bytes", input->len);
done:
    av_packet_free(&packet);
    return ok;
}

static void amc_decode_drain(IPodTouchAMCState *s)
{
    AMCDecoder *d = s->decoder;
    if (!d || !d->input_pending) {
        return;
    }
    if (d->cursor) {
        g_byte_array_remove_range(d->pcm, 0, d->cursor);
        d->cursor = 0;
    }
    AVFrame *frame = d->frame;
    int err = 0;
    while (d->pcm->len < 65536 &&
           (err = avcodec_receive_frame(d->codec, frame)) == 0) {
        unsigned channels = frame->ch_layout.nb_channels;
        if ((frame->format != AV_SAMPLE_FMT_FLTP &&
             frame->format != AV_SAMPLE_FMT_S16P &&
             frame->format != AV_SAMPLE_FMT_S32P) ||
            (channels != 1 && channels != 2) ||
            (d->rate && frame->sample_rate != d->rate) ||
            (d->channels && channels != d->channels) ||
            frame->nb_samples <= 0 || frame->nb_samples > 4096 ||
            frame->nb_samples * channels * 2 > d->capacity) {
            err = AVERROR_INVALIDDATA;
            break;
        }
        unsigned offset = d->pcm->len;
        d->channels = channels;
        d->rate = frame->sample_rate;
        g_byte_array_set_size(d->pcm, offset + frame->nb_samples * channels * 2);
        for (int i = 0; i < frame->nb_samples; i++) {
            for (unsigned ch = 0; ch < channels; ch++) {
                int sample;
                if (frame->format == AV_SAMPLE_FMT_S16P) {
                    sample = ((const int16_t *)frame->extended_data[ch])[i];
                } else if (frame->format == AV_SAMPLE_FMT_S32P) {
                    sample = ((const int32_t *)frame->extended_data[ch])[i] >> 16;
                } else {
                    float value = ((const float *)frame->extended_data[ch])[i];
                    sample = isfinite(value) ? lrintf(
                        MAX(-1.0f, MIN(value, 32767.0f / 32768.0f)) * 32768) : 0;
                }
                stw_le_p(d->pcm->data + offset + (i * channels + ch) * 2, sample);
            }
        }
        g_queue_push_tail(&d->output_sizes,
            GUINT_TO_POINTER(frame->nb_samples * channels * 2));
        av_frame_unref(frame);
    }
    if (err == AVERROR(EAGAIN)) {
        d->input_pending = false;
    } else if (err < 0) {
        warn_report("AMC: invalid compressed frame (%d)", err);
        amc_decode_fail(s);
    }
}

static void amc_decode_publish(IPodTouchAMCState *s)
{
    AMCDecoder *d = s->decoder;
    uint8_t header[0x12];
    const hwaddr base = AMC_BUF_BASE + AMC_RESULT_OFFSET;
    bool failed = d && d->failed && g_queue_is_empty(&d->output_sizes);
    unsigned bytes = failed ? d->capacity :
        d ? GPOINTER_TO_UINT(g_queue_peek_head(&d->output_sizes)) : 0;
    if (!bytes || d->error_reported || (!failed && d->pcm->len - d->cursor < bytes) ||
        address_space_read(&address_space_memory, base, MEMTXATTRS_UNSPECIFIED,
                           header, sizeof(header))) {
        return;
    }
    /* These are alternating buffers of interleaved S16 samples, not L/R
     * planes. The completion handler advances one buffer per interrupt. */
    if ((s->pending & 4) || lduw_le_p(header + 0xa + d->slot * 4)) {
        return;
    }
    if (failed) {
        uint8_t error[4];
        /* c0609b5c reads status bit 0 and this code from +0x2ff28, passing it
         * to the output stream. 100 is the driver's own "no output" error
         * (c0609c58). Supply silence, never stale PCM, for that failed frame. */
        stw_le_p(error, 1);
        stw_le_p(error + 2, 100);
        address_space_write(&address_space_memory, AMC_BUF_BASE + 0x2ff28,
                            MEMTXATTRS_UNSPECIFIED, error, sizeof(error));
        g_byte_array_set_size(d->pcm, bytes);
        memset(d->pcm->data, 0, bytes);
        d->cursor = 0;
    }
    if (address_space_write(&address_space_memory,
        base + 0x100 + d->slot * d->capacity, MEMTXATTRS_UNSPECIFIED,
        d->pcm->data + d->cursor, bytes)) {
        return;
    }
    stw_le_p(header + 2, d->buffers);
    stw_le_p(header + 4, d->capacity / 2);
    stw_le_p(header + 0xa + d->slot * 4, 1);
    stw_le_p(header + 0xc + d->slot * 4, bytes / 2);
    if (address_space_write(&address_space_memory, base, MEMTXATTRS_UNSPECIFIED,
                            header, sizeof(header))) {
        return;
    }
    d->cursor += bytes;
    if (failed) {
        d->error_reported = true;
    } else {
        g_queue_pop_head(&d->output_sizes);
    }
    d->slot = (d->slot + 1) % d->buffers;
    s->pending |= 4;
    AMCT("decode output %u samples, remaining %zu", bytes / 2,
         d->pcm->len - d->cursor);
}

#else
static void amc_decoder_close(IPodTouchAMCState *s) {}
static bool amc_decode_dma(IPodTouchAMCState *s, uint32_t head) { return false; }
static void amc_decode_publish(IPodTouchAMCState *s) {}
static void amc_decode_drain(IPodTouchAMCState *s) {}
#endif

/* Initialization publishes the number and capacity of the two output buffers.
 * The driver compares this capacity against its decoder-program table before
 * it supplies any compressed input. Stream completions separately report the
 * actual sample count at header +0xc / +0x10. */
#define AMC_SELFTEST_SAMPLES 0x800

static void amc_write_result_block(IPodTouchAMCState *s)
{
    hwaddr base = AMC_BUF_BASE + AMC_RESULT_OFFSET;
    uint16_t capacity = AMC_SELFTEST_SAMPLES;
    uint16_t buffers = 2;
    uint16_t back = 0;
    const char *env = getenv("IT_AMC_FRAMES");

#ifdef IT_HAVE_AVCODEC
    if (s->codec_decode) {
        AMCProgram program = amc_program(s);
        if (program == AMC_UNKNOWN) {
            warn_report("AMC: unsupported decoder program");
        }
        capacity = amc_output_capacity(program) / 2;
        buffers = program == AMC_UNKNOWN ? 0 : program == AMC_ALAC ? 1 : 2;
    }
#endif

    if (env) {
        capacity = (uint16_t)strtoul(env, NULL, 0);
    }

    /* Belt and braces: the block must sit inside the aperture we documented. */
    QEMU_BUILD_BUG_ON(AMC_RESULT_OFFSET + 0x100 > AMC_BUF_SIZE);

    address_space_write(&address_space_memory, base + AMC_RESULT_BUFFERS,
                        MEMTXATTRS_UNSPECIFIED, &buffers, sizeof(buffers));
    address_space_write(&address_space_memory, base + AMC_RESULT_CAPACITY,
                        MEMTXATTRS_UNSPECIFIED, &capacity, sizeof(capacity));

    address_space_read(&address_space_memory, base + AMC_RESULT_CAPACITY,
                       MEMTXATTRS_UNSPECIFIED, &back, sizeof(back));
    if (back != capacity) {
        warn_report_once("ipod amc: result block at 0x%" HWADDR_PRIx
                         " did not take the write (read back 0x%04x, wanted "
                         "0x%04x) -- is the buffer aperture still RAM?",
                         base, back, capacity);
    }
    AMCT("result block %" HWADDR_PRIx ": buffers %u capacity %u",
         base, buffers, capacity);
}

/*
 * IT_AMC_PC=<hex offset> logs the guest PC and LR for accesses to that register,
 * which is how you find out *which* driver loop is hammering it. Capped so a
 * spin does not fill the disk.
 */
static void amc_log_caller(hwaddr addr, uint32_t val)
{
    static int64_t want = -2;
    static int budget = 64;
    CPUARMState *env;

    if (want == -2) {
        const char *v = getenv("IT_AMC_PC");
        want = v ? strtoll(v, NULL, 16) : -1;
    }
    if (want < 0 || (hwaddr)want != addr || budget <= 0 || !current_cpu) {
        return;
    }
    budget--;
    env = &ARM_CPU(current_cpu)->env;
    /*
     * Walk the r7 frame chain. This driver is ARM, not Thumb, and every
     * non-leaf function opens with `push {r4-r7, lr}; add r7, sp, #0xc`, so a
     * frame is { r4, r5, r6, saved r7, saved lr } with r7 pointing at the
     * saved r7. That means the caller's callee-saved registers are readable
     * too, which is the only way to get at the AppleAMC_r2 instance: the self
     * test holds it in r5, and r5 is long gone by the time the register access
     * happens several frames deeper. Each entry prints as lr/r5.
     */
    fprintf(stderr, "[AMC] %04x <- %08x  pc=%08x lr=%08x  stack:",
            (unsigned)addr, val, env->regs[15], env->regs[14]);
    uint32_t fp = env->regs[7];
    for (int i = 0; i < 8 && fp; i++) {
        uint32_t frame[5] = { 0 };      /* r4, r5, r6, r7, lr */
        if (cpu_memory_rw_debug(current_cpu, fp - 12, (uint8_t *)frame,
                                sizeof(frame), false) != 0) {
            break;
        }
        fprintf(stderr, " %08x/r5=%08x", frame[4], frame[1]);
        if (frame[3] <= fp) {
            break;              /* not a plausible frame chain any more */
        }
        fp = frame[3];
    }
    fprintf(stderr, "\n");

    /*
     * IT_AMC_DEREF=<hex>[,<hex>...]: at the probed access, treat r5 as the
     * AppleAMC_r2 instance and dump each [r5 + <hex>], following it one level
     * if it looks like a pointer. The self test's final assertions
     * (AppleAMCDriver_r2.cpp lines 934/935) compare the bytes the engine
     * produced against halfword [[r5+0x48c]+4] << 1, so that is where the
     * expected count lives -- but print the raw word too, because a deref that
     * merely reads zeroes is indistinguishable from a wrong r5 otherwise.
     */
    /*
     * IT_AMC_FRAME=<hex lr> walks out to the frame whose saved lr is that
     * value and dumps the 32 words below its r7. The self test
     * (0xc060c4d0: push {r4-r7,lr}; add r7,sp,#0xc; push {r8,sl,fp};
     * sub sp,sp,#0x68) therefore has its locals at r7-0x80 + n, so its sp[0x54]
     * and sp[0x58] -- the two counts assertion 934/935 tests -- land at r7-0x2c
     * and r7-0x28. Nothing writes those until after the last AMC access of a
     * cycle, so read them at the FIRST access of the next cycle: the self test
     * is re-entered from the same call site, so the frame is at the same
     * address and the slot still holds the previous cycle's value.
     */
    const char *want_frame = getenv("IT_AMC_FRAME");
    if (want_frame) {
        /* Walk to the frame whose saved lr is <hex> -- that frame is the
         * caller we care about -- and dump the 32 words below its r7. */
        uint32_t target = strtoul(want_frame, NULL, 16);
        uint32_t fp = env->regs[7];
        for (int i = 0; i < 8 && fp; i++) {
            uint32_t frame[5] = { 0 };
            if (cpu_memory_rw_debug(current_cpu, fp - 12, (uint8_t *)frame,
                                    sizeof(frame), false) != 0) {
                break;
            }
            if (frame[4] == target) {
                uint32_t words[32];
                if (cpu_memory_rw_debug(current_cpu, fp - sizeof(words),
                                        (uint8_t *)words, sizeof(words),
                                        false) == 0) {
                    fprintf(stderr, "[AMC] frame r7=%08x:", fp);
                    for (unsigned j = 0; j < ARRAY_SIZE(words); j++) {
                        fprintf(stderr, " %d:%08x",
                                (int)(j * 4) - (int)sizeof(words), words[j]);
                    }
                    fprintf(stderr, "\n");
                }
                break;
            }
            if (frame[3] <= fp) {
                break;
            }
            fp = frame[3];
        }
    }

    /*
     * IT_AMC_V2P=<hex va>[,...]: print the physical page behind a guest kernel
     * VA. The driver's result buffer lives in the kernel map (0xea74....), well
     * outside the 1:1 window, so a pmemsave dump cannot reach it and neither
     * can any offline analysis -- but the only question that matters about it
     * is whether its physical address is one the engine was ever told.
     */
    /* IT_AMC_MEM=<hex va>[,...]: 64 bytes of guest memory at each VA. */
    const char *mem = getenv("IT_AMC_MEM");
    for (const char *p = mem; p && *p; ) {
        uint64_t va = strtoull(p, NULL, 16);
        uint8_t buf[64];
        if (cpu_memory_rw_debug(current_cpu, va, buf, sizeof(buf), false) == 0) {
            fprintf(stderr, "[AMC] mem %08x:", (uint32_t)va);
            for (unsigned i = 0; i < sizeof(buf); i++) {
                fprintf(stderr, "%02x", buf[i]);
            }
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "[AMC] mem %08x: <unreadable>\n", (uint32_t)va);
        }
        p = strchr(p, ',');
        if (p) {
            p++;
        }
    }

    const char *v2p = getenv("IT_AMC_V2P");
    if (v2p) {
        fprintf(stderr, "[AMC] v2p:");
        for (const char *p = v2p; p && *p; ) {
            uint64_t va = strtoull(p, NULL, 16);
            hwaddr pa = cpu_get_phys_page_debug(current_cpu, va & ~0xfffULL);
            fprintf(stderr, " %08x->%08x", (uint32_t)va,
                    pa == -1 ? 0xffffffffu
                             : (uint32_t)(pa | (va & 0xfff)));
            p = strchr(p, ',');
            if (p) {
                p++;
            }
        }
        fprintf(stderr, "\n");
    }

    const char *deref = getenv("IT_AMC_DEREF");
    if (deref) {
        fprintf(stderr, "[AMC] r4=%08x r5=%08x r6=%08x sl=%08x",
                env->regs[4], env->regs[5], env->regs[6], env->regs[10]);
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
 * Level-triggered, with a real acknowledge path so it cannot storm: the line
 * only goes high once the driver has both enabled a source and touched a
 * control register, and any write to the acknowledge register drops it again.
 */
static void amc_update_irq(IPodTouchAMCState *s)
{
    /*
     * IT_AMC_STATE: the driver never writes the enable register (0xa8c) -- it
     * only ever *disables* everything at init -- so int_mask stays 0 and the
     * line could never rise. That leaves the completion interrupt permanently
     * unserviced, which is what strands the stream: the engine's workloop tick
     * keeps running but nothing ever advances it. Under the handshake, an armed
     * job raises the line regardless of the mask and the acknowledge drops it.
     */
    bool level = s->codec_decode ? (s->pending & s->int_mask[0]) != 0 :
                 s->irq_armed &&
                 (s->state_handshake ||
                  s->int_mask[0] != 0 || s->int_mask[1] != 0);

    qemu_set_irq(s->irq, level);
}

static void amc_decode_tick(void *opaque)
{
    IPodTouchAMCState *s = opaque;
    amc_decode_drain(s);
#ifdef IT_HAVE_AVCODEC
    AMCDecoder *d = s->decoder;
    if ((AMC_REG(0x100) & 1) &&
        (!d || (!d->input_pending && !d->dma_pending && !d->failed))) {
        if (amc_decode_dma(s, AMC_REG(0x100))) {
            AMC_REG(0x100) = 0;
            ((AMCDecoder *)s->decoder)->dma_pending = true;
            amc_decode_drain(s);
        } else {
            warn_report("AMC: DMA decode failed");
            amc_decode_fail(s);
        }
    }
#endif
    amc_decode_publish(s);
#ifdef IT_HAVE_AVCODEC
    d = s->decoder;
    /* AudioQueue treats completion of its final input as end-of-stream.
     * Copying compressed bytes into libavcodec is not completion: publish
     * their decoded output before returning that input DMA to the guest. */
    if (d && d->dma_pending && !d->input_pending &&
        (!d->failed || d->error_reported) &&
        g_queue_is_empty(&d->output_sizes)) {
        d->dma_pending = false;
        s->pending |= 0x40000;
    }
#endif
    amc_update_irq(s);
    timer_mod(s->decode_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

/* Returns the controller index for a per-controller register, or -1. */
static int amc_ctrl_of(hwaddr addr, hwaddr base)
{
    if (addr == base) {
        return 0;
    }
    if (addr == base + AMC_CTRL_STRIDE) {
        return 1;
    }
    return -1;
}

/*
 * If addr is an engine's command register, return that engine's bank base;
 * otherwise -1. See the bank list in the header.
 */
static const uint32_t amc_banks[] = { 0x00, 0x20, 0x40, 0x60,
                                      0x100, 0x180, 0x200, 0x230 };

static int amc_bank_cmd(hwaddr addr)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(amc_banks); i++) {
        if (addr == amc_banks[i] + AMC_BANK_CMD) {
            return amc_banks[i];
        }
    }
    return -1;
}

static uint64_t ipod_touch_amc_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(opaque);
    uint32_t res;
    int c;

    if ((c = amc_ctrl_of(addr, AMC_INT_MASK)) >= 0) {
        res = s->int_mask[c];
    } else if ((c = amc_ctrl_of(addr, AMC_INT_STATUS)) >= 0 ||
               (c = amc_ctrl_of(addr, AMC_INT_RAWSTATUS)) >= 0) {
        /*
         * Every enabled source reads back as pending. The driver's wait loops
         * are all of the form "spin until (status & my_mask) != 0", so this is
         * the minimum that lets them finish; reporting only what we could
         * genuinely justify (nothing) is what hangs them.
         */
        res = s->codec_decode ? (c ? 0 : s->pending) : s->int_mask[c];
        /*
         * IT_AMC_STATE: the driver's final wait before a stream can move is
         *
         *     do { IOSleep(0); } while ((read(0xa98) & pending_mask) == 0);
         *
         * and it reaches it having only ever written the *disable* register
         * (0xa90 <- 0x07ffffff, 0xb10 <- 0xffffffff) -- it never enables a
         * source, so int_mask is 0 and the mask-mirroring above answers 0 and
         * the loop never ends. We do not know which bit it is waiting on, so
         * report every source pending; whatever the mask is, it is satisfied.
         *
         * STILL REQUIRED, re-measured after the result block landed. Once the
         * self test passes, the driver does finally write the real enable
         * register 0xa8c -- which looked like it might retire this fake. It
         * does not: that write happens *after* the wait above, so the wait is
         * still unsatisfiable without us. Measured with IT_AMC_NOFAKE=1, one
         * variable, same warm overlay: 696 AMC accesses with the fake, and
         * 1,716,145 with it off, with the I2S controller never brought up at
         * all. Do not delete this without re-running that A/B.
         */
        if (!s->codec_decode && s->state_handshake && res == 0 && c == 0 &&
            !getenv("IT_AMC_NOFAKE")) {
            /* Controller 0 only: reporting controller 1 pending as well made
             * the driver service a source that does not exist. */
            res = s->irq_armed ? 0xffffffff : 0;
        }
    } else {
        res = AMC_REG(addr);
    }

    AMCT("R %04x -> %08x", (unsigned)addr, res);
    amc_log_caller(addr, res);
    return res;
}

static void ipod_touch_amc_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(opaque);
    int c;

    AMCT("W %04x <- %08x", (unsigned)addr, (uint32_t)val);
    amc_log_caller(addr, (uint32_t)val);
    AMC_REG(addr) = (uint32_t)val;

    if ((c = amc_ctrl_of(addr, AMC_INT_ENABLE)) >= 0) {
        s->int_mask[c] |= (uint32_t)val;
    } else if ((c = amc_ctrl_of(addr, AMC_INT_DISABLE)) >= 0) {
        s->int_mask[c] &= ~(uint32_t)val;
    } else if (addr == AMC_INT_ACK) {
        s->irq_armed = false;      /* acknowledged -- drop the line */
        s->pending &= ~(uint32_t)val;
    } else if (s->state_handshake && amc_bank_cmd(addr) >= 0) {
        /*
         * IT_AMC_STATE=1, experimental -- NOT part of the proven freeze fix.
         *
         * At VA 0xc0612854 the driver writes command 4 to an engine's command
         * register and then polls that engine's state register up to ten times
         * for (state & 7) == 7 before giving up; at VA 0xc06129f0 it requires
         * (state & 7) == 0 before issuing command 0x10. Neither can be met by a
         * register that only reads back what was written, so answer them: a
         * start command completes immediately and any later command clears it.
         *
         * Measured: with only engine 4 (bank 0x100) answered, the driver got
         * its 7 first try and advanced to commands 0x10 and 0x60, then stalled
         * polling engine 5 (bank 0x180, state at 0x194) 24 times. Hence the
         * full bank table rather than a single hard-coded pair.
         */
        AMC_REG(amc_bank_cmd(addr) + AMC_BANK_STATE) =
            (val == AMC_CMD_START) ? AMC_STATE_DONE : 0;
    } else if (!s->state_handshake) {
        s->irq_armed = true;       /* some work was started */
    } else if (addr == AMC_JOB_GO || addr == AMC_JOB_GO2 || addr == AMC_JOB_CMD) {
        /*
         * Only a job start arms the line. Arming on *any* write (which is all
         * the freeze fix needed) turns the completion interrupt into a storm
         * once the line can actually rise: the handler's own register writes
         * (0x804, 0x858) re-arm it the instant it acknowledges at 0xc48.
         *
         * The job is the 0x938-0x97c descriptor block followed by 0x99c and
         * then 0x984/0x988; those three are the only writes that mean "go".
         */
        s->irq_armed = true;
        if (addr == AMC_JOB_GO) {
            /*
             * We report completion immediately, so the results the engine would
             * have DMAed have to be in place by the time the driver looks --
             * i.e. now, not at the acknowledge. Only on the first of the two
             * kick registers, so a job writes its block once.
             */
            amc_write_result_block(s);
            if (s->codec_decode) {
                s->pending |= 4;
            }
        }
    }

    if (s->codec_decode) {
        if (addr == AMC_JOB_CMD) {
            uint8_t clear[0x12] = { 0 };
            amc_decoder_close(s);
            s->pending = 0;
            AMC_REG(0x100) = 0;
            address_space_write(&address_space_memory,
                AMC_BUF_BASE + AMC_RESULT_OFFSET, MEMTXATTRS_UNSPECIFIED,
                clear, sizeof(clear));
            address_space_write(&address_space_memory, AMC_BUF_BASE + 0x2ff28,
                MEMTXATTRS_UNSPECIFIED, clear, 4);
            /* A new stream must not inherit a failure or DMA descriptor
             * from the previous stream. */
            timer_mod(s->decode_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
        } else if (addr == 0x110 && val == 0x20) {
            /* Bank 4 acknowledges its completed linked DMA before rearming. */
            s->pending &= ~0x40000u;
        }
    }
    amc_update_irq(s);
}

static const MemoryRegionOps amc_ops = {
    .read = ipod_touch_amc_read,
    .write = ipod_touch_amc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_amc_reset(DeviceState *dev)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(dev);

    amc_decoder_close(s);
    timer_del(s->decode_timer);
    s->pending = 0;
    s->codec_decode = getenv("IT_AMC_DECODE") != NULL ||
                      getenv("IT_AMC_AAC") != NULL;
#ifndef IT_HAVE_AVCODEC
    if (s->codec_decode) {
        warn_report("AMC: this build has no libavcodec support");
        s->codec_decode = false;
    }
#endif
    if (s->codec_decode) {
        timer_mod(s->decode_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->int_mask, 0, sizeof(s->int_mask));
    s->irq_armed = false;
    s->state_handshake = s->codec_decode || getenv("IT_AMC_STATE") != NULL;
    amc_update_irq(s);
}

static void ipod_touch_amc_init(Object *obj)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &amc_ops, s, "amc", AMC_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->decode_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, amc_decode_tick, s);
}

static void ipod_touch_amc_finalize(Object *obj)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(obj);
    timer_free(s->decode_timer);
    amc_decoder_close(s);
}

static void ipod_touch_amc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ipod_touch_amc_reset);
}

static const TypeInfo ipod_touch_amc_info = {
    .name          = TYPE_IPOD_TOUCH_AMC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchAMCState),
    .instance_init = ipod_touch_amc_init,
    .instance_finalize = ipod_touch_amc_finalize,
    .class_init    = ipod_touch_amc_class_init,
};

static void ipod_touch_amc_register_types(void)
{
    type_register_static(&ipod_touch_amc_info);
}

type_init(ipod_touch_amc_register_types)
