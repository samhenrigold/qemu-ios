/* Explicit SDK 2.0 fixture backend. Included after report/ObjC helpers.
 * Callbacks run on UIKit's current run loop; queue/file/UI share that owner. */
static AudioQueueRef pcm_queue;
static FILE *pcm_file;
static UInt32 pcm_left;
static int pcm_paused, pcm_failed;
static __typeof__(&AudioQueueNewOutput) aq_new;
static __typeof__(&AudioQueueAllocateBuffer) aq_alloc;
static __typeof__(&AudioQueueEnqueueBuffer) aq_enqueue;
static __typeof__(&AudioQueueStart) aq_start;
static __typeof__(&AudioQueueStop) aq_stop;
static __typeof__(&AudioQueueDispose) aq_dispose;
static __typeof__(&AudioQueuePause) aq_pause;
static __typeof__(&AudioQueueSetParameter) aq_parameter;

static int stop_pcm(void)
{
    if (pcm_queue) {
        OSStatus stop = aq_stop(pcm_queue, true);
        OSStatus dispose = aq_dispose(pcm_queue, true);
        if (stop || dispose) report("FAIL PCM teardown: stop=%d dispose=%d", (int)stop, (int)dispose);
        if (dispose) { pcm_failed = 1; return 0; } /* Retain ownership; retry only on this run loop. */
        pcm_queue = 0;
    }
    if (pcm_file) { if (fclose(pcm_file)) report("FAIL PCM close"); pcm_file = 0; }
    pcm_left = 0; pcm_paused = 0; return 1;
}

static unsigned pcm_le32(const unsigned char *p)
{ return p[0] | (unsigned)p[1]<<8 | (unsigned)p[2]<<16 | (unsigned)p[3]<<24; }

static void pcm_fill(void *user, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    if (!pcm_left || pcm_failed) return;
    UInt32 count = pcm_left < buffer->mAudioDataBytesCapacity ? pcm_left : buffer->mAudioDataBytesCapacity;
    if (!count || count % 4 || fread(buffer->mAudioData, 1, count, pcm_file) != count) {
        pcm_failed = 1; report("FAIL PCM read/buffer alignment"); return;
    }
    buffer->mAudioDataByteSize = count;
    OSStatus status = aq_enqueue(queue, buffer, 0, 0);
    if (status) { pcm_failed = 1; report("FAIL PCM enqueue: %d", (int)status); return; }
    pcm_left -= count;
}

static void pcm_poll_failure(void)
{
    /* Dispose outside the callback, on the same main run loop. */
    if (pcm_failed && stop_pcm()) pcm_failed = 0;
}

static void pcm_control(int tag)
{
    if (!pcm_queue || pcm_failed) return;
    OSStatus status;
    if (tag == 15) {
        status = pcm_paused ? aq_start(pcm_queue, 0) : aq_pause(pcm_queue);
        if (!status) pcm_paused = !pcm_paused;
    } else status = aq_parameter(pcm_queue, kAudioQueueParam_Volume, tag == 16 ? 0.1f : 0.8f);
    if (status) { pcm_failed = 1; report("FAIL PCM control: %d", (int)status); }
}

static void play_audio(const char *name)
{
    if (strcmp(name, "stereo.wav")) {
        report("UNSUPPORTED legacy fixture: compressed audio needs separate packet coverage"); return;
    }
    if (!stop_pcm()) return;
    pcm_failed = 0;
    /* Framework handles stay open while resolved function pointers are retained. */
    static void *toolbox, *cf;
    if (!toolbox) toolbox = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    if (!toolbox) { report("FAIL AudioToolbox unavailable"); return; }
#define AQ_RESOLVE(var, symbol) var = dlsym(toolbox, symbol); if (!var) { report("FAIL missing %s", symbol); return; }
    AQ_RESOLVE(aq_new, "AudioQueueNewOutput") AQ_RESOLVE(aq_alloc, "AudioQueueAllocateBuffer")
    AQ_RESOLVE(aq_enqueue, "AudioQueueEnqueueBuffer") AQ_RESOLVE(aq_start, "AudioQueueStart")
    AQ_RESOLVE(aq_stop, "AudioQueueStop") AQ_RESOLVE(aq_dispose, "AudioQueueDispose")
    AQ_RESOLVE(aq_pause, "AudioQueuePause") AQ_RESOLVE(aq_parameter, "AudioQueueSetParameter")
#undef AQ_RESOLVE
    if (!cf) cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    CFRunLoopRef (*current_loop)(void) = cf ? dlsym(cf, "CFRunLoopGetCurrent") : 0;
    if (!current_loop) { report("FAIL missing current run loop"); return; }
    char path[1200];
    const char *bundle = utf8(m0(m0(C("NSBundle"), S("mainBundle")), S("bundlePath")));
    if (!bundle || snprintf(path, sizeof(path), "%s/%s", bundle, name) >= (int)sizeof(path)) {
        report("FAIL PCM resource path"); return;
    }
    pcm_file = fopen(path, "rb"); unsigned char chunk[16];
    if (!pcm_file || fread(chunk,1,12,pcm_file)!=12 || memcmp(chunk,"RIFF",4) || memcmp(chunk+8,"WAVE",4)) goto invalid;
    int format_seen = 0;
    while (fread(chunk,1,8,pcm_file)==8) {
        unsigned length = pcm_le32(chunk+4);
        if (length > 16*1024*1024) goto invalid; /* Bundled six-second fixture, bounded chunks. */
        if (!memcmp(chunk,"fmt ",4)) {
            if (length<16 || fread(chunk,1,16,pcm_file)!=16 || chunk[0]!=1 || chunk[1] ||
                chunk[2]!=2 || chunk[3] || pcm_le32(chunk+4)!=44100 || pcm_le32(chunk+8)!=176400 ||
                chunk[12]!=4 || chunk[13] || chunk[14]!=16 || chunk[15]) goto invalid;
            format_seen = 1;
            if (fseek(pcm_file,length-16+(length&1),SEEK_CUR)) goto invalid;
        } else if (!memcmp(chunk,"data",4)) {
            if (!format_seen || !length || length%4) goto invalid;
            pcm_left = length; break;
        } else if (fseek(pcm_file,length+(length&1),SEEK_CUR)) goto invalid;
    }
    if (!pcm_left) goto invalid;
    AudioStreamBasicDescription format = {44100,kAudioFormatLinearPCM,
        kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked,4,1,4,2,16,0};
    OSStatus status = aq_new(&format,pcm_fill,0,current_loop(),0,0,&pcm_queue);
    if (!status) for (unsigned i=0;i<2;++i) {
        AudioQueueBufferRef buffer;
        status = aq_alloc(pcm_queue,32768,&buffer);
        if (status) break;
        pcm_fill(0,pcm_queue,buffer);
        if (pcm_failed) { status = -1; break; }
    }
    if (!status) status = aq_start(pcm_queue,0);
    if (status) { report("FAIL PCM queue/start: %d",(int)status); stop_pcm(); }
    else report("RUNNING audio stereo.wav (AudioQueue, stereo 16-bit 44100 Hz; host waveform gate required)");
    return;
invalid:
    report("FAIL invalid stereo.wav PCM fixture"); stop_pcm();
}
