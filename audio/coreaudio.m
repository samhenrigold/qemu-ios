/*
 * QEMU OS X CoreAudio audio driver
 *
 * Copyright (c) 2005 Mike Kronenberg
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
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>            /* pthread_X */

#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/audio.h"
#include "qom/object.h"
#include "audio_int.h"

#define TYPE_AUDIO_COREAUDIO "audio-coreaudio"
OBJECT_DECLARE_SIMPLE_TYPE(AudioCoreaudio, AUDIO_COREAUDIO)

struct AudioCoreaudio {
    AudioMixengBackend parent_obj;
};

typedef struct coreaudioVoiceOut {
    HWVoiceOut hw;
    pthread_mutex_t buf_mutex;
    AudioDeviceID device_id;
    int frame_size_setting;
    uint32_t buffer_count;
    UInt32 device_frame_size;
    AudioDeviceIOProcID ioprocid;
    bool enabled;
    /*
     * Channels the DEVICE actually runs, which is not always the number we
     * asked it for. We request stereo, but a device is free to refuse: the
     * Studio Display's speakers run 8 channels and keep running 8 after our
     * set-format call. The IOProc buffer is then frameCount * dev_nchannels
     * frames wide while our samples are 2 wide, so writing them contiguously
     * lays every frame at the wrong stride and plays as garbage. Recorded here
     * so the IOProc can scatter into the device's layout instead.
     */
    UInt32 dev_nchannels;
} CoreaudioVoiceOut;

static const AudioObjectPropertyAddress voice_out_addr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};

static OSStatus coreaudio_get_voice_out(AudioDeviceID *id)
{
    UInt32 size = sizeof(*id);

    return AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                      &voice_out_addr,
                                      0,
                                      NULL,
                                      &size,
                                      id);
}

static OSStatus coreaudio_get_out_framesizerange(AudioDeviceID id,
                                                 AudioValueRange *framerange)
{
    UInt32 size = sizeof(*framerange);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      framerange);
}

static OSStatus coreaudio_get_out_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      framesize);
}

static OSStatus coreaudio_set_out_framesize(AudioDeviceID id, UInt32 *framesize)
{
    UInt32 size = sizeof(*framesize);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectSetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      size,
                                      framesize);
}

static OSStatus coreaudio_set_out_streamformat(AudioDeviceID id,
                                               AudioStreamBasicDescription *d)
{
    UInt32 size = sizeof(*d);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamFormat,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectSetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      size,
                                      d);
}

static const AudioObjectPropertyAddress rate_addr = {
    kAudioDevicePropertyNominalSampleRate,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};

static OSStatus coreaudio_get_rate(AudioDeviceID id, Float64 *rate)
{
    UInt32 size = sizeof(*rate);

    return AudioObjectGetPropertyData(id, &rate_addr, 0, NULL, &size, rate);
}

static OSStatus coreaudio_get_out_isrunning(AudioDeviceID id, UInt32 *result)
{
    UInt32 size = sizeof(*result);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceIsRunning,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    return AudioObjectGetPropertyData(id,
                                      &addr,
                                      0,
                                      NULL,
                                      &size,
                                      result);
}

static void coreaudio_logstatus(OSStatus status)
{
    const char *str = "BUG";

    switch (status) {
    case kAudioHardwareNoError:
        str = "kAudioHardwareNoError";
        break;

    case kAudioHardwareNotRunningError:
        str = "kAudioHardwareNotRunningError";
        break;

    case kAudioHardwareUnspecifiedError:
        str = "kAudioHardwareUnspecifiedError";
        break;

    case kAudioHardwareUnknownPropertyError:
        str = "kAudioHardwareUnknownPropertyError";
        break;

    case kAudioHardwareBadPropertySizeError:
        str = "kAudioHardwareBadPropertySizeError";
        break;

    case kAudioHardwareIllegalOperationError:
        str = "kAudioHardwareIllegalOperationError";
        break;

    case kAudioHardwareBadDeviceError:
        str = "kAudioHardwareBadDeviceError";
        break;

    case kAudioHardwareBadStreamError:
        str = "kAudioHardwareBadStreamError";
        break;

    case kAudioHardwareUnsupportedOperationError:
        str = "kAudioHardwareUnsupportedOperationError";
        break;

    case kAudioDeviceUnsupportedFormatError:
        str = "kAudioDeviceUnsupportedFormatError";
        break;

    case kAudioDevicePermissionsError:
        str = "kAudioDevicePermissionsError";
        break;

    default:
        error_printf(" Reason: status code %" PRId32, (int32_t)status);
        return;
    }

    error_printf(" Reason: %s", str);
}

