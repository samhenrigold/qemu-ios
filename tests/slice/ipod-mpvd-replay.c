/* Native MPEG-4 snapshot replay, bounded migration state, and DMA isolation.
 *
 * Requires macOS VideoToolbox and ffmpeg on PATH: main() makes its own I/P movie in the scratch directory; no
 * guest runs. The check builds as Objective-C for AVFoundation.
 *
 * SLICE hw/arm/ipod_video.c range struct IPodVideoDecoder | #endif
 * SLICE include/hw/arm/ipod_touch_mpvd.h typedef IPodTouchMPVDState
 * SLICE hw/arm/ipod_touch_mpvd.c range /* Bound retained | #ifdef __APPLE__\ntypedef
 * SLICE hw/arm/ipod_touch_mpvd.c range typedef struct MPVDDecoder | \n#else\nstatic void mpvd_decoder_close
 * SLICE:vm hw/arm/ipod_touch_mpvd.c range static int mpvd_put_packets | static const VMStateDescription vmstate_ipod_touch_mpvd
 * PKG glib-2.0
 * CFLAGS -x objective-c -fobjc-arc -Wno-deprecated-declarations -framework AVFoundation -framework VideoToolbox
 * CFLAGS -framework Foundation -framework CoreMedia -framework CoreVideo
 */
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <glib.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#define MPVD_REG_SIZE 0x70000
#define MEMTXATTRS_UNSPECIFIED 0
#define warn_report(...) ((void)0)
#define error_report(...) ((void)0)
#define qemu_set_irq(a,b) ((void)0)
static unsigned writes;
static int address_space_memory;
static uint8_t ram[4 * 1024 * 1024];
static int address_space_read(void *s, uint32_t addr, int attr, void *data, size_t size) {
 assert(addr >= 0x08000000 && addr - 0x08000000 + size <= sizeof(ram));
 memcpy(data, ram + (addr - 0x08000000), size); return 0;
}
static int address_space_write(void *s, uint32_t addr, int attr, const void *data, size_t size) {
 assert(addr >= 0x08000000 && addr - 0x08000000 + size <= sizeof(ram));
 memcpy(ram + (addr - 0x08000000), data, size); writes++; return 0;
}
typedef struct IPodVideoDecoder IPodVideoDecoder;
typedef int SysBusDevice, MemoryRegion, qemu_irq;
#include "slice.h"
typedef struct { GByteArray *data; unsigned pos; int error; } QEMUFile;
typedef void VMStateField;
typedef void JSONWriter;
typedef struct {
 const char *name;
 int (*get)(QEMUFile*,void*,size_t,const VMStateField*);
 int (*put)(QEMUFile*,void*,size_t,const VMStateField*,JSONWriter*);
} VMStateInfo;
static int qemu_file_get_error(QEMUFile *f) { return f->error; }
static void qemu_put_buffer(QEMUFile *f,const uint8_t *data,size_t n) {g_byte_array_append(f->data,data,n);}
static void qemu_put_be32(QEMUFile *f,uint32_t n) {uint32_t be=GUINT32_TO_BE(n);qemu_put_buffer(f,(void*)&be,4);}
static int qemu_get_buffer(QEMUFile *f,uint8_t *data,size_t n) {
 if(n>f->data->len-f->pos){f->error=-EIO;return 0;} memcpy(data,f->data->data+f->pos,n);f->pos+=n;return n;
}
static uint32_t qemu_get_be32(QEMUFile *f) {uint32_t n=0;qemu_get_buffer(f,(void*)&n,4);return GUINT32_FROM_BE(n);}
#include "vm.h"
static void clone_state(IPodTouchMPVDState *from, IPodTouchMPVDState *to) {
 assert(!mpvd_pre_save(from));
 assert(!mpvd_pre_load(to));
 memcpy(to->regs,from->regs,sizeof(to->regs));
 to->decode_enabled=to->saved_decode_enabled=from->decode_enabled;
 to->replay_width=from->replay_width;to->replay_height=from->replay_height;to->replay_time_bits=from->replay_time_bits;
 QEMUFile f={.data=g_byte_array_new()};
 assert(!mpvd_put_packets(&f,&from->packets,0,NULL,NULL));
 assert(!mpvd_get_packets(&f,&to->packets,0,NULL));
 assert(!mpvd_post_load(to,3));
 g_byte_array_unref(f.data);
}
static bool submit(IPodTouchMPVDState *s, GBytes *packet) {
 size_t length;const uint8_t *data=g_bytes_get_data(packet,&length);
 memset(ram,0,sizeof(ram));memcpy(ram+4096,data,length);
 s->decode_enabled=true;s->regs[0x6006c/4]=(4<<16)|3;
 s->regs[0x1009c/4]=5;s->regs[0x10010/4]=data[4]>>6;
 s->regs[0x60018/4]=0x08001008;s->regs[0x6001c/4]=0x08001000+length;
 s->regs[0x6003c/4]=0x08200000;s->regs[0x60044/4]=0x08210000;
 return mpvd_decode(s);
}
int main(void){@autoreleasepool{
 assert(!system("ffmpeg -v error -y -f lavfi -i testsrc2=size=64x48:rate=30 -frames:v 8 -c:v mpeg4 -bf 0 -g 30 "
                "-q:v 3 -enc_time_base 1:30 fixture.mp4"));
 AVURLAsset *asset=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@"fixture.mp4"] options:nil];
 AVAssetTrack *track=[[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];assert(track);
 NSError *error=nil;AVAssetReader *reader=[[AVAssetReader alloc]initWithAsset:asset error:&error];
 AVAssetReaderTrackOutput *output=[AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
 [reader addOutput:output];assert([reader startReading]);
 GPtrArray *packets=g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
 CMSampleBufferRef sample;
 while((sample=[output copyNextSampleBuffer])){
  CMBlockBufferRef block=CMSampleBufferGetDataBuffer(sample);size_t n=CMBlockBufferGetDataLength(block);
  if(!block || !n){CFRelease(sample);continue;}
  uint8_t *data=g_malloc(n);assert(!CMBlockBufferCopyDataBytes(block,0,n,data));
  size_t start=0;while(start+5<n && memcmp(data+start,"\0\0\1\xb6",4))start++;
  assert(start+5<n);g_ptr_array_add(packets,g_bytes_new(data+start,n-start));g_free(data);CFRelease(sample);
 }
 assert(packets->len>=6);
 IPodTouchMPVDState a={0},b={0};
 assert(submit(&a,packets->pdata[0]));assert(submit(&a,packets->pdata[1]));
 assert(a.packets->len==2);
 clone_state(&a,&b);assert(!b.decoder && b.packets->len==2);
 for(unsigned i=2;i<packets->len;i++){
  writes=0;assert(submit(&a,packets->pdata[i]));assert(writes==72);
  uint8_t pixels[4608];memcpy(pixels,ram+0x200000,3072);memcpy(pixels+3072,ram+0x210000,1536);
  writes=0;assert(submit(&b,packets->pdata[i]));assert(writes==72); /* replay adds zero writes */
  assert(!memcmp(pixels,ram+0x200000,3072));assert(!memcmp(pixels+3072,ram+0x210000,1536));
 }
 /* An I-picture bounds history again. */
 assert(submit(&a,packets->pdata[0]));assert(a.packets->len==1);
 size_t n;const uint8_t *p=g_bytes_get_data(packets->pdata[1],&n);
 for(unsigned i=1;i<MPVD_HISTORY_PACKETS;i++)mpvd_history_append(&a,p,n,64,48,5);
 assert(!mpvd_pre_save(&a));mpvd_history_append(&a,p,n,64,48,5);
 assert(a.history_unavailable && !a.packets && mpvd_pre_save(&a)==-ENOTSUP);
 assert(submit(&a,packets->pdata[1]));assert(mpvd_pre_save(&a)==-ENOTSUP);
 assert(submit(&a,packets->pdata[0]));assert(!mpvd_pre_save(&a));
 /* Byte limit independent of count, and reset releases all retained state. */
 uint8_t *large=g_malloc0(4*1024*1024);memcpy(large,"\0\0\1\xb6",4);
 mpvd_history_append(&a,large,4*1024*1024,64,48,5);large[4]=0x40;
 for(unsigned i=0;i<3;i++)mpvd_history_append(&a,large,4*1024*1024,64,48,5);
 assert(a.packet_bytes==MPVD_HISTORY_BYTES && !mpvd_pre_save(&a));
 mpvd_history_append(&a,large,4*1024*1024,64,48,5);g_free(large);
 assert(a.history_unavailable);mpvd_decoder_close(&a);mpvd_history_clear(&a);
 assert(!a.decoder && !a.packets && !a.packet_bytes && !mpvd_pre_save(&a));
 /* Reject malformed serialized count, packet length, order, truncated bytes. */
 QEMUFile f={.data=g_byte_array_new()};qemu_put_be32(&f,MPVD_HISTORY_PACKETS+1);
 assert(mpvd_get_packets(&f,&a.packets,0,NULL)==-EINVAL);
 g_byte_array_set_size(f.data,0);f.pos=f.error=0;qemu_put_be32(&f,1);qemu_put_be32(&f,MPVD_PACKET_BYTES+1);
 assert(mpvd_get_packets(&f,&a.packets,0,NULL)==-EINVAL);
 g_byte_array_set_size(f.data,0);f.pos=f.error=0;qemu_put_be32(&f,1);qemu_put_be32(&f,n);qemu_put_buffer(&f,p,n);
 assert(mpvd_get_packets(&f,&a.packets,0,NULL)==-EINVAL); /* P cannot begin history */
 g_byte_array_set_size(f.data,0);f.pos=f.error=0;qemu_put_be32(&f,1);qemu_put_be32(&f,20);
 assert(mpvd_get_packets(&f,&a.packets,0,NULL)==-EIO);
 clone_state(&b,&a);a.replay_width=65;assert(mpvd_post_load(&a,3)==-EINVAL);
 a.saved_decode_enabled=false;assert(mpvd_post_load(&a,2)==-EINVAL);
 mpvd_decoder_close(&a);mpvd_history_clear(&a);mpvd_decoder_close(&b);mpvd_history_clear(&b);
 g_ptr_array_unref(packets);g_byte_array_unref(f.data);
 puts("PASS: native MPVD I/P restored pixels, zero replay DMA, bounded history recovery, malformed state");
}}
