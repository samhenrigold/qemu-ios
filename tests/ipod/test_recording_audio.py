#!/usr/bin/env python3
"""Actual mixer bridge: bounded packets, timestamp gaps and capture lifecycle."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'contrib/macos-app/qemu-macos-extras.c').read_text()
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define g_new(t,n) ((t*)calloc(n,sizeof(t)))
#define g_free free
typedef int CaptureVoiceOut;
typedef int AudioState;
typedef int Error;
typedef int audcnotification_e;
typedef int VMChangeStateEntry;
typedef int RunState;
#define AUD_CNOTIFY_DISABLE 0
#define AUDIO_FORMAT_S16 1
struct audsettings { int freq,nchannels,fmt,endianness; };
struct audio_capture_ops { void (*notify)(void*,int);void (*capture)(void*,const void*,int);void (*destroy)(void*); };
static struct audio_capture_ops ops;
static void *capture_context;
static bool ready,fail_capture;
static int64_t now;
static void (*jobs[64])(void*);
static void *args[64];
static unsigned job_count;
static bool qemu_ios_ui_ready(void) {return ready;}
static int64_t g_get_monotonic_time(void) {return now;}
static void *qemu_get_aio_context(void) {return NULL;}
static void aio_bh_schedule_oneshot(void *ctx,void (*fn)(void*),void *arg) {assert(job_count<64);jobs[job_count]=fn;args[job_count++]=arg;}
static void flush(void) {for(unsigned i=0;i<job_count;i++)jobs[i](args[i]);job_count=0;}
static AudioState *audio_get_default_audio_state(Error **err) {return (void*)1;}
static CaptureVoiceOut *AUD_add_capture(AudioState *s,struct audsettings *as,struct audio_capture_ops *o,void *ctx) {
 assert(s && as->freq==44100 && as->nchannels==2 && as->fmt==AUDIO_FORMAT_S16 && !as->endianness);
 if(fail_capture)return NULL;
 ops=*o;capture_context=ctx;return (void*)2;
}
static void AUD_del_capture(CaptureVoiceOut *c,void *ctx) {assert(c && ctx==capture_context);ops.destroy(ctx);capture_context=NULL;}
static void error_report_err(Error *e) {}
static VMChangeStateEntry *qemu_add_vm_change_state_handler(void (*fn)(void*,bool,RunState),void *ctx) {return (void*)3;}
static void qemu_del_vm_change_state_handler(VMChangeStateEntry *e) {assert(e==(void*)3);}
'''+s[s.index('/* Guest output capture uses'):]+r'''
int main(void) {
 uint8_t input[20000],output[16384];double seconds;
 memset(input,0xa5,sizeof(input));assert(!qemu_ios_audio_capture_start());ready=true;
 uint64_t token=qemu_ios_audio_capture_start();assert(token && !capture_context);flush();
 assert(qemu_ios_audio_capture_time(token)==0);
 assert(qemu_ios_audio_capture_read(token,output,sizeof(output),&seconds)==0 && seconds==0);
 ops.capture(capture_context,input,sizeof(input));
 assert(qemu_ios_audio_capture_read(token,output,sizeof(output),&seconds)==16384 && seconds==0);
 assert(!memcmp(input,output,sizeof(output)));
 assert(qemu_ios_audio_capture_read(token,output,sizeof(output),&seconds)==3616);
 assert(fabs(seconds-16384/176400.0)<1e-8);
 assert(qemu_ios_audio_capture_read(token,output,sizeof(output),&seconds)==0);
 now=2000000;recording_audio_vm_changed(capture_context,true,0);
 ops.capture(capture_context,input,1764);
 assert(qemu_ios_audio_capture_read(token,output,sizeof(output),&seconds)==1764 && fabs(seconds-1.99)<1e-8);
 assert(qemu_ios_audio_capture_read(token,NULL,0,&seconds)==-1);
 for(int i=0;i<129;i++)ops.capture(capture_context,input,4);
 assert(qemu_ios_audio_capture_read(token,output,sizeof(output),&seconds)==-1);
 qemu_ios_audio_capture_stop(token);
 uint64_t next=qemu_ios_audio_capture_start();flush();assert(next!=token && capture_context);
 assert(qemu_ios_audio_capture_read(token,output,sizeof(output),&seconds)==-1);
 qemu_ios_audio_capture_stop(token);flush();assert(capture_context);
 ops.capture(capture_context,input,4);
 qemu_ios_audio_capture_stop(next);flush();
 assert(qemu_ios_audio_capture_read(next,output,sizeof(output),&seconds)==4);
 assert(qemu_ios_audio_capture_read(next,output,sizeof(output),&seconds)==0 && seconds<0);
 next=qemu_ios_audio_capture_start();flush();
 void *ctx=capture_context;ops.destroy(ctx);ops.destroy(ctx); /* Cleanup can visit multiple voices. */
 assert(qemu_ios_audio_capture_read(next,output,sizeof(output),&seconds)==-1);
 fail_capture=true;next=qemu_ios_audio_capture_start();flush();
 assert(qemu_ios_audio_capture_read(next,output,sizeof(output),&seconds)==-1);
 qemu_ios_audio_capture_stop(next);flush();
 puts("PASS: mixer format, packet splitting, timestamps, pause gap, overflow and start/stop/cleanup generations");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'check.c';p.write_text(code)
 subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(p),'-o',tmp+'/check'],check=True)
 subprocess.run([tmp+'/check'],check=True)
