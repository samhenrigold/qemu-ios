/* Host unit gate for queue/file ownership and failure propagation, not audio fidelity.
 * Minimal public API-shaped doubles isolate the maintained callback implementation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
typedef int OSStatus;
typedef unsigned UInt32;
typedef void *AudioQueueRef;
typedef void *CFRunLoopRef;
typedef struct { void *mAudioData; UInt32 mAudioDataBytesCapacity, mAudioDataByteSize; } Buffer;
typedef Buffer *AudioQueueBufferRef;
typedef struct { double rate; unsigned format, flags, packetBytes, packetFrames, frameBytes, channels, bits, reserved; } AudioStreamBasicDescription;
#define kAudioFormatLinearPCM 1
#define kLinearPCMFormatFlagIsSignedInteger 2
#define kLinearPCMFormatFlagIsPacked 4
#define kAudioQueueParam_Volume 1
#define RTLD_NOW 2
extern OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *,void (*)(void *,AudioQueueRef,AudioQueueBufferRef),void *,CFRunLoopRef,void *,unsigned,AudioQueueRef *);
extern OSStatus AudioQueueAllocateBuffer(AudioQueueRef,unsigned,AudioQueueBufferRef *);
extern OSStatus AudioQueueEnqueueBuffer(AudioQueueRef,AudioQueueBufferRef,unsigned,void *);
extern OSStatus AudioQueueStart(AudioQueueRef,void *);
extern OSStatus AudioQueueStop(AudioQueueRef,bool);
extern OSStatus AudioQueueDispose(AudioQueueRef,bool);
extern OSStatus AudioQueuePause(AudioQueueRef);
extern OSStatus AudioQueueSetParameter(AudioQueueRef,unsigned,float);
static unsigned running_reports;
static void report(const char *format,...) { if (!strncmp(format,"RUNNING",7)) ++running_reports; }
static void *dlopen(const char *name,int flags) { (void)name;(void)flags;return (void *)1; }
static void *dlsym(void *handle,const char *name);
static void *m0(void *object,void *selector) { (void)object;(void)selector;return 0; }
static void *C(const char *name) { (void)name;return 0; }
static void *S(const char *name) { (void)name;return 0; }
static char test_bundle[128];
static const char *utf8(void *object) { (void)object;return test_bundle; }
#include "pcm_audio.h"
static int enqueue_status,dispose_status,stop_calls,dispose_calls,enqueue_calls;
static OSStatus enqueue(AudioQueueRef queue,AudioQueueBufferRef buffer,unsigned count,void *desc)
{ (void)queue;(void)buffer;(void)count;(void)desc;++enqueue_calls;return enqueue_status; }
static OSStatus stop(AudioQueueRef queue,bool immediate) { (void)queue;assert(immediate);++stop_calls;return 0; }
static OSStatus dispose(AudioQueueRef queue,bool immediate) { (void)queue;assert(immediate);++dispose_calls;return dispose_status; }
static unsigned allocation;
static OSStatus new_queue(const AudioStreamBasicDescription *format,void (*callback)(void *,AudioQueueRef,AudioQueueBufferRef),void *user,CFRunLoopRef loop,void *mode,unsigned flags,AudioQueueRef *queue)
{ (void)callback;(void)user;(void)mode;(void)flags;assert(format->rate==44100 && format->channels==2 && format->frameBytes==4 && loop);*queue=(void *)1;allocation=0;return 0; }
static OSStatus allocate(AudioQueueRef queue,unsigned size,AudioQueueBufferRef *out)
{ (void)queue;static unsigned char bytes[2][32768];static Buffer buffers[2];assert(allocation<2);buffers[allocation]=(Buffer){bytes[allocation],size,0};*out=&buffers[allocation++];return 0; }
static int start_calls;
static OSStatus start(AudioQueueRef queue,void *time) { (void)queue;(void)time;++start_calls;return 0; }
static OSStatus pause_queue(AudioQueueRef queue) { (void)queue;return 0; }
static OSStatus parameter(AudioQueueRef queue,unsigned key,float value) { (void)queue;(void)key;(void)value;return 0; }
static CFRunLoopRef runloop(void) { return (void *)2; }
static void *dlsym(void *handle,const char *name)
{
    (void)handle;
    if (!strcmp(name,"AudioQueueNewOutput")) return new_queue;
    if (!strcmp(name,"AudioQueueAllocateBuffer")) return allocate;
    if (!strcmp(name,"AudioQueueEnqueueBuffer")) return enqueue;
    if (!strcmp(name,"AudioQueueStart")) return start;
    if (!strcmp(name,"AudioQueueStop")) return stop;
    if (!strcmp(name,"AudioQueueDispose")) return dispose;
    if (!strcmp(name,"AudioQueuePause")) return pause_queue;
    if (!strcmp(name,"AudioQueueSetParameter")) return parameter;
    if (!strcmp(name,"CFRunLoopGetCurrent")) return runloop;
    return 0;
}
static void input(unsigned bytes)
{
    pcm_file=tmpfile();assert(pcm_file);
    for (unsigned i=0;i<bytes;++i) assert(fputc(i,pcm_file)!=EOF);
    rewind(pcm_file);pcm_left=bytes;pcm_failed=0;pcm_queue=(void *)1;
}
int main(void)
{
    aq_enqueue=enqueue;aq_stop=stop;aq_dispose=dispose;
    unsigned char data[8];Buffer buffer={data,8,0};
    input(12);pcm_fill(0,pcm_queue,&buffer);
    assert(!pcm_failed && pcm_left==4 && buffer.mAudioDataByteSize==8 && enqueue_calls==1);
    pcm_fill(0,pcm_queue,&buffer);assert(!pcm_failed && pcm_left==0 && buffer.mAudioDataByteSize==4);
    pcm_fill(0,pcm_queue,&buffer);assert(enqueue_calls==2);assert(stop_pcm());
    input(12);enqueue_status=-7;pcm_fill(0,pcm_queue,&buffer);
    assert(pcm_failed && pcm_left==12); /* Failed enqueue cannot consume/announce success. */
    pcm_poll_failure();assert(!pcm_queue && !pcm_file && !pcm_failed);enqueue_status=0;
    input(4);pcm_left=8;pcm_fill(0,pcm_queue,&buffer);
    assert(pcm_failed && enqueue_calls==3);pcm_poll_failure();assert(!pcm_queue && !pcm_file);
    input(8);buffer.mAudioDataBytesCapacity=6;pcm_fill(0,pcm_queue,&buffer);
    assert(pcm_failed);pcm_poll_failure();assert(!pcm_queue && !pcm_file);
    input(8);dispose_status=-9;assert(!stop_pcm());
    assert(pcm_queue && pcm_file && pcm_failed); /* Failed dispose keeps its resources owned. */
    dispose_status=0;pcm_poll_failure();assert(!pcm_queue && !pcm_file && !pcm_failed);
    int stops=stop_calls,disposes=dispose_calls;assert(stop_pcm());
    assert(stop_calls==stops && dispose_calls==disposes); /* Repeated cleanup cannot reuse handles. */
    strcpy(test_bundle,"/private/tmp/ltm-pcm-test-XXXXXX");assert(mkdtemp(test_bundle));
    char path[160];snprintf(path,sizeof(path),"%s/stereo.wav",test_bundle);
    FILE *wav=fopen(path,"wb");assert(wav);
    const unsigned char header[]={ 'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,2,0,0x44,0xac,0,0,0x10,0xb1,2,0,4,0,16,0,'d','a','t','a',8,0,0,0,1,0,2,0,3,0,4,0 };
    assert(fwrite(header,1,sizeof(header),wav)==sizeof(header));assert(!fclose(wav));
    enqueue_status=-7;play_audio("stereo.wav");
    assert(!running_reports && !start_calls && !pcm_queue && !pcm_file); /* Preload failure never starts. */
    enqueue_status=0;play_audio("stereo.wav");assert(running_reports==1 && start_calls==1);
    assert(stop_pcm());assert(!remove(path));assert(!rmdir(test_bundle));
    puts("PASS PCM fill/exhaustion, enqueue/read/alignment failure, main-loop cleanup, failed-dispose retention and idempotent release");
}
