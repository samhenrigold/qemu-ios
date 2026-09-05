#!/usr/bin/env python3
"""Check reference changes and missing slices against independently known pixels.

Use PKG_CONFIG_PATH pointing to the native package's patched FFmpeg prefix.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'hw/arm/ipod_touch_h264.c').read_text()
code = source[source.index('typedef struct IPodH264State'):source.index('static const MemoryRegionOps')]
prelude = r'''
#undef __APPLE__
#define IT_HAVE_AVCODEC 1
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <glib.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef int qemu_irq;
typedef uint64_t hwaddr;
static void qemu_irq_raise(int irq) {}
static void qemu_irq_lower(int irq) {}
#define trace_ipod_touch_h264_read(...) ((void)0)
#define trace_ipod_touch_h264_write(...) ((void)0)
#define clz32(x) __builtin_clz(x)
#define MEMTXATTRS_UNSPECIFIED 0
#define warn_report(...) fprintf(stderr,__VA_ARGS__)
static int address_space_memory;
static uint8_t ram[65536];
static void stl_be_p(void *p,uint32_t v) { v=GUINT32_TO_BE(v);memcpy(p,&v,4); }
static int address_space_read(void *as,hwaddr a,int attrs,void *p,size_t n) {
    assert(a>=0x08000000 && a<=0x08010000 && n<=0x08010000-a);
    memcpy(p,ram+(a-0x08000000),n);return 0;
}
static int address_space_write(void *as,hwaddr a,int attrs,const void *p,size_t n) {
    assert(a>=0x08000000 && a<=0x08010000 && n<=0x08010000-a);
    memcpy(ram+(a-0x08000000),p,n);return 0;
}
'''
check = r'''
static void pcm_slice(IPodH264State *s, unsigned luma, bool leading_dc)
{
    uint8_t pcm[4096]={0};unsigned bit=s->bit;
    for(unsigned mb=0;mb<8;mb++) {
        if(leading_dc && !mb) {
            h264_ue(pcm,&bit,3);h264_ue(pcm,&bit,0);
            h264_se(pcm,&bit,0);h264_put(pcm,&bit,1,1);continue;
        }
        h264_ue(pcm,&bit,25);
        while(bit%8)h264_put(pcm,&bit,0,1);
        for(unsigned i=0;i<384;i++)h264_put(pcm,&bit,i<256 ? luma : 128,8);
    }
    h264_put(pcm,&bit,1,1);
    g_byte_array_set_size(s->rbsp,0);
    g_byte_array_append(s->rbsp,pcm,(bit+7)/8);
}

int main(void) {
  for(unsigned variant=0;variant<28;variant++) {
    IPodH264State s={.rbsp=g_byte_array_new()};
    uint32_t *r=s.regs;
    r[0x1030/4]=4;r[0x1034/4]=4; /* 64x64, sixteen macroblocks */
    r[0x1028/4]=26;r[0x105c/4]=1; /* disable cross-slice deblocking */
    r[0x100c/4]=1;r[0x10d4/4]=2;
    r[0x1040/4]=1;r[0x106c/4]=0x0201;
    r[0x1200/4+1]=0x08000000>>10;r[0x1200/4+2]=0x08001000>>10;
    r[0x1200/4+3]=0x08002000>>10;r[0x1200/4+4]=0x08003000>>10;
    r[0x1200/4+5]=0x08004000>>10;r[0x1200/4+6]=0x08005000>>10;
    r[0x100/4]=0x0403;r[0x104/4]=0x0605;
    memset(ram,0xa5,6144);
    memset(ram+0x2000,80,4096);memset(ram+0x3000,128,2048);
    memset(ram+0x4000,180,4096);memset(ram+0x5000,128,2048);
    /* Each P slice skips eight MBs: every pixel copies its L0[0] reference.
     * ue(8) + rbsp_stop_one_bit = 00010011; no motion/entropy approximation. */
    /* Omit a row/one macroblock, or overlap the following slice by one MB. */
    const uint8_t skip=variant==4 ? 0x2c : variant==5 ? 0x11 : variant==6 ? 0x15 : 0x13;
    g_byte_array_append(s.rbsp,&skip,1);
    if(variant==9) {
        /* I16x16 DC prediction with no residual: all samples are 128. */
        r[0x102c/4]=2;
        uint8_t intra[9]={0};unsigned bit=0;
        for(unsigned mb=0;mb<8;mb++) {
            h264_ue(intra,&bit,3);h264_ue(intra,&bit,0);
            h264_se(intra,&bit,0);h264_put(intra,&bit,1,1);
        }
        h264_put(intra,&bit,1,1);g_byte_array_set_size(s.rbsp,0);
        g_byte_array_append(s.rbsp,intra,(bit+7)/8);
    }
    if(variant>=11) {
        r[0x102c/4]=2;s.bit=(variant-11)%8;
        pcm_slice(&s,80,variant>=19);
    }
    assert(h264_decode_software(&s) && s.partial);
    h264_write(&s,0x1004,0x0c,4);assert(s.partial && s.codec);
    for(unsigned i=0;i<6144;i++)assert(ram[i]==0xa5);
    r[0x1038/4]=2;
    r[0x102c/4]=0;s.bit=0;
    g_byte_array_set_size(s.rbsp,0);
    const uint8_t rest=0x13;g_byte_array_append(s.rbsp,&rest,1);
    r[0x100/4]=0x0605;r[0x104/4]=0x0403; /* swap L0 for the bottom half */
    if(variant==7) {
        h264_write(&s,0x1004,0x7ff,4);assert(!s.partial && !s.codec);
    }
    if(variant==8) {
        /* An I slice may finish a non-IDR picture begun with a P slice. */
        r[0x102c/4]=2;
        pcm_slice(&s,180,false);
    }
    if(variant==1)r[0x1040/4]=0; /* one active reference */
    if(variant==2) {
        r[0x1040/4]=2;r[0x104/4]=0x0605;r[0x108/4]=0x0403;
    }
    if(variant==3) {
        r[0x1200/4+7]=0x08006000>>10;r[0x1200/4+8]=0x08007000>>10;
        r[0x100/4]=0x0807; /* introduced by this slice, replay the earlier one */
        memset(ram+0x6000,220,4096);memset(ram+0x7000,128,2048);
    }
    if(variant==10)s.partial_bytes=64*1024*1024;
    if(variant==27)r[0x1018/4]=1; /* PPS must remain stable within a picture. */
    if((variant>=4 && variant<=7) || variant==10 || variant==27) {
        assert(!h264_decode_software(&s) && !s.partial);
        assert(!s.codec && !s.partial_slices && !s.partial_bytes);
        for(unsigned i=0;i<6144;i++)assert(ram[i]==0xa5);
        h264_decoder_close(&s);g_byte_array_unref(s.rbsp);continue;
    }
    assert(h264_decode_software(&s) && !s.partial);
    assert(!s.partial_slices->len && !s.partial_bytes);
    for(unsigned i=0;i<4096;i++) {
        unsigned top=(variant==9 || (variant>=19 && i/64<16 && i%64<16)) ? 128 : 80;
        assert(ram[i]==(i<2048 ? top : variant==3 ? 220 : 180));
    }
    for(unsigned i=4096;i<6144;i++)assert(ram[i]==128);
    h264_decoder_close(&s);g_byte_array_unref(s.rbsp);
  }
  /* Constrained flag, hardware alpha, hardware beta; last five are invalid. */
  const int settings[][3]={{0,0,0},{1,12,-12},{2,0,0},
                          {0,1,0},{0,0,-1},{0,14,0},{0,0,-14}};
  for(unsigned sample=0;sample<G_N_ELEMENTS(settings);sample++) {
    unsigned constrained=settings[sample][0];
    IPodH264State s={.rbsp=g_byte_array_new()};uint32_t *r=s.regs;
    r[0x1030/4]=4;r[0x1034/4]=4;r[0x1028/4]=26;r[0x105c/4]=1;
    r[0x100c/4]=1;r[0x10d4/4]=2;r[0x1018/4]=constrained;
    r[0x1060/4]=settings[sample][1];r[0x1064/4]=settings[sample][2];
    r[0x106c/4]=0x0201;r[0x100/4]=0x0403;
    for(unsigned j=1;j<=4;j++)r[0x1200/4+j]=(0x08000000+(j-1)*4096)>>10;
    memset(ram,0xa5,6144);memset(ram+0x2000,80,4096);
    memset(ram+0x3000,128,2048);
    uint8_t payload[16]={0};unsigned bit=0;
    /* P-skip, I16x16 DC without residual, then fourteen P-skips.
     * The intra block may use its left inter neighbor only when unconstrained. */
    h264_ue(payload,&bit,1);h264_ue(payload,&bit,8);
    h264_ue(payload,&bit,0);h264_se(payload,&bit,0);
    h264_put(payload,&bit,1,1);h264_ue(payload,&bit,14);
    h264_put(payload,&bit,1,1);
    g_byte_array_append(s.rbsp,payload,(bit+7)/8);
    if(sample>=2) {
        assert(!h264_decode_software(&s));
        for(unsigned j=0;j<6144;j++)assert(ram[j]==0xa5);
        h264_decoder_close(&s);g_byte_array_unref(s.rbsp);continue;
    }
    assert(h264_decode_software(&s) && !s.partial);
    for(unsigned j=0;j<4096;j++) {
        bool intra=j/64<16 && j%64>=16 && j%64<32;
        assert(ram[j]==(intra && constrained ? 128 : 80));
    }
    for(unsigned j=4096;j<6144;j++)assert(ram[j]==128);
    h264_decoder_close(&s);g_byte_array_unref(s.rbsp);
  }
    puts("PASS: reference replay, mixed slices, all PCM alignments, constrained prediction, coverage, reset and memory bound");
}
'''
with tempfile.TemporaryDirectory(prefix='h264-slices-') as tmp:
    c,exe=Path(tmp)/'check.c',Path(tmp)/'check';c.write_text(prelude+code+check)
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0','libavcodec','libavutil'],text=True))
    rpaths=[f'-Wl,-rpath,{flag[2:]}' for flag in flags if flag.startswith('-L')]
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),*flags,*rpaths,'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
