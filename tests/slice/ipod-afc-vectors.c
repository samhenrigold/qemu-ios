/* Replay saved AFC file bytes through production tcp_usb with fragmented I/O.
 *
 * This tests USB transport framing, not a guest AFC server. Real-device vectors
 * (../qemu-ios-files/real-device/afc/t_*.bin beside the tree, each equal to its back_*.bin) are optional;
 * synthetic odd-sized payloads always run.
 *
 * SLICE include/hw/arm/ipod_touch_tcp_usb.h range enum\n{ | #endif /* HW_ARM_IPOD_TOUCH_TCP_USB_H
 * SLICE hw/arm/ipod_touch_tcp_usb.c range static bool tcp_usb_debug(void) | int tcp_usb_request(
 * SLICE hw/arm/ipod_touch_tcp_usb.c fn tcp_usb_request
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <netdb.h>
#define USB_DIR_IN 0x80
#define g_free free
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define trace_ipod_touch_tcp_usb_log(msg) ((void)(msg))
#define g_malloc0(n) calloc(1,n)
static void qemu_set_fd_handler(int fd,void (*r)(void *),void (*w)(void *),void *arg) {}
static ssize_t fragmented_read(int fd,void *p,size_t n) { return read(fd,p,n<=5?(n>2?2:n):(n>127?127:n)); }
static ssize_t fragmented_write(int fd,const void *p,size_t n) { return write(fd,p,n<=5?(n>2?2:n):(n>251?251:n)); }
#define read fragmented_read
#define write fragmented_write
#include "slice.h"
#undef read
#undef write
static unsigned completions;
static int last_status;
static uint8_t packet[32767];
static bool nak;
static int device_reply(tcp_usb_state_t *s,void *arg,tcp_usb_header_t *h,char *p) {
 if(nak)return -2;
 assert(h->length>=0);
 if(h->ep&USB_DIR_IN)memcpy(p,packet,h->length);else memcpy(packet,p,h->length);
 h->addr=9;return h->length;
}
static int host_reply(tcp_usb_state_t *s,void *arg,tcp_usb_header_t *h,char *p) {
 last_status=h->length;completions++;return 0;
}
static void transfer(tcp_usb_state_t *host,tcp_usb_state_t *dev,tcp_usb_header_t *h,uint8_t *p) {
 unsigned before=completions;
 assert(tcp_usb_request(host,h,(char*)p)==0);
 for(unsigned i=0;completions==before;i++) {
  assert(i<10000);tcp_usb_callback(dev,1,1);tcp_usb_callback(host,1,1);
 }
 assert(last_status==(nak?-2:h->length));
 assert(host->state==tcp_usb_idle);
}
static void roundtrip(const uint8_t *data,size_t size) {
 int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
 tcp_usb_state_t host,dev;tcp_usb_init(&host,host_reply,NULL,NULL);tcp_usb_init(&dev,device_reply,NULL,NULL);
 host.socket=sockets[0];dev.socket=sockets[1];host.closed=dev.closed=0;
 fcntl(sockets[0],F_SETFL,O_NONBLOCK);fcntl(sockets[1],F_SETFL,O_NONBLOCK);
 uint8_t reply[32767];
 const unsigned sizes[]={1,511,512,513,32767};
 for(size_t off=0,i=0;off<size;i++) {
  unsigned n=sizes[i%5];if(n>size-off)n=size-off;
  tcp_usb_header_t h={.ep=4,.length=n};
  transfer(&host,&dev,&h,(uint8_t*)data+off);assert(h.addr==9);
  memset(reply,0,sizeof(reply));h=(tcp_usb_header_t){.ep=0x83,.length=n};
  transfer(&host,&dev,&h,reply);assert(h.addr==9 && !memcmp(reply,data+off,n));off+=n;
 }
 nak=true;tcp_usb_header_t h={.ep=0x83,.length=128};transfer(&host,&dev,&h,reply);nak=false;
 tcp_usb_cleanup(&host);tcp_usb_cleanup(&dev);
}
static uint8_t *slurp(const char *path,long *size) {
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);
 assert(*size>0 && *size<=32*1024*1024);uint8_t *p=malloc(*size);assert(fread(p,1,*size,f)==(size_t)*size);fclose(f);
 return p;
}
int main(void) {
 static uint8_t synthetic[65535];for(unsigned i=0;i<sizeof(synthetic);i++)synthetic[i]=(i*31)^i;
 roundtrip(synthetic,sizeof(synthetic));
 char here[4096],pattern[4200];snprintf(here,sizeof(here),"%s",__FILE__);
 snprintf(pattern,sizeof(pattern),"%s/../../../qemu-ios-files/real-device/afc/t_*.bin",dirname(here));
 glob_t g;size_t n=0;
 if(!glob(pattern,0,NULL,&g)) {
  for(n=0;n<g.gl_pathc;n++) {
   char back[4200];snprintf(back,sizeof(back),"%s",g.gl_pathv[n]);
   char *t=strrchr(back,'/')+1;memmove(t+4,t+1,strlen(t+1)+1);memcpy(t,"back",4);   /* t_X -> back_X */
   long size,bsize;uint8_t *p=slurp(g.gl_pathv[n],&size),*b=slurp(back,&bsize);
   assert(size==bsize && !memcmp(p,b,size));   /* real-device reference matches */
   roundtrip(p,size);free(p);free(b);
  }
  globfree(&g);
 }
 printf("PASS: fragmented tcp_usb headers/payloads, odd transfer sizes, exact file bytes and NAK replies"
        " (%zu saved real-device AFC vectors plus synthetic data)\n",n);
}
