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
#include "audio.h"

#define AUDIO_CAP "coreaudio"
#include "audio_int.h"

typedef struct coreaudioVoiceOut {
    HWVoiceOut hw;
    pthread_mutex_t buf_mutex;
    AudioDeviceID outputDeviceID;
    int frameSizeSetting;
    uint32_t bufferCount;
    UInt32 audioDevicePropertyBufferFrameSize;
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
} coreaudioVoiceOut;

static const AudioObjectPropertyAddress voice_addr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};

static OSStatus coreaudio_get_voice(AudioDeviceID *id)
{
    UInt32 size = sizeof(*id);

    return AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                      &voice_addr,
                                      0,
                                      NULL,
                                      &size,
                                      id);
}

static OSStatus coreaudio_get_framesizerange(AudioDeviceID id,
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

static OSStatus coreaudio_get_framesize(AudioDeviceID id, UInt32 *framesize)
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

static OSStatus coreaudio_set_framesize(AudioDeviceID id, UInt32 *framesize)
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

static OSStatus coreaudio_set_streamformat(AudioDeviceID id,
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

static OSStatus coreaudio_get_isrunning(AudioDeviceID id, UInt32 *result)
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

static void coreaudio_logstatus (OSStatus status)
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
        AUD_log (AUDIO_CAP, "Reason: status code %" PRId32 "\n", (int32_t)status);
        return;
    }

    AUD_log (AUDIO_CAP, "Reason: %s\n", str);
}

static void G_GNUC_PRINTF (2, 3) coreaudio_logerr (
    OSStatus status,
    const char *fmt,
    ...
    )
{
    va_list ap;

    va_start (ap, fmt);
    AUD_log (AUDIO_CAP, fmt, ap);
    va_end (ap);

    coreaudio_logstatus (status);
}

static void G_GNUC_PRINTF (3, 4) coreaudio_logerr2 (
    OSStatus status,
    const char *typ,
    const char *fmt,
    ...
    )
{
    va_list ap;

    AUD_log (AUDIO_CAP, "Could not initialize %s\n", typ);

    va_start (ap, fmt);
    AUD_vlog (AUDIO_CAP, fmt, ap);
    va_end (ap);

    coreaudio_logstatus (status);
}

#define coreaudio_playback_logerr(status, ...) \
    coreaudio_logerr2(status, "playback", __VA_ARGS__)

static int coreaudio_buf_lock (coreaudioVoiceOut *core, const char *fn_name)
{
    int err;

    err = pthread_mutex_lock (&core->buf_mutex);
    if (err) {
        dolog ("Could not lock voice for %s\nReason: %s\n",
               fn_name, strerror (err));
        return -1;
    }
    return 0;
}

static int coreaudio_buf_unlock (coreaudioVoiceOut *core, const char *fn_name)
{
    int err;

    err = pthread_mutex_unlock (&core->buf_mutex);
    if (err) {
        dolog ("Could not unlock voice for %s\nReason: %s\n",
               fn_name, strerror (err));
        return -1;
    }
    return 0;
}

