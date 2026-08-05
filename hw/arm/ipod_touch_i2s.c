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
 * samples and looks exactly like a broken audio path.
 * contrib/run-ipod-touch.sh --sound (or your local
 * ~/Developer/qemu-ios-files/ios3/run-ios3.sh --sound) sets this up for you.
 *
 * Sample format: 44100 Hz, 16-bit, stereo, little-endian. IT_I2S_RATE overrides.
 * This is no longer an assumption -- it is confirmed from the guest's own
 * behaviour, two ways. photoShutter.caf is lpcm 44100/mono/16, and what arrives
 * at this FIFO is that file's samples BIT FOR BIT with each one duplicated into
 * both channels: iOS resamples every clip to the hardware rate, so a hardware
 * rate other than 44100 would have left resampled samples here, not the file's.
 * And the guest's own ring producer refills its 16-page ring every 371.6 ms,
 * which is 16 x 1024 frames at 44.1 kHz.
 *
 * ---------------------------------------------------------------------------
 * FIDELITY, SETTLED: THE SHUTTER IS NOW SAMPLE-EXACT. (2026-08-03, later still)
 *
 * The sound was audible but wrong -- "recognisable but garbled". Measured
 * against the source PCM rather than listened to, the transformation was:
 *
 *     out = src[0..22122]                      the whole clip, sample-exact
 *         + a replay of the ring's last 15 periods (15360 frames, 0.348 s)
 *         + single torn samples, ~4000 in 1.5 M frames
 *         + whole streams with left and right swapped and skewed one sample
 *
 * Nothing was wrong with the rate, the width, the signedness, the endianness or
 * the channel layout: the clip itself came through byte-identical to the file
 * on the guest's own disk from the very first run. Three defects produced the
 * rest, and all three are ours:
 *
 * 1. hw/dma/pl080.c raised each terminal-count interrupt from the I bit of the
 *    NEXT descriptor rather than the one that had just finished, so every
 *    LLI-chained interrupt fired one descriptor early. The audio engine reads
 *    "terminal count with LLIx == 0" as "the DMA has run out of work" and hands
 *    over the head of its next batch; a descriptor early, the batch it hands
 *    over is the one still in flight, and the channel walks it again. That is
 *    the 61440-byte replay, exactly. (An upstream QEMU bug, not one of ours.)
 *
 * 2. This FIFO was modelled 8192 bytes deep -- 46 ms, two of the guest's 4096-
 *    byte periods -- and the depth is precisely how far the DMA's read head
 *    runs ahead of the sound leaving the device. The engine refills the ring
 *    just behind that head, so at 8192 the head sat ON the producer: samples
 *    read out of a page mid-write, and after a clip the engine's silence-fill
 *    lost the race and pages of the clip played twice. 2048 restores the
 *    margin. See the header.
 *
 * 3. A stream stops wherever Config = 0 lands, so it can end having delivered
 *    an odd number of 16-bit samples; with no LRCLK in our byte stream that
 *    swapped the channels for every stream after it. it_i2s_align_frame() pads
 *    the partial frame when the driver resets the TX FIFO.
 *
 * With all three: eight consecutive shutters, all 22121 frames, all identical
 * to photoShutter.caf's PCM, L == R on every frame, peak 5615 = the file's own
 * peak, delivered in 0.5018-0.5032 s of guest time against an ideal 0.50161 s
 * (within 0.3%) with no inter-element gap over 3.5 ms. The wav capture is
 * byte-identical to this FIFO tap, so the host path adds nothing.
 *
 * ---------------------------------------------------------------------------
 * SOLVED, END TO END: THE GUEST NOW PLAYS AUDIBLE SOUND. (2026-08-03, later)
 *
 * A screenshot shutter produces 693378 bytes at this FIFO with peak amplitude
 * 5615 and two transient bursts with a decay -- the shape of a camera shutter,
 * not silence. Captured through `-audio driver=wav`.
 *
 * The last blocker was DMAC0's completion interrupt, and it was two of our own
 * defects stacked, both in the "nothing in this tree can fail" family:
 *
 *   1. The UART receive DMA fabricated completions. The kernel's serial driver
 *      leaves a 2048-byte peripheral-to-memory channel armed on the console
 *      UART's URXH with the terminal-count interrupt enabled. hw/dma/pl080.c
 *      ignored the peripheral request lines, so all 2048 bytes were "received"
 *      from an empty FIFO inside the Config write and terminal count fired for
 *      a read that never happened. The request lines are now declared paced in
 *      ipod_touch_2g.c, so an idle console produces nothing, as on real
 *      hardware.
 *   2. The interrupt masks were sticky. pl080_run() seeded tc_mask from tc_int,
 *      so once a terminal count was pending it could never be masked out again
 *      and the line stayed high forever. The masks are now derived live from
 *      the channels' own Configuration words (pl080_refresh_masks()).
 *
 * With those two fixed, DMAC0 goes on VIC line 0x10 -- which is what our own
 * DeviceTree says (/arm-io/dmac0 interrupts = <0x10>, dmac1 = <0x11>); they
 * were never a shared line and no OR gate is involved. Single-variable A/B on
 * the same binary and the same warm overlay, shutter fired on both:
 *
 *      IT_DMAC0_IRQ=0x11   36864 samples, peak 0,     0 DMAC0 IntStatus reads
 *      IT_DMAC0_IRQ=0x10  346689 samples, peak 5615, 19 reads, 27 IntTCClear
 *
 * Boot and reboot are unaffected (lit 0.5449 / 0.5450, against 0.5461 before).
 *
 * Everything below this line predates that fix. Sections 4 and 5 of the next
 * block, and the "not yet fixable" note in section 4, are SUPERSEDED: the
 * producer did not have a second problem, and the wedge on 0x10 was the
 * fabricated UART completion, not anything in the guest.
 *
 * ---------------------------------------------------------------------------
 * SOLVED: THE DMA PATH IS OPEN. PCM NOW REACHES THIS FIFO. (2026-08-03)
 *
 * The blocker described in the rest of this comment -- "the DMA request is
 * built and submitted and never executed" -- is FIXED, and the fix is the
 * ready interrupt above. A screenshot shutter now programs PL080 channel 5
 * (src 0x08c99000 -> dst 0x3ca00010, 2048 halfwords per period, LLI-chained)
 * and 73728 bytes per sound arrive here and are handed to the backend.
 * `AppleARMIISAudio`'s channel start returns kIOReturnSuccess instead of
 * kIOReturnNotReady.
 *
 * HOW IT WAS FOUND, since the previous rounds of this investigation kept
 * blaming the wrong layer. A gdbstub breakpoint on the gated channel-start
 * action c0565928, hit live while firing the shutter:
 *
 *   - [this+0x8a] = 1, so the prime-and-wait block runs; [this+0x8c] = 0.
 *   - The "prime" at [[this+0x70] vt+0x220] is IOService::enableInterrupt(0)
 *     (the kernel stub at c017a95a: lookupInterrupt via vt+0x294, then the
 *     controller's vt+0x358). [this+0x70] = the i2s0 nub, whose single
 *     interrupt source carries vector 0x2c -- GPIO group 1, bit 12. vt+0x224
 *     is the matching disableInterrupt.
 *   - [this+0x5c] is an IOCommandGate (commandSleep/commandWakeup at vt+0x90 /
 *     vt+0x94) and [this+0x84] an IOTimerEventSource.
 *   - The wait ALWAYS ended 10.00 s later -- exactly [req+0x68] -- in the timer
 *     handler c0565cc0, which CAS's the state to 4. commandSleep returned 0,
 *     not THREAD_TIMED_OUT, so r6 kept the 0xE00002D8 preloaded at c0565974.
 *
 * So kIOReturnNotReady WAS the timeout, and state 4 is the abort state, not a
 * "done but empty" state. Confirmed independently from the other side:
 * IT_GPIO_TRACE shows the guest write INTEN group 1 <- 0x1060 (bit 12 newly
 * set) immediately after the last I2S setup write, i.e. it really does arm the
 * source it then sleeps on. We never asserted it.
 *
 * THE ONE SUBTLETY, and a single shot does not work without it: the handler at
 * c0565c2c reads the wait state FIRST, and if it is 1 -- which is what
 * c0565968 sets just before enabling the source -- it records 2 and returns
 * without waking anyone. The wait loop at c05659c8 only leaves on 3 or 4, so
 * that interrupt is swallowed. Measured: with a one-shot the trace reads
 * COMPLETION(state=1) then ABORT(state=2) 10 s later; with a repeating
 * interrupt it reads COMPLETION(state=1), COMPLETION(state=2), then POSTWAIT
 * with state=3 and RETURN with r6=0.
 *
 * WHAT IS STILL MISSING: the PCM is all zeroes. 36864 samples, peak 0, not one
 * non-zero. So the transport is fixed and the PRODUCER is not: something
 * upstream never writes the buffer at 0x08c99000 that the DMA faithfully
 * copies here. IT_AMC_STATE=1 changes nothing about that (same 73728 bytes,
 * still all zero), so it is not the AMC command/status handshake. That is the
 * next piece of work, and it is a different question from the one this comment
 * used to pose -- "why is no channel ever programmed" is answered.
 *
 * ---------------------------------------------------------------------------
 * WHO WAS SUPPOSED TO FILL 0x08c99000, and what is known about why they did
 * not (measured 2026-08-03, all of it on this tree, one variable at a time)
 *
 * 1. NOTHING EVER WRITES PCM THERE. IT_RAM_WATCH (hw/arm/ipod_touch_2g.c)
 *    shadows the 64 KB at 0x08c99000 and logs every write with the guest PC.
 *    Over a whole run: 28672 writes, every one of them ZERO, from four PCs
 *    that are two unrolled bzero loops -- 0ff1b3bc/0ff1b3c4 (iBoot, at boot;
 *    IBOOT_MEM_BASE is 0x0ff00000) and c006a498/c006a4a0 (the kernel, 24 KB of
 *    it at sound time). After the I2S bring-up there is not one non-bzero
 *    write. The ring is allocated, zeroed, DMA'd and abandoned. Cross-checked
 *    with QMP pmemsave: all-zero before the sound and for 9 s after it.
 *
 * 2. THE AUDIO ITSELF IS IN RAM AT THE TIME. photoShutter.caf is lpcm 44100/
 *    mono/16-bit; searching all of DRAM for 64-byte runs from its middle finds
 *    them during the sound at 0x0a92d000 and 0x0ab11000 (page cache) and at
 *    0x0dc45044/0x0dce1044/0x0ddff044 in a process's pages -- the +0x044 is the
 *    CAF data chunk's payload offset, so those are file pages 8/4/2. The `caff`
 *    header is at 0x0a95d000. Positive control in the same scan: the strings
 *    "photoShutter" and "UISounds" are found. So the file is read and mapped
 *    and only the copy INTO the ring never happens.
 *
 * 3. IT IS NOT THE AMC. The same scan finds none of that PCM anywhere in the
 *    AMC's 192 KB aperture, with the same positive control. Nothing on this
 *    path touches the AMC at all: IT_AMC_TRACE emits zero lines for a sound.
 *
 * 4. THE DMA'S COMPLETION INTERRUPT IS NEVER DELIVERED, and that is ours.
 *    pl080_update now logs every transition of the combined line under
 *    IT_DMAC_TRACE. DMAC0 raises it once during boot and it is NEVER
 *    acknowledged -- across an entire run the kernel makes ZERO IntStatus
 *    reads and ZERO IntTCClear writes to DMAC0 -- while DMAC1, on the same VIC
 *    number, is serviced cleanly every time (6 reads, 6 acks). Both devices get
 *    the SAME qemu_irq from s5l8900_get_irq(), so DMAC1's deassertions drop
 *    DMAC0's pending interrupt. Proved from the other side with IT_DMAC0_IRQ:
 *    move DMAC0 to 0x10, the number its own header defines, and the kernel
 *    services it 1838 times in one boot. The audio channel is channel 5 on
 *    DMAC0, so every per-period terminal count it raises goes nowhere, and the
 *    audio engine is never told a period finished.
 *
 *    Not yet fixable as it stands: on 0x10 the boot stops at the logo (lit
 *    0.0214), and so does driving both lines through a split-irq
 *    (IT_DMAC0_SPLIT=1) -- with 2196 DMAC0 acks and DMAC1 never reached at all.
 *    Something earlier in the boot depends on DMAC0's interrupt NOT arriving,
 *    which is the same shape as the reverted TYPE_OR_IRQ experiment and is now
 *    at least explained by it: DMAC0's channel-3 terminal count from boot is
 *    latched and never cleared, so a correctly-shared line is held high
 *    forever.
 *
 * 5. The guest kernel log says nothing at all during a sound -- no AMC
 *    assertion, no "could not start DMA". Every layer reports success.
 *
 * Everything below was probed, not inferred, and is kept because it is what
 * ruled out the layers that are NOT at fault.
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
 *    (b) THIS ONE. CONFIRMED, and fixed by the ready interrupt -- see the top
 *        of this comment. c0565974 in the I2S kext, which preloads r6 =
 *        0xE00002D8 as the
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

/*
 * The controller-ready interrupt (GPIO group 1, bit 12 -- see the header).
 *
 * AppleS5L8720X will not program a PL080 channel for this controller until it
 * has seen this interrupt: its gated channel-start routine calls
 * IOService::enableInterrupt(0) on the i2s0 nub, arms a 10 s IOTimerEventSource
 * and sleeps. Before this existed the timer always won and the start returned
 * kIOReturnNotReady, which is exactly what "AppleARMIISAudioDevice: could not
 * start DMA: device is not ready" was reporting.
 *
 * It is raised a short while AFTER the setup writes rather than during them,
 * because the driver enables the interrupt source only once it has finished
 * writing registers -- the enable happens further down the same call chain that
 * did the writes. A pending latch set before the source is enabled is not
 * guaranteed to be redelivered by the GPIO controller model, so the timer
 * removes the ordering question entirely.
 *
 * IT IS PERIODIC, AND THAT IS NOT A DETAIL. A single shot is not enough, which
 * is measurable rather than arguable: the handler at c0565c2c reads the wait
 * state first, and if it is 1 -- which is exactly what the start routine sets
 * just before it enables the source -- it records 2 and returns WITHOUT waking
 * the sleeper. The wait loop only leaves on 3 or 4, so one interrupt that lands
 * in that window is swallowed and the 10 s timer still wins. Only the next
 * interrupt, arriving with the state at 2, takes the branch that sets 3,
 * disables the source and wakes the sleeper. A real I2S service interrupt
 * free-runs while the block is clocking, so repeating is also the physically
 * honest model -- and it self-limits, because we only raise while the guest
 * still has the source unmasked in the GPIO controller.
 *
 * IT_I2S_IRQ=0 disables it, for bisecting against the old silent behaviour.
 */
#define IT_I2S_READY_DELAY_NS  (2 * 1000 * 1000)
#define IT_I2S_READY_PERIOD_NS (2 * 1000 * 1000)
/* 64 x 2 ms = 128 ms of interrupts per start: two orders of magnitude more than
 * the handshake needs, three orders inside the driver's 10 s deadline, and it
 * leaves nothing ticking once a sound is over. */
#define IT_I2S_READY_TICKS     64

static bool it_i2s_irq_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("IT_I2S_IRQ");
        cached = (v == NULL) ? 1 : (atoi(v) != 0);
    }
    return cached != 0;
}

