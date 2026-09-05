#!/usr/bin/env python3
"""Amplifier control bytes must change complete stereo frames at the host sink."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
amp = (root/'hw/arm/ipod_touch_lm48821.c').read_text()
i2s = (root/'hw/arm/ipod_touch_i2s.c').read_text()
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#define IT_I2S_RING_SIZE 32
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CLAMP(x,a,b) ((x)<(a)?(a):((x)>(b)?(b):(x)))
typedef struct { uint8_t control; } LM48821State;
typedef LM48821State I2CSlave;
typedef LM48821State DeviceState;
#define LM48821(s) (s)
struct audsettings { unsigned freq; };
typedef struct {
    uint8_t ring[IT_I2S_RING_SIZE];
    uint32_t ring_format[IT_I2S_RING_SIZE/4];
    unsigned ring_head, voice_rate, fifo_bytes;
    uint64_t total_bytes, dropped, last_push_ns;
    bool pushed_since_tick, active, card_ok;
    struct audsettings as;
    void *pace_timer, *card;
    FILE *dump;
    unsigned ring_tail, ring_level;
    void *voice;
    LM48821State *amplifier;
    double output_gain;
} IPodTouchI2SState;
#define QEMU_CLOCK_VIRTUAL 0
static unsigned scheduled;
static uint64_t qemu_clock_get_ns(int clock) { return 1000; }
static void timer_mod(void *timer, uint64_t now) { assert(now==1000);scheduled++; }
static void it_i2s_trace(IPodTouchI2SState *s) {}
static void it_i2s_activate(IPodTouchI2SState *s) {}
static void it_i2s_update_dma_req(IPodTouchI2SState *s) {}
static void it_i2s_out_cb(void *s,int free_bytes) {}
static void *AUD_open_out(void *card,void *voice,const char *name,void *opaque,
                         void (*cb)(void *,int),struct audsettings *as) { return opaque; }
static void AUD_set_volume_out(void *voice,int mute,int left,int right) {}
static void AUD_set_active_out(void *voice,int active) {}
#define warn_report(...) ((void)0)
static uint16_t lduw_le_p(const uint8_t *p) { return p[0] | (p[1]<<8); }
static void stw_le_p(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static uint8_t played[64];
static unsigned count, limit=64;
static size_t AUD_write(void *voice, void *data, size_t size) {
    unsigned n=MIN(size,limit); n &= ~3u;
    assert(count+n<=sizeof(played));
    memcpy(played+count,data,n); count+=n;
    return n;
}
'''
for name in ('lm48821_gain','lm48821_recv','lm48821_send','lm48821_reset'):
    code += re.search(r'^(?:static )?(?:double|uint8_t|int|void) '+name+r'\(.*?^}', amp, re.M|re.S).group()+'\n'
for name in ('it_i2s_drain','it_i2s_update_voice','it_i2s_push'):
    code += re.search(r'^static void '+name+r'\([^;]*?\n\{.*?^}',i2s,re.M|re.S).group()+'\n'
code += r'''
int main(void) {
    LM48821State amp={0};
    IPodTouchI2SState s={.amplifier=&amp,.output_gain=1,.voice_rate=44100,.as={44100},.card_ok=true};
    assert(lm48821_gain(amp.control,0)==0 && lm48821_gain(amp.control,1)==0);
    assert(lm48821_send(&amp,0x9b)==0 && lm48821_recv(&amp)==0x9b);
    assert(lm48821_gain(amp.control,0)==1 && lm48821_gain(amp.control,1)==1);
    /* Back-to-back control bytes are commands, never register/data pairs. */
    lm48821_send(&amp,0x83);
    assert(fabs(lm48821_gain(amp.control,0)-pow(10,-6.0/20))<1e-12);
    lm48821_send(&amp,0x9a);
    assert(lm48821_gain(amp.control,0)==1 && lm48821_gain(amp.control,1)==0);
    lm48821_send(&amp,0x9f);
    assert(fabs(lm48821_gain(amp.control,0)-pow(10,-76.0/20))<1e-12);
    lm48821_send(&amp,0xb3); /* +6 dB */
    uint8_t samples[8];
    stw_le_p(samples,20000);stw_le_p(samples+2,-20000);
    stw_le_p(samples+4,1000);stw_le_p(samples+6,-1000);
    it_i2s_push(&s,samples,6);limit=4;
    lm48821_send(&amp,0); /* a later mute cannot erase queued audio */
    it_i2s_drain(&s,64);
    assert(count==4 && s.ring_level==2 && s.ring_tail==4);
    assert((int16_t)lduw_le_p(played)==32767 && (int16_t)lduw_le_p(played+2)==-32768);
    it_i2s_push(&s,samples+6,2);
    it_i2s_drain(&s,64);
    assert(count==8 && s.ring_level==0);
    assert((int16_t)lduw_le_p(played+4)==1995 && (int16_t)lduw_le_p(played+6)==-1995);
    /* Wrapped stereo frames and output calibration preserve channel order. */
    s.ring_head=s.ring_tail=28;s.ring_level=0;s.output_gain=.5;count=0;
    lm48821_send(&amp,0x9b);
    stw_le_p(samples,100);stw_le_p(samples+2,200);
    stw_le_p(samples+4,300);stw_le_p(samples+6,400);
    it_i2s_push(&s,samples,8);
    it_i2s_drain(&s,8);
    assert(count==8 && s.ring_tail==4 && !s.ring_level);
    for(unsigned i=0;i<4;i++) assert(lduw_le_p(played+i*2)==50*(i+1));
    /* Rate changes are serviced outside the audio callback, after old-rate
     * frames drain. A later control byte only affects subsequent frames. */
    count=0;s.output_gain=1;s.as.freq=22050;
    it_i2s_push(&s,samples,4);
    s.as.freq=44100;lm48821_send(&amp,0);
    it_i2s_push(&s,samples+4,4);
    it_i2s_drain(&s,8);assert(!count && scheduled==1);
    it_i2s_update_voice(&s);assert(s.voice_rate==22050);
    it_i2s_drain(&s,8);assert(count==4 && scheduled==2 && s.ring_level==4);
    assert(lduw_le_p(played)==100 && lduw_le_p(played+2)==200);
    it_i2s_update_voice(&s);assert(s.voice_rate==44100);
    it_i2s_drain(&s,8);assert(count==8 && !s.ring_level);
    assert(!lduw_le_p(played+4) && !lduw_le_p(played+6));
    lm48821_reset(&amp);assert(lm48821_recv(&amp)==0);
    puts("PASS: amplifier gain/mute/channel control, clipping, partial writes, wrap and queued rate/gain transitions");
}
'''
with tempfile.TemporaryDirectory(prefix='audio-volume-') as tmp:
    c,exe=Path(tmp)/'test.c',Path(tmp)/'test'
    c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