static void G_GNUC_PRINTF(2, 3) coreaudio_logerr(OSStatus status,
                                                const char *fmt, ...)
{
    va_list ap;

    error_printf("coreaudio: ");
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
    coreaudio_logstatus(status);
    error_printf("\n");
}

static void G_GNUC_PRINTF(3, 4) coreaudio_logerr2(OSStatus status,
                                                  const char *typ,
                                                  const char *fmt, ...)
{
    va_list ap;

    error_printf("coreaudio: Could not initialize %s: ", typ);
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
    coreaudio_logstatus(status);
    error_printf("\n");
}

#define coreaudio_playback_logerr(status, ...) \
    coreaudio_logerr2(status, "playback", __VA_ARGS__)

static int coreaudio_voice_out_buf_lock(CoreaudioVoiceOut *core,
                                        const char *fn_name)
{
    int err;

    err = pthread_mutex_lock(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not lock voice for %s: %s",
                     fn_name, strerror(err));
        return -1;
    }
    return 0;
}

static int coreaudio_voice_out_buf_unlock(CoreaudioVoiceOut *core,
                                          const char *fn_name)
{
    int err;

    err = pthread_mutex_unlock(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not unlock voice for %s: %s",
                     fn_name, strerror(err));
        return -1;
    }
    return 0;
}

#define COREAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args)       \
    static ret_type glue(coreaudio_, name)args_decl                   \
    {                                                                 \
        CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;            \
        ret_type ret;                                                 \
                                                                      \
        if (coreaudio_voice_out_buf_lock(core, "coreaudio_" #name)) { \
            return 0;                                                 \
        }                                                             \
                                                                      \
        ret = glue(audio_generic_, name)args;                         \
                                                                      \
        coreaudio_voice_out_buf_unlock(core, "coreaudio_" #name);     \
        return ret;                                                   \
    }
COREAUDIO_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut *hw), (hw))
COREAUDIO_WRAPPER_FUNC(get_buffer_out, void *, (HWVoiceOut *hw, size_t *size),
                       (hw, size))