static void it_i2s_ready_cb(void *opaque)
{
    IPodTouchI2SState *s = (IPodTouchI2SState *)opaque;
    uint32_t bit = 1u << IT_I2S_GPIO_INT_BIT;

    if (!s->sysic || !s->enable || s->ready_ticks == 0) {
        return;         /* block powered down, or the burst is spent */
    }
    s->ready_ticks--;
    /*
     * Only while the guest actually wants it. The driver masks the source in
     * the GPIO controller the moment its handshake completes, so we never
     * assert a line it has told us it does not want. The tick budget rather
     * than the mask is what ends the burst, because the mask is set a few
     * instructions AFTER the register writes that arm us -- stopping on "masked
     * right now" would race that window and fire nothing at all.
     */
    if (s->sysic->gpio_int_enabled[IT_I2S_GPIO_INT_GROUP] & bit) {
        s->sysic->gpio_int_status[IT_I2S_GPIO_INT_GROUP] |= bit;
        qemu_irq_raise(s->sysic->gpio_irqs[IT_I2S_GPIO_INT_GROUP]);
        s->ready_irqs++;
        if (s->ready_irqs <= 8) {
            IT_I2S_DPRINTF("ready irq #%" PRIu64 ": gpio group %d bit %d\n",
                           s->ready_irqs, IT_I2S_GPIO_INT_GROUP,
                           IT_I2S_GPIO_INT_BIT);
        }
    }
    timer_mod(s->ready_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + IT_I2S_READY_PERIOD_NS);
}