#define COREAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args) \
    static ret_type glue(coreaudio_, name)args_decl             \
    {                                                           \
        coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;     \
        ret_type ret;                                           \
                                                                \
        if (coreaudio_buf_lock(core, "coreaudio_" #name)) {         \
            return 0;                                           \
        }                                                       \
                                                                \
        ret = glue(audio_generic_, name)args;                   \
                                                                \
        coreaudio_buf_unlock(core, "coreaudio_" #name);             \
        return ret;                                             \
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
    dolog("IT_CA_TAP: muting output, recording %zu MiB to %s "
          "(f32le %d ch @ %d Hz)\n", ca_tap.cap >> 20, p,
          hw->info.nchannels, hw->info.freq);
}

static OSStatus audioDeviceIOProc(
    AudioDeviceID inDevice,
    const AudioTimeStamp *inNow,
    const AudioBufferList *inInputData,
    const AudioTimeStamp *inInputTime,
    AudioBufferList *outOutputData,
    const AudioTimeStamp *inOutputTime,
    void *hwptr)
{
    UInt32 frameCount, pending_frames;
    void *out = outOutputData->mBuffers[0].mData;
    HWVoiceOut *hw = hwptr;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hwptr;
    size_t len;

    if (coreaudio_buf_lock (core, "audioDeviceIOProc")) {
        inInputTime = 0;
        return 0;
    }

    if (inDevice != core->outputDeviceID) {
        coreaudio_buf_unlock (core, "audioDeviceIOProc(old device)");
        return 0;
    }

    frameCount = core->audioDevicePropertyBufferFrameSize;
    pending_frames = hw->pending_emul / hw->info.bytes_per_frame;

    if (ca_tap.pcm && frameCount) {
        unsigned b = pending_frames / frameCount;
        ca_tap.pend_hist[MIN(b, 8u)]++;
        ca_tap.pend_sum += b;
        if (pending_frames < ca_tap.pend_min * frameCount || !ca_tap.calls) {
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
     * the middle of a 500 ms clip is what "recognisable but garbled" was.
     * Silence is the honest thing to play when there is nothing to play.
     */
    if (pending_frames < frameCount) {
        memset(out, 0, frameCount * core->dev_nchannels *
                       (hw->info.bytes_per_frame / hw->info.nchannels));
        if (ca_tap.pcm) {
            size_t n = frameCount * hw->info.bytes_per_frame;
            ca_tap.calls++;
            ca_tap.starved++;
            ca_tap.frames_silent += frameCount;
            if (ca_tap.cls_n < ca_tap.cls_cap) {
                ca_tap.cls[ca_tap.cls_n++] = 0;
            }
            if (ca_tap.len + n <= ca_tap.cap) {
                memset(ca_tap.pcm + ca_tap.len, 0, n);   /* what a dropout is */
                ca_tap.len += n;
            }
        }
        inInputTime = 0;
        coreaudio_buf_unlock (core, "audioDeviceIOProc(empty)");
        return 0;
    }

    len = frameCount * hw->info.bytes_per_frame;
    if (ca_tap.pcm) {
        ca_tap.calls++;
        ca_tap.frames_played += frameCount;
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

        memset(out, 0, (size_t)frameCount * core->dev_nchannels * ssz);

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
        size_t n = frameCount * hw->info.bytes_per_frame;
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

    coreaudio_buf_unlock (core, "audioDeviceIOProc");
    return 0;
}

static OSStatus init_out_device(coreaudioVoiceOut *core)
{
    OSStatus status;
    AudioValueRange frameRange;

    AudioStreamBasicDescription streamBasicDescription = {
        .mBitsPerChannel = core->hw.info.bits,
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

    status = coreaudio_get_voice(&core->outputDeviceID);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                   "Could not get default output Device\n");
        return status;
    }
    if (core->outputDeviceID == kAudioDeviceUnknown) {
        dolog ("Could not initialize playback - Unknown Audiodevice\n");
        return status;
    }

    /* get minimum and maximum buffer frame sizes */
    status = coreaudio_get_framesizerange(core->outputDeviceID,
                                          &frameRange);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                    "Could not get device buffer frame range\n");
        return status;
    }

    if (frameRange.mMinimum > core->frameSizeSetting) {
        core->audioDevicePropertyBufferFrameSize = (UInt32) frameRange.mMinimum;
        dolog ("warning: Upsizing Buffer Frames to %f\n", frameRange.mMinimum);
    } else if (frameRange.mMaximum < core->frameSizeSetting) {
        core->audioDevicePropertyBufferFrameSize = (UInt32) frameRange.mMaximum;
        dolog ("warning: Downsizing Buffer Frames to %f\n", frameRange.mMaximum);
    } else {
        core->audioDevicePropertyBufferFrameSize = core->frameSizeSetting;
    }

    /* set Buffer Frame Size */
    status = coreaudio_set_framesize(core->outputDeviceID,
                                     &core->audioDevicePropertyBufferFrameSize);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                    "Could not set device buffer frame size %" PRIu32 "\n",
                                    (uint32_t)core->audioDevicePropertyBufferFrameSize);
        return status;
    }

    /* get Buffer Frame Size */
    status = coreaudio_get_framesize(core->outputDeviceID,
                                     &core->audioDevicePropertyBufferFrameSize);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                    "Could not get device buffer frame size\n");
        return status;
    }
    core->hw.samples = core->bufferCount * core->audioDevicePropertyBufferFrameSize;

    /* set Samplerate */
    status = coreaudio_set_streamformat(core->outputDeviceID,
                                        &streamBasicDescription);
    if (status == kAudioHardwareBadObjectError) {
        return 0;
    }
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                   "Could not set samplerate %lf\n",
                                   streamBasicDescription.mSampleRate);
        core->outputDeviceID = kAudioDeviceUnknown;
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
        OSStatus s2 = AudioObjectGetPropertyData(core->outputDeviceID, &addr,
                                                 0, NULL, &sz, &actual);

        core->dev_nchannels = (s2 == kAudioHardwareNoError &&
                               actual.mChannelsPerFrame)
                              ? actual.mChannelsPerFrame
                              : core->hw.info.nchannels;
        if (core->dev_nchannels != core->hw.info.nchannels) {
            dolog("device runs %u channels, we produce %u -- "
                  "scattering into the first %u\n",
                  (unsigned)core->dev_nchannels,
                  (unsigned)core->hw.info.nchannels,
                  (unsigned)MIN(core->dev_nchannels, core->hw.info.nchannels));
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
    core->ioprocid = NULL;
    status = AudioDeviceCreateIOProcID(core->outputDeviceID,
                                       audioDeviceIOProc,
                                       &core->hw,
                                       &core->ioprocid);
    if (status == kAudioHardwareBadDeviceError) {
        return 0;
    }
    if (status != kAudioHardwareNoError || core->ioprocid == NULL) {
        coreaudio_playback_logerr (status, "Could not set IOProc\n");
        core->outputDeviceID = kAudioDeviceUnknown;
        return status;
    }

    return 0;
}