COREAUDIO_WRAPPER_FUNC(put_buffer_out, size_t,
                       (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
COREAUDIO_WRAPPER_FUNC(write, size_t, (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
#undef COREAUDIO_WRAPPER_FUNC

/*
 * callback to feed audiooutput buffer. called without BQL.
 * allowed to lock "buf_mutex", but disallowed to have any other locks.
 */
/*
 * IT_CA_TAP=<path> -- capture what CoreAudio's own IO thread actually plays,
 * and MUTE the device so nothing reaches the speakers.
 *
 * This exists because `-audio driver=wav` cannot see any timing defect: it has
 * no clock, so a starved sink, a burst delivery and a stopped voice all produce
 * a byte-perfect file. CoreAudio is the sink the user actually hears, and its
 * IOProc has a hard, all-or-nothing requirement -- if fewer than one full device
 * buffer of frames is queued it returns having written NOTHING, and the hardware
 * plays whatever was left in that buffer. Counting those is the measurement that
 * matters, and it can only be taken with the real device clock driving the real
 * callback.
 *
 * The tap therefore records, per callback, what was played (or the silence a
 * starved callback amounts to) into a preallocated heap buffer -- no file I/O on
 * the realtime thread -- and then zeroes the hardware buffer. Enabling it makes
 * the emulator silent by construction, which is what lets this run on a machine
 * somebody is working at.
 */
typedef struct CATap {
    uint8_t *pcm;
    size_t cap, len;
    uint64_t calls, starved, frames_played, frames_silent;
    /* Margin: how many frames were queued when the IOProc ran, bucketed in
     * units of the device buffer. Bucket 0 is a dropout; bucket 1 means we
     * cleared the bar with nothing to spare, which is a dropout waiting for the
     * first main-loop hiccup. This is the number the whole diagnosis turns on. */
    uint64_t pend_hist[9];
    uint64_t pend_min, pend_sum;
    /* One byte per callback: 0 starved, 1 played silence, 2 played signal. A
     * starved callback between two that carried signal is an audible dropout;
     * one during an idle stretch is not, and the two are indistinguishable in
     * the recording because both come out as zeroes. */
    uint8_t *cls;
    size_t cls_n, cls_cap;
    /* Cost of the tap itself, on the realtime thread. The tap MUTES the device,
     * and the fair objection to that is "a muted device may not be timed like a
     * live one". The HAL's callback cadence comes from the device clock and is
     * independent of the sample values written, so the only way this tap can
     * perturb what it measures is by stealing time inside the IOProc. Measure
     * that directly rather than argue it. */
    uint64_t tap_ns_max, tap_ns_sum;
    char *path;
} CATap;

static CATap ca_tap;

static void ca_tap_dump(void)
{
    FILE *f;

    if (!ca_tap.path || !ca_tap.pcm) {
        return;
    }
    f = fopen(ca_tap.path, "wb");
    if (f) {
        fwrite(ca_tap.pcm, 1, ca_tap.len, f);
        fclose(f);
    }
    f = fopen(g_strdup_printf("%s.log", ca_tap.path), "w");
    if (f) {
        fprintf(f, "callbacks      %" PRIu64 "\n", ca_tap.calls);
        fprintf(f, "starved        %" PRIu64 "  (%.2f%%)\n", ca_tap.starved,
                ca_tap.calls ? ca_tap.starved * 100.0 / ca_tap.calls : 0.0);
        fprintf(f, "frames played  %" PRIu64 "\n", ca_tap.frames_played);
        fprintf(f, "frames silent  %" PRIu64 "\n", ca_tap.frames_silent);
        fprintf(f, "queued at callback: min %" PRIu64 " mean %.1f device buffers\n",
                ca_tap.pend_min,
                ca_tap.calls ? ca_tap.pend_sum * 1.0 / ca_tap.calls : 0.0);
        for (int i = 0; i < 9; i++) {
            fprintf(f, "  %s%d buf %8" PRIu64 "%s\n", i == 8 ? ">=" : " ", i,
                    ca_tap.pend_hist[i], i == 0 ? "   <-- DROPOUT" : "");
        }
        /*
         * A starved callback is only audible if it interrupts signal. Count the
         * ones with signal within one second either side; the rest are the
         * device sitting idle with the voice still open.
         */
        uint64_t audible = 0;
        const size_t win = 86;          /* ~1 s of 11.6 ms callbacks */
        for (size_t i = 0; i < ca_tap.cls_n; i++) {
            if (ca_tap.cls[i] != 0) {
                continue;
            }
            bool before = false, after = false;
            for (size_t j = i > win ? i - win : 0; j < i; j++) {
                before |= ca_tap.cls[j] == 2;
            }
            for (size_t j = i + 1; j < MIN(i + win, ca_tap.cls_n); j++) {
                after |= ca_tap.cls[j] == 2;
            }
            audible += before && after;
        }
        fprintf(f, "tap cost on the RT thread: mean %.1f us, max %.1f us, "
                "against an %.2f ms callback period\n",
                ca_tap.calls ? ca_tap.tap_ns_sum / 1000.0 / ca_tap.calls : 0.0,
                ca_tap.tap_ns_max / 1000.0,
                ca_tap.calls ? 512 * 1000.0 / 44.1 / 1000.0 : 0.0);
        fprintf(f, "starved DURING CONTENT %" PRIu64 "   <-- the audible ones\n",
                audible);
        fclose(f);
    }
    if (ca_tap.cls) {
        char *cp = g_strdup_printf("%s.cls", ca_tap.path);
        FILE *c = fopen(cp, "wb");
        if (c) {
            fwrite(ca_tap.cls, 1, ca_tap.cls_n, c);
            fclose(c);
        }
        g_free(cp);
    }
}

static void ca_tap_init(HWVoiceOut *hw)
{
    const char *p = getenv("IT_CA_TAP");

    if (!p || ca_tap.pcm) {
        return;
    }
    /* 180 s at the voice's own rate; a run that overruns it simply stops
     * recording rather than growing on the realtime thread. */
    ca_tap.cap = (size_t)hw->info.bytes_per_second * 180;
    ca_tap.pcm = g_malloc0(ca_tap.cap);
    ca_tap.cls_cap = 1 << 20;
    ca_tap.cls = g_malloc0(ca_tap.cls_cap);
    ca_tap.path = g_strdup(p);
    atexit(ca_tap_dump);
    info_report("coreaudio: IT_CA_TAP: muting output, recording %zu MiB "
                "to %s (f32le %d ch @ %d Hz)", ca_tap.cap >> 20, p,
                hw->info.nchannels, hw->info.freq);
}

static OSStatus out_device_ioproc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    UInt32 frame_size, pending_frames;
    void *out = outOutputData->mBuffers[0].mData;
    HWVoiceOut *hw = hwptr;
    CoreaudioVoiceOut *core = hwptr;
    size_t len;

    if (coreaudio_voice_out_buf_lock(core, "out_device_ioproc")) {
        inInputTime = 0;
        return 0;
    }

    if (inDevice != core->device_id) {
        coreaudio_voice_out_buf_unlock(core, "out_device_ioproc(old device)");
        return 0;
    }

    frame_size = core->device_frame_size;
    pending_frames = hw->pending_emul / hw->info.bytes_per_frame;

    if (ca_tap.pcm && frame_size) {
        unsigned b = pending_frames / frame_size;
        ca_tap.pend_hist[MIN(b, 8u)]++;
        ca_tap.pend_sum += b;
        if (pending_frames < ca_tap.pend_min * frame_size || !ca_tap.calls) {
            ca_tap.pend_min = b;
        }
    }

    /*
     * If there are not enough samples, set signal and return.
     *
     * Clear the hardware buffer first. Upstream returns leaving it untouched,
     * so the device re-plays whatever it held -- the previous 11.6 ms, over and
     * over for as long as the starvation lasts. That turns a dropout into a
     * repeated fragment, which is far more audible than the silence it stands
     * in for: measured on the iPod touch shutter, a single starved period in
     * the middle of a 500 ms clip is what "recognizable but garbled" was.
     * Silence is the honest thing to play when there is nothing to play.
     */
    if (pending_frames < frame_size) {
        memset(out, 0, frame_size * core->dev_nchannels *
                       (hw->info.bytes_per_frame / hw->info.nchannels));
        if (ca_tap.pcm) {
            size_t n = frame_size * hw->info.bytes_per_frame;
            ca_tap.calls++;
            ca_tap.starved++;
            ca_tap.frames_silent += frame_size;
            if (ca_tap.cls_n < ca_tap.cls_cap) {
                ca_tap.cls[ca_tap.cls_n++] = 0;
            }
            if (ca_tap.len + n <= ca_tap.cap) {
                memset(ca_tap.pcm + ca_tap.len, 0, n);   /* what a dropout is */
                ca_tap.len += n;
            }
        }
        inInputTime = 0;
        coreaudio_voice_out_buf_unlock(core, "out_device_ioproc(empty)");
        return 0;
    }

    len = frame_size * hw->info.bytes_per_frame;
    if (ca_tap.pcm) {
        ca_tap.calls++;
        ca_tap.frames_played += frame_size;
    }

    if (core->dev_nchannels != hw->info.nchannels) {
        /*
         * The device runs a different channel count than we produce, so the
         * samples cannot simply be poured in: each of our frames occupies one
         * device frame, of which we fill the leading channels and leave the
         * rest silent. Zero first, so any channel we do not drive (and any
         * frame we run short on) is silence rather than whatever the device
         * held.
         */
        const unsigned ssz = hw->info.bytes_per_frame / hw->info.nchannels;
        const unsigned scpy = MIN(core->dev_nchannels, hw->info.nchannels);
        uint8_t *dst = out;
        size_t frames_done = 0;

        memset(out, 0, (size_t)frame_size * core->dev_nchannels * ssz);

        while (len && hw->pending_emul) {
            size_t write_len, start, nframes, f;
            const uint8_t *src;

            start = audio_ring_posb(hw->pos_emul, hw->pending_emul,
                                    hw->size_emul);
            assert(start < hw->size_emul);

            write_len = MIN(MIN(hw->pending_emul, len),
                            hw->size_emul - start);
            nframes = write_len / hw->info.bytes_per_frame;
            src = hw->buf_emul + start;

            for (f = 0; f < nframes; f++) {
                memcpy(dst + (frames_done + f) * core->dev_nchannels * ssz,
                       src + f * hw->info.bytes_per_frame,
                       (size_t)scpy * ssz);
            }

            frames_done += nframes;
            write_len = nframes * hw->info.bytes_per_frame;
            if (!write_len) {
                break;              /* partial frame: leave it for next time */
            }
            hw->pending_emul -= write_len;
            len -= write_len;
        }
    } else {
        while (len) {
            size_t write_len, start;

            start = audio_ring_posb(hw->pos_emul, hw->pending_emul,
                                    hw->size_emul);
            assert(start < hw->size_emul);

            write_len = MIN(MIN(hw->pending_emul, len),
                            hw->size_emul - start);

            memcpy(out, hw->buf_emul + start, write_len);
            hw->pending_emul -= write_len;
            len -= write_len;
            out += write_len;
        }
    }

    if (ca_tap.pcm) {
        int64_t t_in = qemu_clock_get_ns(QEMU_CLOCK_HOST);
        size_t n = frame_size * hw->info.bytes_per_frame;
        const float *f = outOutputData->mBuffers[0].mData;
        bool signal = false;
        for (size_t i = 0; i < n / sizeof(float); i++) {
            if (f[i] != 0.0f) {
                signal = true;
                break;
            }
        }
        if (ca_tap.cls_n < ca_tap.cls_cap) {
            ca_tap.cls[ca_tap.cls_n++] = signal ? 2 : 1;
        }
        if (ca_tap.len + n <= ca_tap.cap) {
            memcpy(ca_tap.pcm + ca_tap.len, outOutputData->mBuffers[0].mData, n);
            ca_tap.len += n;
        }
        memset(outOutputData->mBuffers[0].mData, 0, n);   /* mute the speakers */
        int64_t dt = qemu_clock_get_ns(QEMU_CLOCK_HOST) - t_in;
        if (dt > 0) {
            ca_tap.tap_ns_sum += dt;
            if ((uint64_t)dt > ca_tap.tap_ns_max) {
                ca_tap.tap_ns_max = dt;
            }
        }
    }

    coreaudio_voice_out_buf_unlock(core, "out_device_ioproc");
    return 0;
}

/* called without BQL. The same device, now at another nominal rate. */
static OSStatus handle_rate_change(
    AudioObjectID in_object_id,
    UInt32 in_number_addresses,
    const AudioObjectPropertyAddress *in_addresses,
    void *in_client_data)
{
    CoreaudioVoiceOut *core = in_client_data;
    Float64 rate;

    bql_lock();
    if (in_object_id == core->device_id &&
        coreaudio_get_rate(in_object_id, &rate) == kAudioHardwareNoError &&
        rate >= 8000 &&
        !coreaudio_voice_out_buf_lock(core, "handle_rate_change")) {
        audio_pcm_hw_set_freq_out(&core->hw, (int)rate);
        coreaudio_voice_out_buf_unlock(core, "handle_rate_change");
    }
    bql_unlock();
    return 0;
}

static OSStatus init_out_device(CoreaudioVoiceOut *core)
{
    AudioDeviceID device_id;
    AudioDeviceIOProcID ioprocid;
    AudioValueRange value_range;
    OSStatus status;
    UInt32 device_frame_size;

    AudioStreamBasicDescription stream_basic_description = {
        .mBitsPerChannel = audio_format_bits(core->hw.info.af),
        .mBytesPerFrame = core->hw.info.bytes_per_frame,
        .mBytesPerPacket = core->hw.info.bytes_per_frame,
        .mChannelsPerFrame = core->hw.info.nchannels,
        .mFormatFlags = kLinearPCMFormatFlagIsFloat,
        .mFormatID = kAudioFormatLinearPCM,
        .mFramesPerPacket = 1,
        .mSampleRate = core->hw.info.freq
    };

    /*
     * Assume the device runs what we produce until we have asked it; several
     * paths below return early, and a zero here would make the IOProc scatter
     * into nothing and play silence.
     */
    core->dev_nchannels = core->hw.info.nchannels;

    status = coreaudio_get_voice_out(&device_id);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get default output device");
        return status;
    }
    if (device_id == kAudioDeviceUnknown) {
        error_report("coreaudio: Could not initialize playback: "
                     "Unknown audio device");
        return status;
    }

    /*
     * Play at the rate the device runs, never ask it to run ours. The voice
     * used to stay at the audiodev's 44.1 kHz and ask the device for 44.1 kHz
     * too: a device that cannot (AirPods: 48 kHz, 24 or 16 kHz with the mic
     * open) keeps its own rate, takes the 44.1 kHz frames as its own and
     * plays them 8.8% sharp, draining them faster than the guest makes them
     * -- the crackle. One that can was switched for every app on the Mac.
     * Re-rating the voice makes mixeng resample the guest to the device.
     */
    {
        Float64 rate = 0;

        if (coreaudio_get_rate(device_id, &rate) == kAudioHardwareNoError &&
            rate >= 8000) {
            audio_pcm_hw_set_freq_out(&core->hw, (int)rate);
            stream_basic_description.mSampleRate = rate;
        }
    }

    /* get minimum and maximum buffer frame sizes */
    status = coreaudio_get_out_framesizerange(device_id, &value_range);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get device buffer frame range");
        return status;
    }

    if (value_range.mMinimum > core->frame_size_setting) {
        device_frame_size = value_range.mMinimum;
        warn_report("coreaudio: Upsizing buffer frames to %f",
                    value_range.mMinimum);
    } else if (value_range.mMaximum < core->frame_size_setting) {
        device_frame_size = value_range.mMaximum;
        warn_report("coreaudio: Downsizing buffer frames to %f",
                    value_range.mMaximum);
    } else {
        device_frame_size = core->frame_size_setting;
    }

    /* set Buffer Frame Size */
    status = coreaudio_set_out_framesize(device_id, &device_frame_size);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not set device buffer frame size %" PRIu32,
                                  (uint32_t)device_frame_size);
        return status;
    }

    /* get Buffer Frame Size */
    status = coreaudio_get_out_framesize(device_id, &device_frame_size);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not get device buffer frame size");
        return status;
    }

    /* set Samplerate */
    status = coreaudio_set_out_streamformat(device_id,
                                            &stream_basic_description);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not set samplerate %lf",
                                  stream_basic_description.mSampleRate);
        return status;
    }

    /*
     * Ask the device what it is ACTUALLY running, rather than assuming it took
     * what we just asked for. Setting the stream format can succeed without
     * giving us the channel count we requested -- a multi-channel output (the
     * Studio Display's speakers report 8) stays multi-channel, and then the
     * IOProc's buffer is dev_nchannels wide while our samples are 2 wide. The
     * old code wrote stereo frames into it contiguously, so every frame landed
     * at the wrong stride and the result was unintelligible garbage for any
     * audio at all, guest-generated or not.
     */
    {
        AudioStreamBasicDescription actual = { 0 };
        UInt32 sz = sizeof(actual);
        AudioObjectPropertyAddress addr = {
            kAudioDevicePropertyStreamFormat,
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        OSStatus s2 = AudioObjectGetPropertyData(device_id, &addr,
                                                 0, NULL, &sz, &actual);

        core->dev_nchannels = (s2 == kAudioHardwareNoError &&
                               actual.mChannelsPerFrame)
                              ? actual.mChannelsPerFrame
                              : core->hw.info.nchannels;
        if (core->dev_nchannels != core->hw.info.nchannels) {
            info_report("coreaudio: device runs %u channels, we produce %u -- "
                        "scattering into the first %u",
                        (unsigned)core->dev_nchannels,
                        (unsigned)core->hw.info.nchannels,
                        (unsigned)MIN(core->dev_nchannels,
                                      core->hw.info.nchannels));
        }
    }

    /*
     * set Callback.
     *
     * On macOS 11.3.1, Core Audio calls AudioDeviceIOProc after calling an
     * internal function named HALB_Mutex::Lock(), which locks a mutex in
     * HALB_IOThread::Entry(void*). HALB_Mutex::Lock() is also called in
     * AudioObjectGetPropertyData, which is called by coreaudio driver.
     * Therefore, the specified callback must be designed to avoid a deadlock
     * with the callers of AudioObjectGetPropertyData.
     */
    ioprocid = NULL;
    status = AudioDeviceCreateIOProcID(device_id,
                                       out_device_ioproc,
                                       &core->hw,
                                       &ioprocid);
    if (status == kAudioHardwareBadDeviceError) {
        return 0;
    }
    if (status != kAudioHardwareNoError || ioprocid == NULL) {
        coreaudio_playback_logerr(status, "Could not set IOProc");
        return status;
    }

    core->device_id = device_id;
    core->device_frame_size = device_frame_size;
    core->hw.samples = core->buffer_count * core->device_frame_size;
    audio_generic_initialize_buffer_out(&core->hw);
    core->ioprocid = ioprocid;

    /* Follow the device's rate as it changes (AirPods to and from HFP). */
    AudioObjectAddPropertyListener(device_id, &rate_addr,
                                   handle_rate_change, core);
    return 0;
}