static void it_i2s_arm_ready(IPodTouchI2SState *s)
{
    if (!it_i2s_irq_enabled() || !s->ready_timer || !s->sysic) {
        return;
    }
    s->ready_ticks = IT_I2S_READY_TICKS;
    timer_mod(s->ready_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + IT_I2S_READY_DELAY_NS);
}

/*
 * TX FIFO pacing.
 *
 * THIS IS THE ANSWER TO "WHY IS THE PCM ALL ZEROES", and it is not an AMC or a
 * codec or a routing problem -- it is a clock problem, in this emulator.
 *
 * The guest's audio stack programs an LLI-chained PL080 descriptor over an
 * 18-period, 72 KB ring and starts the channel. It has written nothing into
 * that ring yet: an IOAudio2 output engine fills the ring AHEAD of the play
 * position, in real time, driven by the per-period terminal-count interrupts
 * the DMA raises as it goes. Our PL080 ignored the peripheral request line, so
 * the channel start copied all 72 KB into this FIFO inside a single MMIO write
 * -- 0.84 s of audio in zero guest time -- and then raised every period
 * interrupt at once. Everything we delivered was the silence the ring had been
 * allocated with, and the engine never got a chance to write a sample. Measured
 * both ways: the modelled FIFO saw 73728 bytes with peak 0, and pmemsave of the
 * ring's physical pages (0x08c99000, 64 KB) read all-zero before the sound and
 * for nine seconds after it.
 *
 * So we give the transfer a clock. The block drains its TX FIFO at the sample
 * rate and asserts its DMA request line whenever the FIFO has room; the PL080
 * re-reads that line between elements, so it refills and stops on its own. The
 * gating only applies to peripherals that opt in (see
 * pl080_attach_paced_peripheral), so nothing else on either DMAC changes.
 *
 * The drain is computed from the virtual clock rather than counted in ticks, so
 * a late or coalesced timer callback cannot inflate the rate.
 *
 * IT_I2S_PACE=0 restores the old free-running behaviour for A/B.
 */