static void fini_out_device(coreaudioVoiceOut *core)
{
    OSStatus status;
    UInt32 isrunning;

    /* stop playback */
    status = coreaudio_get_isrunning(core->outputDeviceID, &isrunning);
    if (status != kAudioHardwareBadObjectError) {
        if (status != kAudioHardwareNoError) {
            coreaudio_logerr(status,
                             "Could not determine whether Device is playing\n");
        }

        if (isrunning) {
            status = AudioDeviceStop(core->outputDeviceID, core->ioprocid);
            if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
                coreaudio_logerr(status, "Could not stop playback\n");
            }
        }
    }

    /* remove callback */
    status = AudioDeviceDestroyIOProcID(core->outputDeviceID,
                                        core->ioprocid);
    if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove IOProc\n");
    }
    core->outputDeviceID = kAudioDeviceUnknown;
}

static void update_device_playback_state(coreaudioVoiceOut *core)
{
    OSStatus status;
    UInt32 isrunning;

    status = coreaudio_get_isrunning(core->outputDeviceID, &isrunning);
    if (status != kAudioHardwareNoError) {
        if (status != kAudioHardwareBadObjectError) {
            coreaudio_logerr(status,
                             "Could not determine whether Device is playing\n");
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
        status = AudioDeviceStart(core->outputDeviceID, core->ioprocid);
        if (status != kAudioHardwareBadDeviceError && status != kAudioHardwareNoError) {
            coreaudio_logerr (status, "Could not resume playback\n");
        }
    }
}

/* called without BQL. */
static OSStatus handle_voice_change(
    AudioObjectID in_object_id,
    UInt32 in_number_addresses,
    const AudioObjectPropertyAddress *in_addresses,
    void *in_client_data)
{
    coreaudioVoiceOut *core = in_client_data;

    bql_lock();

    if (core->outputDeviceID) {
        fini_out_device(core);
    }

    if (!init_out_device(core)) {
        update_device_playback_state(core);
    }

    bql_unlock();
    return 0;
}

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *as,
                              void *drv_opaque)
{
    OSStatus status;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;
    int err;
    Audiodev *dev = drv_opaque;
    AudiodevCoreaudioPerDirectionOptions *cpdo = dev->u.coreaudio.out;
    struct audsettings obt_as;

    /* create mutex */
    err = pthread_mutex_init(&core->buf_mutex, NULL);
    if (err) {
        dolog("Could not create mutex\nReason: %s\n", strerror (err));
        return -1;
    }

    obt_as = *as;
    as = &obt_as;
    as->fmt = AUDIO_FORMAT_F32;
    audio_pcm_init_info (&hw->info, as);

    core->frameSizeSetting = audio_buffer_frames(
        qapi_AudiodevCoreaudioPerDirectionOptions_base(cpdo), as, 11610);

    core->bufferCount = cpdo->has_buffer_count ? cpdo->buffer_count : 4;

    ca_tap_init(&core->hw);

    status = AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                            &voice_addr, handle_voice_change,
                                            core);
    if (status != kAudioHardwareNoError) {
        coreaudio_playback_logerr (status,
                                   "Could not listen to voice property change\n");
        return -1;
    }

    if (init_out_device(core)) {
        status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                                   &voice_addr,
                                                   handle_voice_change,
                                                   core);
        if (status != kAudioHardwareNoError) {
            coreaudio_playback_logerr(status,
                                      "Could not remove voice property change listener\n");
        }

        return -1;
    }

    /* Pay the expensive first AudioDeviceStart here, at device open during
     * machine bring-up, not on the guest's timeline (see
     * update_device_playback_state). */
    update_device_playback_state(core);

    return 0;
}