static void fini_out_device(CoreaudioVoiceOut *core)
{
    OSStatus status;
    UInt32 isrunning;

    AudioObjectRemovePropertyListener(core->device_id, &rate_addr,
                                      handle_rate_change, core);

    /* stop playback */
    status = coreaudio_get_out_isrunning(core->device_id, &isrunning);
    if (status != kAudioHardwareBadObjectError) {
        if (status != kAudioHardwareNoError) {
            coreaudio_logerr(status,
                             "Could not determine whether device is playing");
        }

        if (isrunning) {
            status = AudioDeviceStop(core->device_id, core->ioprocid);
            if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
                coreaudio_logerr(status, "Could not stop playback");
            }
        }
    }

    /* remove callback */
    status = AudioDeviceDestroyIOProcID(core->device_id,
                                        core->ioprocid);
    if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove IOProc");
    }
    core->device_id = kAudioDeviceUnknown;
}

static void update_out_device_playback_state(CoreaudioVoiceOut *core)
{
    OSStatus status;
    UInt32 isrunning;

    status = coreaudio_get_out_isrunning(core->device_id, &isrunning);
    if (status != kAudioHardwareNoError) {
        if (status != kAudioHardwareBadObjectError) {
            coreaudio_logerr(status,
                             "Could not determine whether device is playing");
        }

        return;
    }

    /*
     * Start once, never stop. AudioDeviceStart takes 30-45 ms and runs with
     * the BQL held, freezing the whole machine -- guest included -- at the
     * start of EVERY stream. On the iPod touch that freeze lands ~180 ms into
     * each sound (the voice activates after a prebuffer), and the guest's
     * mixer, whose clock runs through the freeze, re-anchors and skips 3-4
     * ring pages: pages of silence in the middle of the clip. Keeping the
     * device running makes activation free. A running IOProc with an idle
     * voice takes the starvation path, which plays (and now clears to)
     * silence, so an open-but-quiet device is inaudible by construction.
     */
    if (!isrunning) {
        status = AudioDeviceStart(core->device_id, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
            coreaudio_logerr(status, "Could not resume playback");
        }
    }
}

