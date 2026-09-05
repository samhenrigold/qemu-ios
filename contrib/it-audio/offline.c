/* Bounded, offline 7E18 hardware-codec reference probe. See README.md. */
#include <AudioToolbox/AudioQueue.h>
#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/AudioServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
static void done(void *p,AudioQueueRef q,AudioQueueBufferRef b) {}
static void check(const char *name,OSStatus e) {printf("%s %d\n",name,(int)e);if(e)exit(10);}
#define FN(lib,name) __typeof__(name) *p_##name=(__typeof__(name)*)dlsym(lib,#name); if(!p_##name){puts("missing " #name);exit(2);}
int main(void) {
 setbuf(stdout,NULL);
 void *cf=dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",RTLD_NOW);
 void *at=dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox",RTLD_NOW);
 if(!cf||!at){puts("dlopen failed");exit(1);}
 FN(cf,CFURLCreateFromFileSystemRepresentation);FN(cf,CFRelease);
 FN(at,AudioFileOpenURL);FN(at,AudioFileGetProperty);FN(at,AudioFileGetPropertyInfo);FN(at,AudioFileReadPackets);FN(at,AudioFileClose);
 FN(at,AudioQueuePrime);FN(at,AudioQueueNewOutput);FN(at,AudioQueueSetProperty);FN(at,AudioQueueAllocateBufferWithPacketDescriptions);FN(at,AudioQueueAllocateBuffer);FN(at,AudioQueueEnqueueBuffer);FN(at,AudioQueueSetOfflineRenderFormat);FN(at,AudioQueueStart);FN(at,AudioQueueOfflineRender);FN(at,AudioQueueDispose);
 FN(at,AudioSessionInitialize);FN(at,AudioSessionSetProperty);FN(at,AudioSessionSetActive);
 check("session init",p_AudioSessionInitialize(NULL,NULL,NULL,NULL));
 UInt32 category=kAudioSessionCategory_MediaPlayback;
 check("session category",p_AudioSessionSetProperty(kAudioSessionProperty_AudioCategory,4,&category));
 check("session active",p_AudioSessionSetActive(true));
 char path[1024];if(!getcwd(path,sizeof(path)-32))exit(3);strcat(path,"/input.m4a");
 CFURLRef url=p_CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8*)path,strlen(path),false);AudioFileID file=NULL;
 check("open",p_AudioFileOpenURL(url,kAudioFileReadPermission,0,&file));p_CFRelease(url);
 AudioStreamBasicDescription in={0};UInt32 size=sizeof(in);
 check("format",p_AudioFileGetProperty(file,kAudioFilePropertyDataFormat,&size,&in));
 printf("input %08x rate %.0f channels %u fpp %u\n",(unsigned)in.mFormatID,in.mSampleRate,(unsigned)in.mChannelsPerFrame,(unsigned)in.mFramesPerPacket);
 UInt64 packets=0;size=sizeof(packets);check("packet count",p_AudioFileGetProperty(file,kAudioFilePropertyAudioDataPacketCount,&size,&packets));
 UInt32 max=0;size=4;check("packet size",p_AudioFileGetProperty(file,kAudioFilePropertyPacketSizeUpperBound,&size,&max));
 if(!packets||packets>4096||!max||max>65536||packets*max>16*1024*1024)exit(4);
 if(!(in.mSampleRate>=8000&&in.mSampleRate<=192000)||!in.mFramesPerPacket)exit(4);
 double frames=(double)(UInt32)packets*in.mFramesPerPacket*44100/in.mSampleRate;
 if(frames>30*44100)exit(4);
 UInt32 limit=(UInt32)frames+8192;
 /* Match the HE format exposed by ExtAudioFile for an implicit-SBR file.
  * Otherwise retain AudioFile's LC core description, exercising that path. */
 if(getenv("IT_AUDIO_HE")) {
  if(in.mFormatID!='aac '||in.mFramesPerPacket!=1024||in.mSampleRate>48000)exit(4);
  in.mFormatID='aach';in.mSampleRate*=2;in.mFramesPerPacket*=2;
 }
 AudioQueueRef queue=NULL;check("new queue",p_AudioQueueNewOutput(&in,done,NULL,NULL,NULL,0,&queue));
 UInt32 policy=kAudioQueueHardwareCodecPolicy_UseHardwareOnly;check("hardware only",p_AudioQueueSetProperty(queue,kAudioQueueProperty_HardwareCodecPolicy,&policy,4));
 UInt32 decodeFrames=limit;
 check("decode buffer size",p_AudioQueueSetProperty(queue,kAudioQueueProperty_DecodeBufferSizeFrames,&decodeFrames,4));
 size=0;
 if(!p_AudioFileGetPropertyInfo(file,kAudioFilePropertyMagicCookieData,&size,NULL)&&size) {
  if(size>65536)exit(5);void *cookie=malloc(size);if(!cookie)exit(6);
  check("cookie read",p_AudioFileGetProperty(file,kAudioFilePropertyMagicCookieData,&size,cookie));
  check("cookie set",p_AudioQueueSetProperty(queue,kAudioQueueProperty_MagicCookie,cookie,size));free(cookie);
 }
 AudioStreamBasicDescription out={44100,kAudioFormatLinearPCM,kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,4,1,4,2,16,0};
 check("offline format",p_AudioQueueSetOfflineRenderFormat(queue,&out,NULL));
 AudioQueueBufferRef input=NULL,output=NULL;
 check("input buffer",p_AudioQueueAllocateBufferWithPacketDescriptions(queue,(UInt32)(packets*max),(UInt32)packets,&input));
 UInt32 count=(UInt32)packets,bytes=input->mAudioDataBytesCapacity;
 check("read packets",p_AudioFileReadPackets(file,false,&bytes,input->mPacketDescriptions,0,&count,input->mAudioData));
 if(count!=packets||!bytes||bytes>input->mAudioDataBytesCapacity)exit(4);
 input->mAudioDataByteSize=bytes;input->mPacketDescriptionCount=count;
 check("enqueue",p_AudioQueueEnqueueBuffer(queue,input,count,input->mPacketDescriptions));
 check("output buffer",p_AudioQueueAllocateBuffer(queue,16384,&output));
 UInt32 prepared=0; check("prime",p_AudioQueuePrime(queue,limit,&prepared)); printf("prepared %u frames\n",(unsigned)prepared);
 check("start offline",p_AudioQueueStart(queue,NULL));
 FILE *f=fopen("output.pcm","wb");if(!f)exit(7);
 AudioTimeStamp time={0};time.mFlags=kAudioTimeStampSampleTimeValid;
 UInt32 total=0;
 for(;total<limit;total+=4096) {
  OSStatus e=p_AudioQueueOfflineRender(queue,&time,output,4096);
  if(e)check("render",e);
  if(output->mAudioDataByteSize>16384)exit(8);
  if(fwrite(output->mAudioData,1,output->mAudioDataByteSize,f)!=output->mAudioDataByteSize)exit(9);
  time.mSampleTime+=4096;
 }
 if(fclose(f))exit(9);
 p_AudioQueueDispose(queue,true);p_AudioFileClose(file);p_AudioSessionSetActive(false);
 printf("rendered %u frames\n",(unsigned)total);exit(0);
}