static bool it_i2s_pace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("IT_I2S_PACE");
        cached = (v == NULL) ? 1 : (atoi(v) != 0);
    }
    return cached != 0;
}

static void it_i2s_update_dma_req(IPodTouchI2SState *s)
{
    bool want;

    if (!s->dmac || !it_i2s_pace_enabled()) {
        return;
    }
    want = s->enable && s->fifo_bytes < s->fifo_depth;
    if (want == s->dma_req) {
        return;
    }
    s->dma_req = want;
    /* Raising re-enters the DMA controller, which will write PCM back into this
     * device; lowering never does. it_i2s_push() only ever raises fifo_bytes,
     * so it can only lower the line, and the recursion terminates. */
    pl080_set_dma_request(s->dmac, s->dma_req_id, want);
}

/*
 * Advance the modelled FIFO by however much real time has passed.
 *
 * TIME MUST NEVER BE DISCARDED HERE, and it used to be: draining clamped at an
 * empty FIFO and forgot the excess, so every host stall longer than the 11.6 ms
 * the FIFO covers permanently slipped the stream. Measured on nand-grow7g, the
 * main loop stalls 20-50 ms roughly every 300 ms, and the guest's 16-page ring
 * laps took 390-411 ms against the ideal 371.5 -- an effective sample clock
 * 5-10% slow and irregular. The guest's engine paces its producer against its
 * own timers, notices the DAC position falling behind, and periodically resyncs
 * its mix cursor forward -- which lands the next content 3-4 pages ahead and
 * leaves the pre-zeroed pages in between to play as inserted silence. That, not
 * the host sink, was the residual audible garble: 7 of 7 shutter captures
 * carried a 3-7 page hole of pure zeros at a page boundary mid-clip.
 *
 * So the shortfall is carried as a debt instead. While the debt is nonzero the
 * FIFO reads empty, the request line stays up, and it_i2s_push() repays the
 * debt byte-for-byte as the PL080 refills -- a catch-up burst that ends exactly
 * when the head is back on real time. The producer is paced by real time and is
 * therefore ahead of a stalled consumer, so the burst only ever reads pages the
 * engine has already written.
 *
 * The debt is bounded (a stall longer than half a ring lap is not worth
 * chasing; the engine will resync regardless) and cleared at every stream
 * boundary, so an idle gap between sounds can never fast-forward the next
 * sound's attack.
 */
