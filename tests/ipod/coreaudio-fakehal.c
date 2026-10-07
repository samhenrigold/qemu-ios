/*
 * A fake CoreAudio output device, interposed into qemu-system-arm with
 * DYLD_INSERT_LIBRARIES, so audio/coreaudio.m can be tested against output
 * devices this Mac does not have (or must not be touched) without a sound
 * reaching the speakers. tests/ipod/test_coreaudio_rate.py drives it.
 *
 * The device runs one fixed nominal rate, like AirPods (48 kHz, or 16/24 kHz
 * with the mic open): setting its stream format succeeds but keeps that rate.
 * Its IOProc is called from our own thread on the device's clock (frameCount
 * frames every frameCount/rate seconds) and every buffer it is handed is
 * appended to FAKEHAL_OUT (f32 interleaved, FAKEHAL_CH channels) -- the samples
 * the listener would hear, at the rate they would hear them.
 *
 *   FAKEHAL_PLAN = RATE[,RATE2@SECS[:dev]]
 *     RATE2@SECS      SECS after the first AudioDeviceStart the device's nominal
 *                     rate changes (AirPods switching to HFP): its
 *                     NominalSampleRate listeners fire.
 *     RATE2@SECS:dev  instead a second device at RATE2 becomes the default
 *                     output (AirPods connecting): DefaultOutputDevice
 *                     listeners on the system object fire.
 *   FAKEHAL_OUT  = path; FAKEHAL_OUT.json gets {"segments": [[frame, rate]...]}
 *                  (the recording's rate from each frame on) at exit.
 *   FAKEHAL_CH   = device channel count (default 2)
 *
 * Every other object and selector is forwarded to the real HAL.
 */
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>

#define DEV1 0x7a11001
#define DEV2 0x7a11002
#define MAXL 16

typedef struct {
    AudioObjectID obj;
    AudioObjectPropertySelector sel;
    AudioObjectPropertyListenerProc proc;
    void *data;
} Listener;

typedef struct {
    AudioObjectID id;
    Float64 rate;
    UInt32 frames;              /* buffer frame size */
    AudioDeviceIOProc proc;
    void *client;
    int running, have_proc;
    pthread_t thr;
} Dev;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static Dev devs[2] = { { DEV1, 48000, 512 }, { DEV2, 48000, 512 } };
static AudioObjectID def_dev = DEV1;
static Listener ls[MAXL];
static int nls;
static UInt32 nch = 2;
static float *rec;
static size_t rec_n, rec_cap;
static struct { size_t frame; double rate; } seg[8];
static int nseg;
static double sw_rate, sw_secs;
static int sw_dev, started;

static Dev *dev(AudioObjectID id)
{
    return id == DEV1 ? &devs[0] : id == DEV2 ? &devs[1] : NULL;
}

static void mark_segment(double rate)
{
    if (nseg < 8 && (!nseg || seg[nseg - 1].rate != rate)) {
        seg[nseg].frame = rec_n / nch;
        seg[nseg++].rate = rate;
    }
}

static void *io_thread(void *p)
{
    Dev *d = p;
    mach_timebase_info_data_t tb;
    uint64_t next = mach_absolute_time();
    float *buf = NULL;
    size_t cap = 0;

    mach_timebase_info(&tb);
    for (;;) {
        pthread_mutex_lock(&mu);
        if (!d->running) {
            pthread_mutex_unlock(&mu);
            break;
        }
        UInt32 n = d->frames;
        double rate = d->rate;
        AudioDeviceIOProc proc = d->proc;
        void *client = d->client;
        pthread_mutex_unlock(&mu);

        if (cap < (size_t)n * nch) {
            cap = (size_t)n * nch;
            buf = realloc(buf, cap * sizeof(float));
        }
        memset(buf, 0x7f, (size_t)n * nch * sizeof(float));   /* garbage, as a real buffer may hold */
        AudioBufferList abl = { 1, { { nch, n * nch * sizeof(float), buf } } };
        AudioTimeStamp ts = { 0 };
        proc(d->id, &ts, &abl, &ts, &abl, &ts, client);

        pthread_mutex_lock(&mu);
        if (d->id == def_dev && d->running) {
            mark_segment(rate);
            if (rec_n + (size_t)n * nch <= rec_cap) {
                memcpy(rec + rec_n, buf, (size_t)n * nch * sizeof(float));
                rec_n += (size_t)n * nch;
            }
        }
        pthread_mutex_unlock(&mu);

        next += (uint64_t)(n * 1e9 / rate * tb.denom / tb.numer);
        mach_wait_until(next);
    }
    free(buf);
    return NULL;
}

