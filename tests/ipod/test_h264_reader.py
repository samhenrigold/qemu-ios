#!/usr/bin/env python3
"""ASan/UBSan check of the actual experimental H.264 MMIO bit reader."""
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_h264.c').read_text()
code = source[source.index('typedef struct IPodH264State'):source.index('static const MemoryRegionOps')]
start = code.index('static bool h264_decode(')
code = code[:start] + 'static bool h264_decode(IPodH264State *s) { return false; }\n' + code[code.index('static uint64_t h264_read', start):]
code += source[source.index('static int h264_put_rbsp'):source.index('static const VMStateInfo vmstate_h264_rbsp')]
code += source[source.index('static int h264_post_load'):source.index('static const VMStateDescription h264_vmstate')]
prelude = r"""
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
typedef void VMStateField;
typedef void JSONWriter;
typedef struct { uint8_t bytes[256]; size_t size,pos; int error; } QEMUFile;
static int qemu_file_get_error(QEMUFile *f) { return f->error; }
static void qemu_put_buffer(QEMUFile *f,const uint8_t *p,size_t n) {
 assert(n<=sizeof(f->bytes)-f->size);if(n)memcpy(f->bytes+f->size,p,n);f->size+=n;
}
static size_t qemu_get_buffer(QEMUFile *f,uint8_t *p,size_t n) {
 if(n>f->size-f->pos){n=f->size-f->pos;f->error=-EIO;}
 if(n)memcpy(p,f->bytes+f->pos,n);f->pos+=n;return n;
}
static void qemu_put_be32(QEMUFile *f,uint32_t n) { n=GUINT32_TO_BE(n);qemu_put_buffer(f,(void *)&n,4); }
static uint32_t qemu_get_be32(QEMUFile *f) { uint32_t n=0;qemu_get_buffer(f,(void *)&n,4);return GUINT32_FROM_BE(n); }

/* This check exercises the platform-independent MMIO path. Native session
 * lifetime and decoding are covered by test_h264_native.py. */
#undef __APPLE__
typedef int SysBusDevice;
typedef int MemoryRegion;
typedef int qemu_irq;
static bool irq;
static void qemu_irq_raise(int line) { irq=true; }
static void qemu_irq_lower(int line) { irq=false; }
#define clz32(x) __builtin_clz(x)
typedef uint64_t hwaddr;
#define warn_report(...) ((void)0)
#define trace_ipod_touch_h264_read(...) ((void)0)
#define trace_ipod_touch_h264_write(...) ((void)0)
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
static uint8_t dram[4096];
static int address_space_read(void *as, hwaddr a, int attrs, void *dst, size_t n) {
    if(a < 0x08000000 || a > 0x08001000 || n > 0x08001000-a) return -1;
    memcpy(dst,dram+(a-0x08000000),n); return 0;
}
"""
check = r"""
int main(void) {
    IPodH264State s = { .rbsp = g_byte_array_new() };
    /* NAL header, four bytes containing emulation prevention, final byte. */
    const uint8_t nal[] = {0x67,0,0,3,1,0xab};
    memcpy(dram+4,nal,sizeof(nal));
    h264_write(&s,0x1200,0x08000000 >> 10,4);
    h264_write(&s,0x180c,4,4); h264_write(&s,0x1810,4+sizeof(nal),4);
    h264_write(&s,0x1600,0x801,4);
    assert(h264_read(&s,0x1628,4) == 0x167);
    for(unsigned bit=0;bit<=32;bit++) {
        s.bit=bit;
        assert(h264_read(&s,0x1480,4)==(bit<32 ? 0x000001abu<<bit : 0));
        assert(s.bit==bit && !s.exhausted);
    }
    s.bit=0;
    assert(h264_read(&s,0x1400,4) == 0x000001ab && s.bit == 32);
    assert(!h264_read(&s,0x1404,4) && s.exhausted);
    /* 7E18 uses non-consuming, zero-padded lookahead to detect PPS trailing
     * bits. Reading beyond the physical tail of the peek window is legal. */
    g_byte_array_set_size(s.rbsp,1);s.rbsp->data[0]=0x80;s.bit=0;s.exhausted=false;
    assert(h264_read(&s,0x1480,4)==0x80000000 && !s.bit && !s.exhausted);
    assert(h264_read(&s,0x1404,4)==1);
    assert(!h264_read(&s,0x1480,4) && s.bit==1 && !s.exhausted);
    /* Small and extended unsigned Exp-Golomb values, including exact cursor. */
    for(unsigned zeros=0;zeros<32;zeros++) {
        g_byte_array_set_size(s.rbsp,8); memset(s.rbsp->data,0,8);
        s.rbsp->data[zeros/8] = 1 << (7-zeros%8);
        s.bit=0; s.exhausted=false;
        h264_write(&s,0x1078,1,4);
        unsigned status=h264_read(&s,0x107c,4), value;
        assert(status & 0x80);
        if(status & 0x40) {
            assert(s.bit == 0 && (status & 0x3f) == zeros);
            assert(!h264_read(&s,0x1400+zeros*4,4));
            value=h264_read(&s,0x1400+((zeros+1)&31)*4,4)-1;
        } else {
            value=status >> 8;
            assert((status & 0x3f) == zeros*2+1);
            assert(s.bit == 0);
            h264_read(&s,0x1400+(status & 31)*4,4);
        }
        assert(value == (1u << zeros)-1 && s.bit == zeros*2+1);
    }
    /* Signed command uses the same peek/consume contract. ue(2) -> se(-1). */
    s.rbsp->data[0]=0x60; s.bit=0;
    h264_write(&s,0x1078,3,4);
    assert(h264_read(&s,0x107c,4)==0xffffff83 && s.bit==0);
    assert(h264_read(&s,0x140c,4)==3 && s.bit==3);
    memset(s.rbsp->data,0,8);s.bit=0;s.exhausted=false;
    h264_write(&s,0x1078,1,4);assert(h264_read(&s,0x107c,4)==0x80);
    h264_write(&s,0x1810,3,4);h264_write(&s,0x1600,0x801,4);
    assert(!s.rbsp->len && h264_read(&s,0x1628,4)==0x100);
    h264_write(&s,0x1200,0x3fffff,4);h264_write(&s,0x1810,10,4);
    h264_write(&s,0x1600,0x801,4);assert(!s.rbsp->len);
    h264_write(&s,0x1004,0x3e0,4);assert(!h264_read(&s,0x1004,4));
    h264_write(&s,0x1000,1,4);assert(irq && h264_read(&s,0x1074,4)==2);
    h264_write(&s,0x1074,2,4);assert(!h264_read(&s,0x1074,4));
    h264_write(&s,0x10c0,0,4);assert(!irq);
    QEMUFile f={0};
    g_byte_array_append(s.rbsp,nal,sizeof(nal));s.bit=17;s.irq_level=true;
    assert(!h264_put_rbsp(&f,&s.rbsp,0,NULL,NULL));
    g_byte_array_set_size(s.rbsp,0);
    assert(!h264_get_rbsp(&f,&s.rbsp,0,NULL));
    assert(s.rbsp->len==sizeof(nal)&&!memcmp(s.rbsp->data,nal,sizeof(nal)));
    assert(!h264_post_load(&s,1)&&s.bit==17&&irq);
    s.bit=sizeof(nal)*8+1;assert(h264_post_load(&s,1)==-EINVAL);
    f=(QEMUFile){0};qemu_put_be32(&f,4*1024*1024+1);
    assert(h264_get_rbsp(&f,&s.rbsp,0,NULL)==-EINVAL);
    f=(QEMUFile){0};qemu_put_be32(&f,8);qemu_put_buffer(&f,nal,2);
    assert(h264_get_rbsp(&f,&s.rbsp,0,NULL)==-EIO);
    g_byte_array_unref(s.rbsp);
    puts("PASS: H.264 NAL/RBSP, consuming reads, padded lookahead, Exp-Golomb, DMA bounds and snapshot framing");
}
"""
with tempfile.TemporaryDirectory(prefix='h264-reader-') as directory:
    main=Path(directory)/'check.c'; exe=Path(directory)/'check'
    main.write_text(prelude+code+check)
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
    subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(main),*flags,'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