#define IT_I2S_PACE_DEBT_MAX 32768u     /* bytes; ~186 ms, half a ring lap */

/* IT_I2S_DEBT=0 restores the old discard-on-stall behaviour, for bisecting. */
static bool it_i2s_debt_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("IT_I2S_DEBT");
        cached = (v == NULL) ? 1 : (atoi(v) != 0);
    }
    return cached != 0;
}

static void it_i2s_pace_drain(IPodTouchI2SState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed;
    uint64_t bytes;
    unsigned frame = 2 * s->as.nchannels;   /* 16-bit samples */

    if (s->pace_last_ns == 0) {
        s->pace_last_ns = now;
        return;
    }
    elapsed = now - s->pace_last_ns;
    if (elapsed <= 0) {
        return;
    }
    s->pace_last_ns = now;

    /*
     * Debt must only chase HOST stalls, never the guest. If the timer is
     * ticking on cadence, the request line is up, and the PL080 still gave us
     * nothing since the last tick, then the guest's descriptor chain is the
     * limiter -- the engine appends work in real time and the DMA has caught
     * its append edge. Carrying debt there glues the read head to the append
     * edge with zero margin, and the next slightly-late append plays pages the
     * mixer has not written yet (measured: delivery degenerated into 3 ms
     * stop-go bursts and the skips returned). Forgive the debt instead: the
     * head falls back to the guest's own pace, which is the honest rate.
     * A genuine host stall is distinguishable because the tick itself is late.
     */
    if (it_i2s_debt_enabled() &&
        elapsed < 3 * IT_I2S_PACE_PERIOD_NS &&
        s->fifo_bytes == 0 && s->dma_req && !s->pushed_since_tick) {
        s->pace_debt = 0;
        return;
    }
    s->pushed_since_tick = false;

    bytes = ((uint64_t)s->as.freq * frame * (uint64_t)elapsed) / 1000000000ULL;
    if (it_i2s_debt_enabled()) {
        /*
         * Repay debt GRADUALLY: at most 25% over real rate per tick. An
         * instant catch-up burst was tried first and it re-created the very
         * skip it was meant to fix -- after a whole-process stall the guest's
         * mixer is exactly as far behind as the DMA, and a zero-time burst
         * reads the pages it has not yet rewritten. At 1.25x the head recovers
         * a 40 ms stall over ~160 ms while the resumed mixer (which refills 8
         * pages in ~7 ms) stays comfortably ahead of it.
         */
        uint64_t due = bytes + s->pace_debt;
        uint64_t budget = bytes + bytes / 4;
        uint64_t apply = MIN(due, budget);

        s->pace_debt = MIN(due - apply, (uint64_t)IT_I2S_PACE_DEBT_MAX);
        bytes = apply;
    }
    if (bytes >= s->fifo_bytes) {
        /* Whatever the FIFO could not supply is still owed, not forgotten. */
        if (it_i2s_debt_enabled()) {
            s->pace_debt = MIN(s->pace_debt + (bytes - s->fifo_bytes),
                               (uint64_t)IT_I2S_PACE_DEBT_MAX);
        }
        s->fifo_bytes = 0;
    } else {
        s->fifo_bytes -= bytes;
    }
}

