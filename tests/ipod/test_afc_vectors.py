#!/usr/bin/env python3
"""Replay saved AFC file bytes through production tcp_usb with fragmented I/O.

This tests USB transport framing, not a guest AFC server. Real-device vectors
are optional; synthetic odd-sized payloads always run.
"""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
header=(root/'include/hw/arm/ipod_touch_tcp_usb.h').read_text()
header=re.sub(r'^#include.*\n','',header,flags=re.M)
source=(root/'hw/arm/ipod_touch_tcp_usb.c').read_text().replace('#include "hw/arm/ipod_touch_tcp_usb.h"','')
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#define USB_DIR_IN 0x80
#define g_free free
#define g_malloc0(n) calloc(1,n)
static void qemu_set_fd_handler(int fd,void (*r)(void *),void (*w)(void *),void *arg) {}
static ssize_t fragmented_read(int fd,void *p,size_t n) { return read(fd,p,n<=5?(n>2?2:n):(n>127?127:n)); }
static ssize_t fragmented_write(int fd,const void *p,size_t n) { return write(fd,p,n<=5?(n>2?2:n):(n>251?251:n)); }
#define read fragmented_read
#define write fragmented_write
''' + header + source + r'''
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
int main(int argc,char **argv) {
 uint8_t synthetic[65535];for(unsigned i=0;i<sizeof(synthetic);i++)synthetic[i]=(i*31)^i;
 roundtrip(synthetic,sizeof(synthetic));
 for(int i=1;i<argc;i++) {
  FILE *f=fopen(argv[i],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
  assert(size>0 && size<=32*1024*1024);uint8_t *p=malloc(size);assert(fread(p,1,size,f)==size);fclose(f);
  roundtrip(p,size);free(p);
 }
 puts("PASS: fragmented tcp_usb headers/payloads, odd transfer sizes, exact file bytes and NAK replies");
}
'''
fixtures=root.parent/'qemu-ios-files/real-device/afc'
vectors=sorted(fixtures.glob('t_*.bin'))
for vector in vectors:
    back=vector.with_name(vector.name.replace('t_','back_',1))
    assert vector.read_bytes()==back.read_bytes(), f'real-device reference mismatch: {vector}'
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/'check.c';path.write_text(code)
    binary=str(Path(directory)/'check')
    subprocess.run(['clang','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(path),'-o',binary],check=True)
    subprocess.run([binary,*map(str,vectors)],check=True,timeout=90)
print(f'Replayed {len(vectors)} saved real-device AFC vectors (plus synthetic data)')