/* called without BQL. */
static OSStatus handle_voice_out_change(
    AudioObjectID in_object_id,
    UInt32 in_number_addresses,
    const AudioObjectPropertyAddress *in_addresses,
    void *in_client_data)
{
    CoreaudioVoiceOut *core = in_client_data;

    bql_lock();

    if (core->device_id) {
        fini_out_device(core);
    }

    init_out_device(core);

    if (core->device_id) {
        update_out_device_playback_state(core);
    }

    bql_unlock();
    return 0;
}

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    OSStatus status;
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;
    int err;
    Audiodev *dev = hw->s->dev;
    AudiodevCoreaudioPerDirectionOptions *cpdo = dev->u.coreaudio.out;
    struct audsettings obt_as;

    /* create mutex */
    err = pthread_mutex_init(&core->buf_mutex, NULL);
    if (err) {
        error_report("coreaudio: Could not create mutex: %s", strerror(err));
        return -1;
    }

    obt_as = *as;
    as = &obt_as;
    as->fmt = AUDIO_FORMAT_F32;
    audio_pcm_init_info(&hw->info, as);

    core->frame_size_setting = audio_buffer_frames(
        qapi_AudiodevCoreaudioPerDirectionOptions_base(cpdo), as, 11610);

    core->buffer_count = cpdo->has_buffer_count ? cpdo->buffer_count : 4;

    ca_tap_init(&core->hw);

    status = AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                            &voice_out_addr,
                                            handle_voice_out_change,
                                            core);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr(status,
                                  "Could not listen to voice property change");
        return -1;
    }

    if (init_out_device(core)) {
        status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                                   &voice_out_addr,
                                                   handle_voice_out_change,
                                                   core);
        if (status != kAudioHardwareNoError) {
            coreaudio_playback_logerr(status,
                                      "Could not remove voice property change listener");
        }

        return -1;
    }

    /* Pay the expensive first AudioDeviceStart here, at device open during
     * machine bring-up, not on the guest's timeline (see
     * update_out_device_playback_state). */
    update_out_device_playback_state(core);

    return 0;
}