/*
 * IT_I2S_STALLDBG=1 -- when the stream is up and nothing has reached the FIFO
 * for >10 ms, print the whole gating state once per episode: the FIFO model,
 * the request line, and the PL080 channel-5 registers. This exists to NAME the
 * limiter during the recurring mid-stream delivery pauses rather than infer it.
 */
static void it_i2s_stall_debug(IPodTouchI2SState *s)
{
    static int on = -1;
    static int64_t episode_reported;
    int64_t now;

    if (on < 0) {
        on = getenv("IT_I2S_STALLDBG") != NULL;
    }
    if (!on || !s->running || !s->dmac || s->last_push_ns == 0) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now - s->last_push_ns < 10 * 1000 * 1000) {
        episode_reported = 0;
        return;
    }
    if (episode_reported) {
        return;
    }
    episode_reported = 1;
    fprintf(stderr, "[i2s-stall] t=%" PRId64 " no push for %.1f ms: "
            "fifo=%u/%u debt=%u req=%d req_single=%08x running=%d | "
            "ch5 src=%08x dest=%08x lli=%08x ctrl=%08x conf=%08x\n",
            now, (now - s->last_push_ns) / 1e6,
            s->fifo_bytes, s->fifo_depth, s->pace_debt, s->dma_req,
            s->dmac->req_single, s->dmac->running,
            s->dmac->chan[5].src, s->dmac->chan[5].dest,
            s->dmac->chan[5].lli, s->dmac->chan[5].ctrl,
            s->dmac->chan[5].conf);
}

static void it_i2s_pace_cb(void *opaque)
{
    IPodTouchI2SState *s = (IPodTouchI2SState *)opaque;

    if (!s->enable) {
        s->dma_req = false;
        if (s->dmac && it_i2s_pace_enabled()) {
            pl080_set_dma_request(s->dmac, s->dma_req_id, false);
        }
        return;         /* block powered down: stop the timer */
    }
    it_i2s_pace_drain(s);
    it_i2s_update_dma_req(s);
    it_i2s_stall_debug(s);
    timer_mod(s->pace_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + IT_I2S_PACE_PERIOD_NS);
}

static void it_i2s_pace_start(IPodTouchI2SState *s)
{
    if (!s->pace_timer || !it_i2s_pace_enabled()) {
        return;
    }
    s->pace_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /* A new stream starts on real time, owing nothing: debt accrued across an
     * idle gap must never fast-forward the next sound's attack. */
    s->pace_debt = 0;
    it_i2s_update_dma_req(s);
    timer_mod(s->pace_timer, s->pace_last_ns + IT_I2S_PACE_PERIOD_NS);
}

/*
 * IT_I2S_VOICELOG=<path> -- every SWVoiceOut state change and every backend
 * callback, timestamped. The one thing a wav capture cannot show is WHEN the
 * bytes were handed over relative to real time, and whether the voice was even
 * running when they were.
 */
static void it_i2s_vlog(IPodTouchI2SState *s, const char *what,
                        int a, int b)
{
    static FILE *f;
    static int state;
    static int64_t t0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (state == 0) {
        const char *p = getenv("IT_I2S_VOICELOG");
        state = -1;
        if (p) {
            f = fopen(p, "w");
            state = f ? 1 : -1;
        }
    }
    if (state < 0) {
        return;
    }
    if (t0 == 0) {
        t0 = now;
    }
    fprintf(f, "%10.6f %-10s %8d %8d  ring=%u fifo=%u run=%d act=%d\n",
            (now - t0) / 1e9, what, a, b, s->ring_level, s->fifo_bytes,
            s->running, s->active);
    fflush(f);
}

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
    it_i2s_vlog(s, "out_cb", free_bytes, s->ring_level);
    it_i2s_drain(s, free_bytes);

    /* When the guest has halted TX and we've flushed everything, park the
     * voice so the backend stops pulling silence. */
    if (!s->running && s->ring_level == 0 && s->active) {
        it_i2s_vlog(s, "DEACT", 0, 0);
        AUD_set_active_out(s->voice, 0);
        s->active = 0;
        /* Next sound prebuffers again from scratch. */
        s->prefilled = false;
        s->prefill_start_ns = 0;
    }
}

