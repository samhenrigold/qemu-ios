/*
 * it_mictest: record from the built-in microphone with an AudioQueue and
 * report what arrived, for the ipad1 machine's capture path (i2s0 RX via
 * CDMA 0x1b). Test-only, never baked into shipping images.
 *
 * Waits DELAY_S for SpringBoard to settle, records RECORD_S of 44.1 kHz stereo
 * S16 and prints to
 * stderr (the job sends it to /dev/console = the serial log) one line:
 *   MICTEST frames=N secs=T rate=R freq=F peak=P glitches=G
 * rate = frames delivered / wall time, freq = the left channel's
 * zero-crossing frequency; with -global s5l8930.i2s.tone-hz=1000 both should
 * read 44100 and 1000.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include <stdarg.h>

/* Report on fd 2, which the launchd job points at /dev/console (the serial
 * log). Not stdio's stderr: 3.2's libSystem has no __stderrp, so a binary that
 * references it dies in dyld before main. */
static void say(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(2, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}
#define printf say

/* The toolchain cannot link the 3.2 SDK's framework stubs, so every framework
 * call goes through dlsym (as it_pbd does); the headers only supply types. */
#define FN(ret, name, args) static ret (*p_##name) args;
FN(OSStatus, AudioSessionInitialize, (CFRunLoopRef, CFStringRef, AudioSessionInterruptionListener, void *))
FN(OSStatus, AudioSessionSetProperty, (AudioSessionPropertyID, UInt32, const void *))
FN(OSStatus, AudioSessionSetActive, (Boolean))
FN(OSStatus, AudioQueueNewInput, (const AudioStreamBasicDescription *, AudioQueueInputCallback, void *,
                                  CFRunLoopRef, CFStringRef, UInt32, AudioQueueRef *))
FN(OSStatus, AudioQueueAllocateBuffer, (AudioQueueRef, UInt32, AudioQueueBufferRef *))
FN(OSStatus, AudioQueueEnqueueBuffer, (AudioQueueRef, AudioQueueBufferRef, UInt32,
                                       const AudioStreamPacketDescription *))
FN(OSStatus, AudioQueueStart, (AudioQueueRef, const AudioTimeStamp *))
FN(OSStatus, AudioQueueStop, (AudioQueueRef, Boolean))
FN(CFRunLoopRef, CFRunLoopGetCurrent, (void))
FN(SInt32, CFRunLoopRunInMode, (CFStringRef, CFTimeInterval, Boolean))
FN(uint64_t, mach_absolute_time, (void))
static CFStringRef *p_kCFRunLoopCommonModes, *p_kCFRunLoopDefaultMode;

static int resolve(void)
{
    void *at = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
#define R(lib, name) if (!(*(void **)&p_##name = dlsym(lib, #name))) { printf("MICTEST error dlsym %s\n", #name); return 0; }
    if (!at || !cf) {
        printf("MICTEST error dlopen\n");
        return 0;
    }
    R(at, AudioSessionInitialize) R(at, AudioSessionSetProperty) R(at, AudioSessionSetActive)
    R(at, AudioQueueNewInput) R(at, AudioQueueAllocateBuffer) R(at, AudioQueueEnqueueBuffer)
    R(at, AudioQueueStart) R(at, AudioQueueStop) R(cf, CFRunLoopGetCurrent) R(cf, CFRunLoopRunInMode)
    R(cf, kCFRunLoopCommonModes) R(cf, kCFRunLoopDefaultMode)
    R(RTLD_DEFAULT, mach_absolute_time)
    return 1;
}

static long frames, crossings, peak, glitches;
static int last_sign, prev1, prev2, have;
static long gpos[12];

static void input_cb(void *user, AudioQueueRef q, AudioQueueBufferRef buf,
                     const AudioTimeStamp *ts, UInt32 npkt,
                     const AudioStreamPacketDescription *desc)
{
    const SInt16 *p = buf->mAudioData;
    UInt32 n = buf->mAudioDataByteSize / 4;

    for (UInt32 i = 0; i < n; i++) {
        int v = p[2 * i];
        /* A pure tone has a small second difference; a dropped or repeated
         * stretch of samples shows up as one large jump. */
        if (have >= 2 && abs(v - 2 * prev1 + prev2) > 1000) {
            if (glitches < 12) {
                gpos[glitches] = have;
            }
            glitches++;
        }
        prev2 = prev1;
        prev1 = v;
        have++;
        int sign = v > 0 ? 1 : v < 0 ? -1 : 0;

        if (sign && last_sign && sign != last_sign) {
            crossings++;
        }
        if (sign) {
            last_sign = sign;
        }
        if (abs(v) > peak) {
            peak = abs(v);
        }
    }
    frames += n;
    p_AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

/* The timebase, not gettimeofday: calendar time is slewed toward the PMU RTC
 * (host wall clock) and does not measure the machine's own rate. */
static double now(void)
{
    /* 24 MHz, numer = denom = 1. Seconds since the first call, through a
     * 32-bit tick difference: the toolchain has no compiler-rt for u64->double. */
    static uint64_t base;
    uint64_t t = p_mach_absolute_time();
    if (!base) {
        base = t;
    }
    return (int)(uint32_t)(t - base) / 24e6;
}

/* No arguments: the toolchain links no crt1, so main is entered straight from
 * dyld with argc/argv never set up (see armv6-toolchain/mkold.py). */
#define DELAY_S  45
#define RECORD_S 10

int main(void)
{
    double secs = RECORD_S;
    int delay = DELAY_S;
    AudioStreamBasicDescription fmt = {
        .mSampleRate = 44100, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4,
        .mChannelsPerFrame = 2, .mBitsPerChannel = 16,
    };
    UInt32 cat = kAudioSessionCategory_RecordAudio;
    AudioQueueRef q;
    OSStatus err;

    printf("MICTEST waiting %d s\n", delay);
    sleep(delay);
    if (!resolve()) {
        _exit(1);
    }
    p_AudioSessionInitialize(NULL, NULL, NULL, NULL);
    p_AudioSessionSetProperty(kAudioSessionProperty_AudioCategory, sizeof(cat), &cat);
    if ((err = p_AudioSessionSetActive(true))) {
        printf("MICTEST error AudioSessionSetActive %ld\n", (long)err);
    }
    if ((err = p_AudioQueueNewInput(&fmt, input_cb, NULL, p_CFRunLoopGetCurrent(),
                                  *p_kCFRunLoopCommonModes, 0, &q))) {
        printf("MICTEST error AudioQueueNewInput %ld\n", (long)err);
        _exit(1);
    }
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        p_AudioQueueAllocateBuffer(q, 8192, &b);
        p_AudioQueueEnqueueBuffer(q, b, 0, NULL);
    }
    if ((err = p_AudioQueueStart(q, NULL))) {
        printf("MICTEST error AudioQueueStart %ld\n", (long)err);
        _exit(1);
    }
    /* Let the first buffer arrive before the clock starts: the queue's start
     * latency is not the capture rate. */
    while (frames == 0) {
        p_CFRunLoopRunInMode(*p_kCFRunLoopDefaultMode, 0.01, false);
    }
    long f0 = frames, c0 = crossings;
    double t0 = now();
    while (now() - t0 < secs) {
        p_CFRunLoopRunInMode(*p_kCFRunLoopDefaultMode, 0.05, false);
    }
    double t = now() - t0;
    long f = frames - f0, c = crossings - c0;
    p_AudioQueueStop(q, true);
    printf("MICTEST frames=%ld secs=%.3f rate=%.0f freq=%.1f peak=%ld glitches=%ld\n",
           f, t, f / t, f ? c / 2.0 / (f / 44100.0) : 0, peak, glitches);
    for (int i = 0; i < 12 && i < glitches; i++) {
        printf("MICTEST glitch at frame %ld (mod 2048 = %ld)\n", gpos[i], gpos[i] % 2048);
    }
    _exit(0);           /* main must not return: there is no crt1 to return to */
}