static void coreaudio_fini_out (HWVoiceOut *hw)
{
    OSStatus status;
    int err;
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;

    status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                               &voice_out_addr,
                                               handle_voice_out_change,
                                               core);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove voice property change listener");
    }

    fini_out_device(core);

    /* destroy mutex */
    err = pthread_mutex_destroy(&core->buf_mutex);
    if (err) {
        error_report("coreaudio: Could not destroy mutex: %s", strerror(err));
    }
}

static void coreaudio_enable_out(HWVoiceOut *hw, bool enable)
{
    CoreaudioVoiceOut *core = (CoreaudioVoiceOut *)hw;

    core->enabled = enable;
    update_out_device_playback_state(core);
}

static void audio_coreaudio_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 0;
    k->voice_size_out = sizeof(CoreaudioVoiceOut);
    k->voice_size_in = 0;

    k->init_out = coreaudio_init_out;
    k->fini_out = coreaudio_fini_out;
    /* wrapper for audio_generic_write */
    k->write = coreaudio_write;
    /* wrapper for audio_generic_buffer_get_free */
    k->buffer_get_free = coreaudio_buffer_get_free;
    /* wrapper for audio_generic_get_buffer_out */
    k->get_buffer_out = coreaudio_get_buffer_out;
    /* wrapper for audio_generic_put_buffer_out */
    k->put_buffer_out = coreaudio_put_buffer_out;
    k->enable_out = coreaudio_enable_out;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_COREAUDIO,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioCoreaudio),
        .class_init = audio_coreaudio_class_init,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_COREAUDIO);