/*
 * HOST-SIDE PREBUFFER, and why the sound was still garbled after it had been
 * proved sample-exact.
 *
 * Everything upstream of here delivers correctly: the clip arrives at the TX
 * FIFO bit-identical to photoShutter.caf, and it arrives paced at 44100 Hz.
 * That was verified with `-audio driver=wav`, and it is true. But a wav backend
 * has no clock -- it concatenates whatever it is given, whenever it is given it
 * -- so it certifies the CONTENT and says nothing about the SCHEDULE, and on a
 * real-time sink the schedule is the whole problem.
 *
 * The sink is CoreAudio, and its IOProc is all-or-nothing: if fewer than one
 * full device buffer (512 frames, 11.6 ms by default) is queued when the
 * hardware calls, audio/coreaudio.m plays NOTHING that period. Our data path
 * had no lead to give it. The modelled TX FIFO drains at exactly the sample
 * rate and is only 2048 bytes deep -- deliberately, because that depth is how
 * far the DMA read head runs ahead of the guest's own producer and raising it
 * breaks fidelity (see the header) -- so PCM reached the backend just in time
 * and never ahead of time. Measured on nand-grow7g: a mean of 0.5 device
 * buffers queued at each callback, and one starved callback landing inside the
 * clip, which puts an 11.6 ms hole in the middle of a 500 ms sound and shifts
 * everything after it. Upstream's starvation path does not even clear the
 * hardware buffer, so what is actually heard is the previous 11.6 ms replayed.
 * That is the garbling: recognisable, because every sample is present and in
 * order, and wrong, because a fragment is repeated in the middle of it.
 *
 * The fix cannot be a deeper FIFO -- that is the guest's clock and it is
 * correct. It is a prebuffer on the far side: hold the first
 * IT_I2S_PREBUFFER_BYTES back, then start the voice, which hands the whole lead
 * to the mixer in one go. From then on production and consumption are both real
 * time, so the lead persists and the host pipeline stays full. The cost is that
 * much output latency, once per sound.
 *
 * The deadline exists so this can never be the reason a sound does not play: a
 * stream shorter than the prebuffer, or one whose delivery stalls, starts
 * anyway. Nothing in this tree may have a wait with no way out.
 */
static bool it_i2s_prefill_ready(IPodTouchI2SState *s)
{
    int64_t now;

    if (s->prefilled || s->ring_level >= s->prebuffer) {
        return true;
    }
    /*
     * There is deliberately no "TX is not running, so start anyway" escape here.
     * That was the first version and it never let the prebuffer run once:
     * it_i2s_align_frame() pushes up to two pad bytes on the TXFCTL write, which
     * is part of SETUP and happens with running still false, so the voice came
     * up on those two bytes and the stream that followed found it already
     * active. Measured -- every ACT in a run read `ring=2 fifo=2 run=0`. The
     * short-clip case is handled where it belongs, on the TXCOM halt.
     */
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->prefill_start_ns == 0) {
        s->prefill_start_ns = now;
        return false;
    }
    return now - s->prefill_start_ns >= IT_I2S_PREFILL_DEADLINE_NS;
}

static void it_i2s_activate(IPodTouchI2SState *s)
{
    if (!s->card_ok || !s->voice || s->active) {
        return;
    }
    if (!it_i2s_prefill_ready(s)) {
        return;
    }
    s->prefilled = true;
    it_i2s_vlog(s, "ACT", s->ring_level, 0);
    AUD_set_active_out(s->voice, 1);
    s->active = 1;
}

/*
 * IT_I2S_TRACE=<path> -- a 16-byte binary record per FIFO element:
 *
 *     int64 virtual_ns, int64 host_ns (low 32) | dma src (high 32)... see below
 *
 * Concretely: {int64 virt_ns, uint32 host_ms, uint32 dma_src}. Three questions
 * need answering together and none of them can be answered from the PCM alone:
 * when did a sample leave (virtual time), when did it leave in the world the
 * host sound card lives in (host time), and WHICH ring address did it come
 * from. The last is what tells a circular buffer that has been read once from
 * one that has been lapped, and the first two are what tell a rate error from
 * an emulator that simply cannot keep up.
 */
static void it_i2s_trace(IPodTouchI2SState *s)
{
    static FILE *f;
    static int state;           /* 0 unknown, 1 open, -1 off */
    struct { int64_t virt_ns; uint32_t host_ms; uint32_t src; } rec;

    if (state == 0) {
        const char *p = getenv("IT_I2S_TRACE");
        state = -1;
        if (p) {
            f = fopen(p, "wb");
            state = f ? 1 : -1;
        }
    }
    if (state < 0) {
        return;
    }
    rec.virt_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    rec.host_ms = (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_HOST) / 1000000);
    rec.src = s->dmac ? s->dmac->paced_src : 0;
    fwrite(&rec, sizeof(rec), 1, f);
}

