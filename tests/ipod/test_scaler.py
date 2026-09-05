#!/usr/bin/env python3
"""Run the actual scaler MMIO and conversion code under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
s = (Path(__file__).resolve().parents[2] / 'hw/arm/ipod_touch_scaler.c').read_text()
s = s[s.index('typedef struct {'):s.index('static const MemoryRegionOps')]
header = r'''
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef uint64_t hwaddr;
typedef int *qemu_irq;
static uint8_t ram[0x100000];
#define BASE 0x0f600000
static void cpu_physical_memory_read(uint64_t a,void *p,size_t n)
{ assert(a>=BASE && a+n<=BASE+sizeof(ram));memcpy(p,ram+(a-BASE),n); }
static void cpu_physical_memory_write(uint64_t a,const void *p,size_t n)
{ assert(a>=BASE && a+n<=BASE+sizeof(ram));memcpy(ram+(a-BASE),p,n); }
static void stw_le_p(void *p,unsigned v) { uint8_t *b=p;b[0]=v;b[1]=v>>8; }
static void qemu_set_irq(qemu_irq irq,int level) { *irq=level; }
#define error_report(...) ((void)0)
'''
check = r'''
int main(void)
{
    int irq=0;IPodScalerState s={.irq=&irq};
    uint32_t *r=s.regs;
    r[1]=0x200;r[4]=0;r[5]=BASE;r[6]=BASE+256;r[7]=(6<<16)|6;
    r[9]=(4<<16)|2;r[12]=4;r[13]=BASE+512;r[15]=(6<<16)|6;r[16]=r[9];
    unsigned matrix[]={596,0,817,596,0xf38,0xe60,596,1033,0};
    memcpy(r+0x220/4,matrix,sizeof(matrix));
    memset(ram,0xa5,sizeof(ram));
    uint8_t luma[]={16,235,81,81};memcpy(ram,luma,4);memcpy(ram+6,luma,4);
    uint8_t uv[]={128,128,90,240};memcpy(ram+256,uv,4);
    scaler_write(&s,4,0x201,4);
    assert(!irq && r[1]==0x200 && scaler_read(&s,12,4)==1);
    uint8_t expected[]={0,0,255,255,0,248,0,248};
    assert(!memcmp(ram+512,expected,8) && !memcmp(ram+524,expected,8));
    for(int i=520;i<524;i++) assert(ram[i]==0xa5);
    scaler_write(&s,8,1,4);assert(irq);
    scaler_write(&s,12,1,4);assert(!irq && !r[3]);
    r[12]=6;r[15]=(6<<16)|6;
    scaler_write(&s,4,0x201,4);assert(irq);
    assert(!memcmp(ram+512,"\0\0\0\xff\xff\xff\xff\xff\0\0\xfe\xff",12));
    r[13]=0x10000000;assert(!scaler_convert(&s));
    r[13]=BASE+512;r[9]|=1;assert(!scaler_convert(&s));
    r[9]&=~1u;r[16]++;assert(!scaler_convert(&s));
    scaler_write(&s,4,2,4);assert(!irq);
    for(unsigned i=0;i<G_N_ELEMENTS(s.regs);i++) assert(!s.regs[i]);
    puts("PASS: NV12 matrix conversion, RGB565/BGRA, padding, DMA bounds, IRQ mask/ack and reset");
}
'''
with tempfile.TemporaryDirectory(prefix='it-scaler-') as tmp:
    c=Path(tmp)/'check.c';exe=Path(tmp)/'check';c.write_text(header+s+check)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe),*flags],check=True)
    subprocess.run([str(exe)],check=True)