static void fire(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    Listener copy[MAXL];
    int n = 0;
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };

    pthread_mutex_lock(&mu);
    for (int i = 0; i < nls; i++) {
        if (ls[i].obj == obj && ls[i].sel == sel) {
            copy[n++] = ls[i];
        }
    }
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < n; i++) {
        copy[i].proc(obj, 1, &a, copy[i].data);
    }
}

static void *switch_thread(void *p)
{
    usleep((useconds_t)(sw_secs * 1e6));
    if (sw_dev) {
        pthread_mutex_lock(&mu);
        devs[1].rate = sw_rate;
        def_dev = DEV2;
        pthread_mutex_unlock(&mu);
        fire(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice);
    } else {
        pthread_mutex_lock(&mu);
        devs[0].rate = sw_rate;
        pthread_mutex_unlock(&mu);
        fire(DEV1, kAudioDevicePropertyNominalSampleRate);
    }
    return NULL;
}

static void dump(void)
{
    const char *path = getenv("FAKEHAL_OUT");
    char *j;
    FILE *f;

    if (!path) {
        return;
    }
    pthread_mutex_lock(&mu);
    f = fopen(path, "wb");
    if (f) {
        fwrite(rec, sizeof(float), rec_n, f);
        fclose(f);
    }
    asprintf(&j, "%s.json", path);
    f = fopen(j, "w");
    if (f) {
        fprintf(f, "{\"channels\": %u, \"segments\": [", (unsigned)nch);
        for (int i = 0; i < nseg; i++) {
            fprintf(f, "%s[%zu, %.0f]", i ? ", " : "", seg[i].frame, seg[i].rate);
        }
        fprintf(f, "]}\n");
        fclose(f);
    }
    pthread_mutex_unlock(&mu);
}

__attribute__((constructor)) static void init(void)
{
    const char *plan = getenv("FAKEHAL_PLAN"), *ch = getenv("FAKEHAL_CH");
    const char *c;

    if (!plan) {
        return;
    }
    devs[0].rate = devs[1].rate = atof(plan);
    if ((c = strchr(plan, ','))) {
        sw_rate = atof(c + 1);
        sw_secs = strchr(c, '@') ? atof(strchr(c, '@') + 1) : 1.0;
        sw_dev = strstr(c, ":dev") != NULL;
    }
    nch = ch ? (UInt32)atoi(ch) : 2;
    rec_cap = (size_t)60 * 48000 * nch;
    rec = calloc(rec_cap, sizeof(float));
    atexit(dump);
}

/* --- the interposed HAL calls --- */

static OSStatus my_Get(AudioObjectID obj, const AudioObjectPropertyAddress *a,
                       UInt32 qs, const void *q, UInt32 *size, void *out)
{
    Dev *d = dev(obj);

    if (!rec) {
        return AudioObjectGetPropertyData(obj, a, qs, q, size, out);
    }
    if (obj == kAudioObjectSystemObject &&
        a->mSelector == kAudioHardwarePropertyDefaultOutputDevice) {
        pthread_mutex_lock(&mu);
        *(AudioDeviceID *)out = def_dev;
        pthread_mutex_unlock(&mu);
        return 0;
    }
    if (!d) {
        return AudioObjectGetPropertyData(obj, a, qs, q, size, out);
    }
    pthread_mutex_lock(&mu);
    OSStatus st = 0;
    switch (a->mSelector) {
    case kAudioDevicePropertyBufferFrameSizeRange:
        *(AudioValueRange *)out = (AudioValueRange){ 15, 4096 };
        break;
    case kAudioDevicePropertyBufferFrameSize:
        *(UInt32 *)out = d->frames;
        break;
    case kAudioDevicePropertyNominalSampleRate:
        *(Float64 *)out = d->rate;
        break;
    case kAudioDevicePropertyDeviceIsRunning:
        *(UInt32 *)out = d->running;
        break;
    case kAudioDevicePropertyStreamFormat:
        *(AudioStreamBasicDescription *)out = (AudioStreamBasicDescription){
            .mSampleRate = d->rate, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kLinearPCMFormatFlagIsFloat | kAudioFormatFlagIsPacked,
            .mBytesPerPacket = 4 * nch, .mFramesPerPacket = 1,
            .mBytesPerFrame = 4 * nch, .mChannelsPerFrame = nch,
            .mBitsPerChannel = 32 };
        break;
    default:
        st = kAudioHardwareUnknownPropertyError;
    }
    pthread_mutex_unlock(&mu);
    return st;
}

