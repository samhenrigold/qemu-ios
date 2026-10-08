/* Decode production touch frames as the guest does, including empty polls.
 *
 * SLICE:wire include/hw/arm/ipod_touch_multitouch.h range #define MT_INTERFACE_VERSION | typedef struct IPodTouchMultitouchState
 * SLICE hw/arm/ipod_touch_multitouch.c range const MTSensorProfile mt_profile_ipod = { | const MTSensorProfile mt_profile_n81
 * SLICE hw/arm/ipod_touch_multitouch.c fn mt_clamp_vel mt_frame_slots mt_const_fingerid mt_build_frame
 */
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
#include "wire.h"
typedef struct { uint64_t last_frame_timestamp; uint32_t frame_counter; const MTSensorProfile *profile; } IPodTouchMultitouchState;
#include "slice.h"
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
    IPodTouchMultitouchState s={.profile=&mt_profile_ipod};MTFingerState fingers[5]={0};
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
    assert(p[45]==5 && p[46]==MT_EVENT_TOUCH_ENDED);
    /* the iPod's measured frame (offset + span) places the lifted finger */
    assert((int16_t)le16(p+49)==-74+(int)(.5f*4720) && (int16_t)le16(p+51)==-187+(int)(.5f*7329));
    free(frame);
    /* K48: the measured frame offset + span (ipad1 calibration) place the finger */
    s.profile=&mt_profile_k48;memset(fingers,0,sizeof(fingers));
    fingers[0]=(MTFingerState){.phase=MT_FINGER_DOWN,.x=.25f,.y=.75f};now+=100000000;
    frame=mt_build_frame(&s,fingers,&length);inspect(frame,length,1);p=(uint8_t*)frame;
    assert((int16_t)le16(p+49)==-72+(int)(.25f*14566) && (int16_t)le16(p+51)==33+(int)(.75f*19465));
    free(frame);
    puts("PASS: empty, one-to-five contact and sparse-slot frames; all checksums and idle timestamp");
}