static void it_i2s_push(IPodTouchI2SState *s, const uint8_t *buf, unsigned len)
{
    unsigned i;

    it_i2s_trace(s);
    s->total_bytes += len;
    /* The bytes just clocked into the TX FIFO. Charge them before anything
     * else: this runs inside the DMA controller's own transfer loop, and the
     * request line we lower here is what ends the burst. */
    s->fifo_bytes += len;
    s->pushed_since_tick = true;
    s->last_push_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /* Stall debt is repaid only by it_i2s_pace_drain(), rate-limited. Repaying
     * it here, instantly, turned a recovered stall into a zero-time burst that
     * outran the guest's own recovery -- see the note in the drain. */

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
    it_i2s_update_dma_req(s);
}

/*
 * Put the stream back on a left-channel boundary.
 *
 * A DMA channel is stopped wherever the guest's Config = 0 lands, which is an
 * arbitrary element -- so a stream can end having delivered an ODD number of
 * 16-bit samples. On the real block that does not matter: the serialiser takes
 * its channel from LRCLK and the driver resets the TX FIFO (this register)
 * before every stream, so the next stream starts on the left channel whatever
 * the last one left behind. Our FIFO is a byte stream with no LRCLK, so a
 * stray half-frame permanently swaps left and right for every stream that
 * follows it -- measured: with the shutter's mono-duplicated PCM, whole sounds
 * came out with L != R for every frame, the whole stream skewed by one sample.
 *
 * Completing the frame with silence rather than discarding the odd sample keeps
 * the sample count and costs at most one 22 us sample of one channel.
 */
static void it_i2s_align_frame(IPodTouchI2SState *s)
{
    unsigned frame = 2 * s->as.nchannels;
    unsigned rem = (unsigned)(s->total_bytes % frame);
    uint8_t pad[8] = { 0 };

    if (rem) {
        it_i2s_push(s, pad, frame - rem);
    }
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
        /* The block's clock. While it is running the TX FIFO empties at the
         * sample rate, which is the only thing that paces the DMA. */
        if (s->enable) {
            s->fifo_bytes = 0;
            it_i2s_pace_start(s);
        } else {
            it_i2s_update_dma_req(s);
        }
        break;
    case IT_I2S_TXCON:
        s->txcon = value;
        break;
    case IT_I2S_TXCOM:
        s->txcom = value;
        if ((value & 0x7) == IT_I2S_CMD_RUN) {
            s->running = true;
            s->pace_debt = 0;   /* stream boundary: start on real time */
            /* Restart the prebuffer deadline with the stream it belongs to;
             * otherwise it runs from whatever setup byte happened to land in the
             * ring, and can expire before there is anything to play. */
            if (!s->prefilled) {
                s->prefill_start_ns = 0;
            }
            it_i2s_activate(s);
            IT_I2S_DPRINTF("TX run (total=%" PRIu64 " dropped=%" PRIu64 ")\n",
                           s->total_bytes, s->dropped);
        } else if (value == IT_I2S_CMD_HALT) {
            s->running = false;
            s->pace_debt = 0;   /* stream boundary: nothing owed across a halt */
            /* A clip shorter than the prebuffer has nothing left to push, so
             * this is its only chance to start the voice -- but ONLY if there is
             * something to play. The driver writes TXCOM = 0 during SETUP as
             * well (it lands between txcon and txfctl, before TX has ever run),
             * and activating there brought the voice up with an empty ring, so
             * the stream that followed started already active and got no
             * prebuffer at all. Measured: mean queue 0.5 device buffers with
             * that call unguarded against 1.5 without it. */
            if (s->ring_level) {
                it_i2s_activate(s);
            }
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
        it_i2s_align_frame(s);
        /* Last register of the driver's setup sequence (enable, clkdiv, txcon,
         * rxcon, txcom, rxcom, txfctl), so the block is fully configured here.
         * Tell the driver the controller is ready. */
        it_i2s_arm_ready(s);
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
    s->ready_ticks = 0;
    s->ready_irqs = 0;
    if (s->ready_timer) {
        timer_del(s->ready_timer);
    }
    s->fifo_bytes = 0;
    s->prefilled = false;
    s->prefill_start_ns = 0;
    s->pace_last_ns = 0;
    if (s->pace_timer) {
        timer_del(s->pace_timer);
    }
    if (s->dma_req && s->dmac) {
        pl080_set_dma_request(s->dmac, s->dma_req_id, false);
    }
    s->dma_req = false;

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

    s->prebuffer = IT_I2S_PREBUFFER_BYTES_DEFAULT;
    const char *pre = getenv("IT_I2S_PREBUFFER_BYTES");
    if (pre) {
        int b = atoi(pre);
        if (b >= 0) {
            s->prebuffer = b;
        }
    }

    s->fifo_depth = IT_I2S_FIFO_BYTES_DEFAULT;
    const char *depth = getenv("IT_I2S_FIFO_BYTES");
    if (depth) {
        int d = atoi(depth);
        if (d > 0) {
            s->fifo_depth = d;
        }
    }

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
    s->ready_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, it_i2s_ready_cb, s);
    s->pace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, it_i2s_pace_cb, s);
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