static OSStatus my_Set(AudioObjectID obj, const AudioObjectPropertyAddress *a,
                       UInt32 qs, const void *q, UInt32 size, const void *in)
{
    Dev *d = dev(obj);

    if (!rec || !d) {
        return AudioObjectSetPropertyData(obj, a, qs, q, size, in);
    }
    pthread_mutex_lock(&mu);
    OSStatus st = 0;
    switch (a->mSelector) {
    case kAudioDevicePropertyBufferFrameSize:
        d->frames = *(const UInt32 *)in;
        break;
    case kAudioDevicePropertyStreamFormat:
        break;              /* accepted; the device keeps its own rate */
    case kAudioDevicePropertyNominalSampleRate:
        st = *(const Float64 *)in == d->rate ? 0 : kAudioDeviceUnsupportedFormatError;
        break;
    default:
        st = kAudioHardwareUnknownPropertyError;
    }
    pthread_mutex_unlock(&mu);
    return st;
}

static OSStatus my_AddL(AudioObjectID obj, const AudioObjectPropertyAddress *a,
                        AudioObjectPropertyListenerProc proc, void *data)
{
    if (!rec || (obj != kAudioObjectSystemObject && !dev(obj))) {
        return AudioObjectAddPropertyListener(obj, a, proc, data);
    }
    pthread_mutex_lock(&mu);
    if (nls < MAXL) {
        ls[nls++] = (Listener){ obj, a->mSelector, proc, data };
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

static OSStatus my_RemoveL(AudioObjectID obj, const AudioObjectPropertyAddress *a,
                           AudioObjectPropertyListenerProc proc, void *data)
{
    if (!rec || (obj != kAudioObjectSystemObject && !dev(obj))) {
        return AudioObjectRemovePropertyListener(obj, a, proc, data);
    }
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nls; i++) {
        if (ls[i].obj == obj && ls[i].sel == a->mSelector && ls[i].proc == proc &&
            ls[i].data == data) {
            ls[i] = ls[--nls];
            break;
        }
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

static OSStatus my_Create(AudioObjectID obj, AudioDeviceIOProc proc, void *client,
                          AudioDeviceIOProcID *id)
{
    Dev *d = dev(obj);

    if (!rec || !d) {
        return AudioDeviceCreateIOProcID(obj, proc, client, id);
    }
    pthread_mutex_lock(&mu);
    d->proc = proc;
    d->client = client;
    d->have_proc = 1;
    *id = (AudioDeviceIOProcID)proc;
    pthread_mutex_unlock(&mu);
    return 0;
}

static OSStatus my_Destroy(AudioObjectID obj, AudioDeviceIOProcID id)
{
    Dev *d = dev(obj);

    if (!rec || !d) {
        return AudioDeviceDestroyIOProcID(obj, id);
    }
    pthread_mutex_lock(&mu);
    d->have_proc = 0;
    pthread_mutex_unlock(&mu);
    return 0;
}

static OSStatus my_Start(AudioObjectID obj, AudioDeviceIOProcID id)
{
    Dev *d = dev(obj);
    pthread_t t;

    if (!rec || !d) {
        return AudioDeviceStart(obj, id);
    }
    pthread_mutex_lock(&mu);
    if (!d->running && d->have_proc) {
        d->running = 1;
        pthread_create(&d->thr, NULL, io_thread, d);
    }
    if (!started++ && sw_rate) {
        pthread_create(&t, NULL, switch_thread, NULL);
        pthread_detach(t);
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

static OSStatus my_Stop(AudioObjectID obj, AudioDeviceIOProcID id)
{
    Dev *d = dev(obj);
    int join;

    if (!rec || !d) {
        return AudioDeviceStop(obj, id);
    }
    pthread_mutex_lock(&mu);
    join = d->running;
    d->running = 0;
    pthread_mutex_unlock(&mu);
    if (join && !pthread_equal(pthread_self(), d->thr)) {
        pthread_join(d->thr, NULL);
    }
    return 0;
}

#define INTERPOSE(r, o) \
    __attribute__((used)) static struct { const void *n, *o; } \
    interpose_##o __attribute__((section("__DATA,__interpose"))) = { (const void *)r, (const void *)o }
INTERPOSE(my_Get, AudioObjectGetPropertyData);
INTERPOSE(my_Set, AudioObjectSetPropertyData);
INTERPOSE(my_AddL, AudioObjectAddPropertyListener);
INTERPOSE(my_RemoveL, AudioObjectRemovePropertyListener);
INTERPOSE(my_Create, AudioDeviceCreateIOProcID);
INTERPOSE(my_Destroy, AudioDeviceDestroyIOProcID);
INTERPOSE(my_Start, AudioDeviceStart);
INTERPOSE(my_Stop, AudioDeviceStop);
