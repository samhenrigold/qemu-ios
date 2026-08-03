/*
 * QEMU Timer based audio emulation
 *
 * Copyright (c) 2004-2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "audio.h"
#include "qemu/timer.h"

#define AUDIO_CAP "noaudio"
#include "audio_int.h"

typedef struct NoVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
} NoVoiceOut;

typedef struct NoVoiceIn {
    HWVoiceIn hw;
    RateCtl rate;
} NoVoiceIn;

/*
 * IT_AUD_RT=<path> -- reconstruct what a REAL-TIME sink would actually have
 * played, as raw PCM in the voice's own format.
 *
 * The `wav` backend has no clock: it concatenates whatever arrives, whenever it
 * arrives, so an underrun, a burst delivery or a stopped-and-restarted voice all
 * produce a byte-perfect file. Every one of those is audible on a real card.
 * This backend DOES have a clock -- audio_rate_* meters it against the virtual
 * clock, which tracks host wall time -- so the gap between "where the sink's
 * playback cursor is" and "how much we have actually been handed" is exactly the
 * silence a real card would have emitted. Write that silence into the stream and
 * the resulting file can be compared against the source PCM, or listened to.
 *
 * Also logs one line per gap to IT_AUD_RT.log, so the defect is countable and
 * not just audible.
 */
static void no_rt_tap(HWVoiceOut *hw, const void *buf, size_t len, size_t due)
{
    static FILE *pcm, *log;
    static int state;                   /* 0 unknown, 1 open, -1 off */
    static uint64_t written, silence;
    static unsigned gaps;
    static int64_t t0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ideal;

    if (state == 0) {
        const char *p = getenv("IT_AUD_RT");
        state = -1;
        if (p) {
            char *lp = g_strdup_printf("%s.log", p);
            pcm = fopen(p, "wb");
            log = fopen(lp, "w");
            g_free(lp);
            state = (pcm && log) ? 1 : -1;
        }
    }
    if (state < 0) {
        return;
    }
    /*
     * The sink's playback cursor derived from the virtual clock, NOT from the
     * backend's RateCtl: audio_rate_start() is called every time the voice is
     * enabled, so a voice that is stopped and restarted mid-stream rewinds that
     * counter and the dropout it caused would vanish from the measurement. The
     * cursor cannot rewind. Whatever we have not been handed by the time the
     * cursor passes it was played as silence.
     */
    if (t0 == 0) {
        t0 = now;
    }
    ideal = muldiv64(now - t0, hw->info.bytes_per_second, NANOSECONDS_PER_SECOND);
    ideal -= ideal % hw->info.bytes_per_frame;
    (void)due;
    if (ideal > written + silence) {
        size_t gap = ideal - (written + silence);
        static const uint8_t zero[4096];
        for (size_t n = gap; n; ) {
            size_t c = MIN(n, sizeof(zero));
            fwrite(zero, 1, c, pcm);
            n -= c;
        }
        silence += gap;
        gaps++;
        fprintf(log, "gap %6u  %8.3f ms  at %8.3f s of output\n", gaps,
                gap * 1000.0 / hw->info.bytes_per_second,
                written * 1.0 / hw->info.bytes_per_second);
    }
    if (len) {
        fwrite(buf, 1, len, pcm);
        written += len;
    }
    fflush(pcm);
    fprintf(log, "#total played %.3f s, %u gaps, %.3f s silent (%.2f%%)\r",
            (written + silence) * 1.0 / hw->info.bytes_per_second, gaps,
            silence * 1.0 / hw->info.bytes_per_second,
            (written + silence) ? silence * 100.0 / (written + silence) : 0.0);
    fflush(log);
}

static size_t no_write(HWVoiceOut *hw, void *buf, size_t len)
{
    NoVoiceOut *no = (NoVoiceOut *) hw;
    size_t due = audio_rate_peek_bytes(&no->rate, &hw->info);
    size_t take = MIN(due, len);

    audio_rate_add_bytes(&no->rate, take);
    no_rt_tap(hw, buf, take, due);
    return take;
}

static int no_init_out(HWVoiceOut *hw, struct audsettings *as, void *drv_opaque)
{
    NoVoiceOut *no = (NoVoiceOut *) hw;

    audio_pcm_init_info (&hw->info, as);
    hw->samples = 1024;
    audio_rate_start(&no->rate);
    return 0;
}

static void no_fini_out (HWVoiceOut *hw)
{
    (void) hw;
}

static void no_enable_out(HWVoiceOut *hw, bool enable)
{
    NoVoiceOut *no = (NoVoiceOut *) hw;

    if (enable) {
        audio_rate_start(&no->rate);
    }
}

static int no_init_in(HWVoiceIn *hw, struct audsettings *as, void *drv_opaque)
{
    NoVoiceIn *no = (NoVoiceIn *) hw;

    audio_pcm_init_info (&hw->info, as);
    hw->samples = 1024;
    audio_rate_start(&no->rate);
    return 0;
}

static void no_fini_in (HWVoiceIn *hw)
{
    (void) hw;
}

static size_t no_read(HWVoiceIn *hw, void *buf, size_t size)
{
    NoVoiceIn *no = (NoVoiceIn *) hw;
    int64_t bytes = audio_rate_get_bytes(&no->rate, &hw->info, size);

    audio_pcm_info_clear_buf(&hw->info, buf, bytes / hw->info.bytes_per_frame);
    return bytes;
}

static void no_enable_in(HWVoiceIn *hw, bool enable)
{
    NoVoiceIn *no = (NoVoiceIn *) hw;

    if (enable) {
        audio_rate_start(&no->rate);
    }
}

static void *no_audio_init(Audiodev *dev, Error **errp)
{
    return &no_audio_init;
}

static void no_audio_fini (void *opaque)
{
    (void) opaque;
}

static struct audio_pcm_ops no_pcm_ops = {
    .init_out = no_init_out,
    .fini_out = no_fini_out,
    .write    = no_write,
    .buffer_get_free = audio_generic_buffer_get_free,
    .run_buffer_out = audio_generic_run_buffer_out,
    .enable_out = no_enable_out,

    .init_in  = no_init_in,
    .fini_in  = no_fini_in,
    .read     = no_read,
    .run_buffer_in = audio_generic_run_buffer_in,
    .enable_in = no_enable_in
};

static struct audio_driver no_audio_driver = {
    .name           = "none",
    .descr          = "Timer based audio emulation",
    .init           = no_audio_init,
    .fini           = no_audio_fini,
    .pcm_ops        = &no_pcm_ops,
    .max_voices_out = INT_MAX,
    .max_voices_in  = INT_MAX,
    .voice_size_out = sizeof (NoVoiceOut),
    .voice_size_in  = sizeof (NoVoiceIn)
};

static void register_audio_none(void)
{
    audio_driver_register(&no_audio_driver);
}
type_init(register_audio_none);
