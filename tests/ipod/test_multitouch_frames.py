#!/usr/bin/env python3
"""Decode production touch frames as the guest does, including empty polls."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'hw/arm/ipod_touch_multitouch.c').read_text()
header = (root/'include/hw/arm/ipod_touch_multitouch.h').read_text()
wire = header[header.index('#define MT_INTERFACE_VERSION'):header.index('typedef struct IPodTouchMultitouchState')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define g_malloc0(n) calloc(1,n)
#define QEMU_CLOCK_VIRTUAL 0
#define MTT(...) ((void)0)
static bool mt_trace(void) { return false; }
static uint64_t now;
static uint64_t qemu_clock_get_ns(int clock) { return now; }
''' + wire + r'''
typedef struct { uint64_t last_frame_timestamp; uint32_t frame_counter; } IPodTouchMultitouchState;
'''
for name in ('mt_clamp_vel', 'mt_frame_slots', 'mt_const_fingerid', 'mt_build_frame'):
    match = re.search(r'^static [^\n]*\b'+name+r'\([^;]*?\n\{.*?^}', source, re.M|re.S)
    assert match, name
    code += match.group()+'\n'
code += r'''
static unsigned le16(uint8_t *p) { return p[0] | p[1]<<8; }
static void inspect(MTFrame *frame, unsigned length, unsigned fingers) {
    uint8_t *p=(uint8_t *)frame;
    unsigned data=le16(p+1), sum=0;
    assert(length==21+data && length<=0x400);
    assert(data==24+(fingers ? fingers : 1)*28+2);
    for(unsigned i=0;i<14;i++)sum+=p[i];
    assert((sum&65535)==le16(p+14));
    sum=0;for(unsigned i=16;i<21;i++)sum+=p[i];assert(!(sum&255));
    assert(le16(p+18)==data);
    assert(frame->frame_packet.header.numFingers==fingers);
    sum=0;for(unsigned i=21;i<length-2;i++)sum+=p[i];
    assert((sum&65535)==le16(p+length-2));
    if(!fingers) for(unsigned i=45;i<73;i++) assert(!p[i]);
}
int main(void) {
    unsetenv("IT_MT_PAD_FINGERS");unsetenv("IT_MT_FINGERID");
    IPodTouchMultitouchState s={0};MTFingerState fingers[5]={0};
    for(unsigned n=0;n<=5;n++) {
        memset(fingers,0,sizeof(fingers));
        for(unsigned i=0;i<n;i++)fingers[i]=(MTFingerState){.phase=MT_FINGER_DOWN,.x=.5f,.y=.5f};
        now+=100000000;unsigned length;MTFrame *frame=mt_build_frame(&s,fingers,&length);
        inspect(frame,length,n);
        uint64_t timestamp=s.last_frame_timestamp;
        free(frame);
        now+=1000000;frame=mt_build_frame(&s,NULL,&length);inspect(frame,length,0);
        assert(s.last_frame_timestamp==timestamp);free(frame);
    }
    memset(fingers,0,sizeof(fingers));
    fingers[4]=(MTFingerState){.phase=MT_FINGER_LIFTED,.x=.5f,.y=.5f};
    unsigned length;now+=100000000;
    MTFrame *frame=mt_build_frame(&s,fingers,&length);inspect(frame,length,1);
    uint8_t *p=(uint8_t*)frame;
    assert(p[45]==5 && p[46]==MT_EVENT_TOUCH_ENDED);free(frame);
    puts("PASS: empty, one-to-five contact and sparse-slot frames; all checksums and idle timestamp");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/'check.c';path.write_text(code)
    binary=str(Path(directory)/'check')
    subprocess.run(['clang','-std=gnu11','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(path),'-o',binary],check=True)
    subprocess.run([binary],check=True)
