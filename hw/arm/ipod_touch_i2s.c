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
 * THE MEASUREMENT THAT SETTLED IT. The DMA request the audio stack builds and
 * submits is fully formed and describes a REAL transfer (61440 bytes, 15
 * periods of 4096, with a completion routine attached). It is then never
 * executed.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE FAILURE ACTUALLY IS (measured 2026-08-03, IT_DMAC_TRACE +
 * IT_I2S_PC + a live RAM dump; every address below is from the running guest,
 * not from the kernelcache in ~/Developer/ipod2g-re, which is a different
 * image and whose addresses do not apply)
 *
 * 1. THE DMA CONTROLLER IS NEVER ASKED ANYTHING. IT_DMAC_TRACE=1 logs every
 *    PL080 read and write on both controllers with the guest PC, interleaved
 *    with this device's accesses in one stream. Across the whole I2S bring-up
 *    -- before it, during it, after it -- there are ZERO PL080 accesses. Every
 *    PL080 access in an entire boot comes from one accessor pair (read
 *    c0750cd4 / write c0750d08) and belongs to UART1 (0x3db00024) on DMAC0 and
 *    SPI4 (0x3e100010) on DMAC1.
 *
 *    So the standing hypothesis -- that this tree's "nothing can fail" default
 *    was feeding the DMAC kext a register value that made it give up -- is
 *    DISPROVEN. It reads no register. No change to hw/dma/pl080.c can fix this
 *    bug, and the PL080 model is exonerated.
 *
 * 2. THE ERROR HAS A NAME. `AppleARMIISAudioDevice: could not start DMA:
 *    device is not ready` is `%s: could not start DMA: %s` (format string at
 *    c0506be4, referenced from the literal pool at c0505acc) with the second
 *    %s coming from the KERNEL's own IOReturn->string table: the entry at
 *    c01fa000 pairs "device is not ready" with 0xE00002D8, i.e.
 *    kIOReturnNotReady. Not a guess -- the table is right there, between
 *    0xE00002D7 "device is offline" and 0xE00002D9 "device/channel is not
 *    attached".
 *
 * 3. THE FULL CALL CHAIN, recovered live from the r7 frame walk on the I2S
 *    enable write (IT_I2S_PC=0). Innermost last:
 *
 *      c0505a6c   AppleARMIISAudio start; logs the message. r4 = this = c0c6a800
 *      c050554c   after `bl c05054f8`; [this+0x74] = 2, so output only
 *      c0505480   c05053ec, per-direction
 *      c05053a8   c0505340: takes [this+0x98] lock, calls the DMA controller's
 *                 [vtable+0x358] and returns whatever it returns
 *      c0318c49   c0318b90 (THUMB) on controller c0bfdb00, vtable c032267c:
 *                 allocates the request, fills +0x54..+0x7c, and hands it to a
 *                 command gate
 *      c018a659   IOCommandGate::runAction
 *      c0318581   the gated action c0318536, on channel object c0b7f500:
 *                 calls [vt+0x380] (which is what brings I2S up), bumps
 *                 [this+0x54], then calls [vt+0x358]
 *      c0318533 / c0565c94   the I2S register writes themselves
 *
 *    c0b7f500's vtable is c056f4c8 -- in the I2S kext -- and its [vt+0x358] is
 *    c0565928. That routine dispatches on [req+0x5c] (2 = output) to
 *    [[this+0x7c] vtable+0x84] = c018b358 = IODMAEventSource::startDMACommand,
 *    whose object c0b99d80 carries [+0x28] = the AppleARMPL080DMAC instance
 *    c0a67c00 and [+0x2c] = DMA channel 5. That in turn calls the controller's
 *    [vtable+0x354] = c07500e0.
 *
 * 4. TWO CANDIDATE PRODUCERS OF kIOReturnNotReady, and one of them is RULED
 *    OUT by measurement:
 *
 *    (a) c075013c in AppleARMPL080DMAC (ARM, not Thumb -- disassembling this
 *        kext as Thumb yields plausible garbage). The only site in that kext
 *        that materialises 0xE00002D8. Its test is
 *            r2 = [controller + 0x64*channel + 0x98]; if (r2 <= 1) return NotReady
 *        i.e. a purely software per-channel state, checked before any register
 *        is touched -- which is why the hardware never sees anything.
 *        RULED OUT: sampled 0.6 s and 2.1 s either side of a failure, the
 *        channel states are [0,0,3,3,2,2,3,0] and do not move; channel 5 reads
 *        2, so the branch is not taken. (State meanings, from the setters:
 *        1 at c07508b8 = allocated, 2 at c0750a34/c0751128 = configured, 3 at
 *        c07503c4 = has a live request.)
 *
 *    (b) c0565974 in the I2S kext, which preloads r6 = 0xE00002D8 as the
 *        DEFAULT result of a prime-and-wait block: it sets [this+0x8c] = 1,
 *        pokes [[this+0x70] vt+0x220], arms a timeout via [[this+0x84] vt+0x94]
 *        and sleeps on [this+0x8c] until it reads 3 or 4. A timeout overwrites
 *        r6 with 0xE00002EB, so 0xE00002D8 survives only if the block ends in
 *        state 4 WITHOUT our timeout firing. Note the entry guard at c0565954
 *        skips the whole block when [req+0x6c] > 999999999, and c0318c20 sets
 *        [req+0x6c] = -1 when the caller passes no deadline.
 *
 *    So the remaining question is narrow and concrete: which of (a)'s deeper
 *    paths or (b) produces the code, and what does [this+0x8c] have to be told.
 *    The cheapest next instrument is a hit probe on c0565974 and c075013c --
 *    neither is reachable from a device model, so it wants gdbstub or a TCG
 *    hook rather than another MMIO trace.
 *
 * 5. DEVICE TREE, for reference (DeviceTree.nowdt.bin, parsed offline):
 *    i2s0's dma-parent is dmac0's phandle, and dma-channels encodes PL080
 *    peripheral request lines, not channel numbers: TX word 0x00000a80 =
 *    flow 1 (mem->periph), dest peripheral 10, FIFO 0x3ca00010; RX word
 *    0x00001056 = flow 2 (periph->mem), src peripheral 11, FIFO 0x3ca00038.
 *    The channel the audio stack actually asks for at runtime is 5.
 *
 * HOW TO MAKE THE GUEST PLAY A SOUND AT ALL, which blocked this for hours:
 * the guest plays NOTHING until something asks it to, and typing on the
 * on-screen keyboard produced no audio (keyboard clicks appear to be off on
 * these images). What works, and needs no touch: the screenshot shutter, i.e.
 * Home and Power held together. `send-key` presses and releases as a unit, so
 * they have to be driven as individual key events -- meta_l+shift down, h
 * down, l down, hold ~0.35 s, release. Touch itself only works on
 * nand-7e18-final with a FRESH overlay; nand-grow7g with a reused baseline
 * overlay delivers frames the digitizer reads and the UI ignores entirely.
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
    device_class_set_legacy_reset(dc, ipod_touch_i2s_reset);
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