static void coreaudio_fini_out (HWVoiceOut *hw)
{
    OSStatus status;
    int err;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;

    status = AudioObjectRemovePropertyListener(kAudioObjectSystemObject,
                                               &voice_addr,
                                               handle_voice_change,
                                               core);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr(status, "Could not remove voice property change listener\n");
    }

    fini_out_device(core);

    /* destroy mutex */
    err = pthread_mutex_destroy(&core->buf_mutex);
    if (err) {
        dolog("Could not destroy mutex\nReason: %s\n", strerror (err));
    }
}

static void coreaudio_enable_out(HWVoiceOut *hw, bool enable)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;

    core->enabled = enable;
    update_device_playback_state(core);
}

static void *coreaudio_audio_init(Audiodev *dev, Error **errp)
{
    return dev;
}

static void coreaudio_audio_fini (void *opaque)
{
}

static struct audio_pcm_ops coreaudio_pcm_ops = {
    .init_out = coreaudio_init_out,
    .fini_out = coreaudio_fini_out,
  /* wrapper for audio_generic_write */
    .write    = coreaudio_write,
  /* wrapper for audio_generic_buffer_get_free */
    .buffer_get_free = coreaudio_buffer_get_free,
  /* wrapper for audio_generic_get_buffer_out */
    .get_buffer_out = coreaudio_get_buffer_out,
  /* wrapper for audio_generic_put_buffer_out */
    .put_buffer_out = coreaudio_put_buffer_out,
    .enable_out = coreaudio_enable_out
};

static struct audio_driver coreaudio_audio_driver = {
    .name           = "coreaudio",
    .descr          = "CoreAudio http://developer.apple.com/audio/coreaudio.html",
    .init           = coreaudio_audio_init,
    .fini           = coreaudio_audio_fini,
    .pcm_ops        = &coreaudio_pcm_ops,
    .max_voices_out = 1,
    .max_voices_in  = 0,
    .voice_size_out = sizeof (coreaudioVoiceOut),
    .voice_size_in  = 0
};

static void register_audio_coreaudio(void)
{
    audio_driver_register(&coreaudio_audio_driver);
}
type_init(register_audio_coreaudio);
