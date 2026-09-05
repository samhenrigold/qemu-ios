#!/usr/bin/env python3
"""Exercise the real gated audio DMA/PCM code. Requires glib and libavcodec.

Uses original silent/short codec packets and synthetic PCM to check the hardware
buffer contract independently of any guest app or copyrighted media asset.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_amc.c').read_text()
start = source.index('typedef enum AMCProgram')
code = source[start:source.index('\n#else', start)]
header = (root / 'include/hw/arm/ipod_touch_amc.h').read_text()
constants = '\n'.join(line for line in header.splitlines() if line.startswith('#define AMC_'))
def function(name):
    begin = source.index('static ', source.index(name)-30)
    return source[begin:source.index('\n}', begin)+2]
# Read the exact shared MMIO paths as well as the codec helpers.
mmio = '\n'.join(function(name) for name in
    ('amc_update_irq(', 'amc_ctrl_of('))
begin = source.index('static const uint32_t amc_banks[]')
mmio += '\n' + source[begin:source.index('\n}', begin)+2]
mmio += '\n' + function('ipod_touch_amc_write(')
mmio += '\n' + function('amc_decode_tick(')
prelude = r'''
#include <glib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IT_HAVE_AVCODEC 1
typedef uint64_t hwaddr;
typedef struct {
    void *decoder, *decode_timer; uint32_t pending, regs[0x3000/4], int_mask[2];
    bool codec_decode, state_handshake, irq_armed; int irq;
} IPodTouchAMCState;
#define IPOD_TOUCH_AMC(s) ((IPodTouchAMCState *)(s))
#define AMC_REG(off) (s->regs[(off)/4])
static bool irq_level;
static unsigned timer_starts;
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int clock) { return 1000; }
static void timer_mod(void *timer, int64_t ns) {
    assert(ns == 1001000); timer_starts++;
}
static void qemu_set_irq(int irq, bool level) { irq_level = level; }
static void amc_log_caller(hwaddr addr, uint32_t value) {}
static void amc_write_result_block(IPodTouchAMCState *s) {}
#define MEMTXATTRS_UNSPECIFIED 0
#define AMCT(...) ((void)0)
static bool amc_trace(void) { return false; }
#define warn_report(...) fprintf(stderr, __VA_ARGS__)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define le32_to_cpu(x) GUINT32_FROM_LE(x)
static unsigned char dram[65536], aperture[0x30000];
static int address_space_memory;
static void stw_le_p(void *p, uint16_t v) { v = GUINT16_TO_LE(v); memcpy(p, &v, 2); }
static void stl_be_p(void *p, uint32_t v) { v = GUINT32_TO_BE(v); memcpy(p, &v, 4); }
static uint16_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v,p,2); return GUINT16_FROM_LE(v); }
static unsigned char *memory(hwaddr a, size_t n) {
    if (a >= 0x08000000 && a - 0x08000000 <= sizeof(dram) &&
        n <= sizeof(dram) - (a - 0x08000000)) return dram + (a - 0x08000000);
    if (a >= AMC_BUF_BASE && a - AMC_BUF_BASE <= sizeof(aperture) &&
        n <= sizeof(aperture) - (a - AMC_BUF_BASE)) return aperture + (a - AMC_BUF_BASE);
    return NULL;
}
static int address_space_read(void *as, hwaddr a, int attrs, void *dst, size_t n) {
    void *p = memory(a,n); if (!p) return -1; memcpy(dst,p,n); return 0;
}
static int address_space_write(void *as, hwaddr a, int attrs, const void *src, size_t n) {
    void *p = memory(a,n); if (!p) return -1; memcpy(p,src,n); return 0;
}
static void word(unsigned offset, uint32_t v) { v = GUINT32_TO_LE(v); memcpy(dram+offset,&v,4); }
'''
check = r'''
int main(void) {
    IPodTouchAMCState s = {0};
    assert(amc_program(&s) == AMC_UNKNOWN);
    s.regs[0x940/4]=0x84006e00; s.regs[0x960/4]=0xc600b800;
    s.regs[0x964/4]=0x848cba5d; s.regs[0x968/4]=0xc013f7fb;
    assert(!amc_dram(0x07ffffff, 1));
    assert(!amc_dram(0x10000000, 0));
    assert(!amc_dram(0x0fffffff, 2));
    assert(amc_dram(0x0fffffff, 1));
    assert(!amc_decode_dma(&s, 0));
    assert(!amc_decode_dma(&s, AMC_BUF_BASE));
    const uint8_t silence[] = {0x20,0x68,0,1,0xa0,0,0x0e};
    memcpy(dram+0x1000,silence,sizeof(silence));
    stw_le_p(aperture+0x2ff00,7);
    stw_le_p(aperture+0x2ff06,4);
    word(0,0); word(4,sizeof(silence)<<16); word(8,0x08001000);
    assert(amc_decode_dma(&s,0x08000001));
    amc_decode_drain(&s);
    AMCDecoder *d = s.decoder;
    assert(d && d->pcm->len == 4096);
    for (unsigned i=0;i<d->pcm->len;i++) assert(d->pcm->data[i] == 0);
    /* Check interleaving, alternating slots, and no overwrite before release. */
    g_byte_array_set_size(d->pcm,8192);
    g_queue_push_tail(&d->output_sizes,GUINT_TO_POINTER(4096));
    for(unsigned i=0;i<4096;i++) stw_le_p(d->pcm->data+i*2,(i&1)?-1234:5678);
    uint8_t *header = aperture+AMC_RESULT_OFFSET;
    amc_decode_publish(&s);
    assert(d->cursor == 4096 && d->slot == 1 && s.pending == 4);
    assert(lduw_le_p(header+0xa) == 1 && lduw_le_p(header+0xe) == 0);
    assert(!memcmp(header+0x100,d->pcm->data,4096));
    amc_decode_publish(&s);
    assert(d->cursor == 4096); /* consumer still owns the first buffer */
    s.pending=0; /* second slot is free while the first remains owned */
    amc_decode_publish(&s);
    assert(d->cursor == 8192 && d->slot == 0 && s.pending == 4);
    assert(lduw_le_p(header+0xe) == 1 && lduw_le_p(header+0xa) == 1);
    assert(!memcmp(header+0x1100,d->pcm->data+4096,4096));
    word(0,0x08000001); /* malformed cyclic chain must terminate */
    assert(!amc_decode_dma(&s,0x08000001));
    word(0,0); word(8,0x0ffffffc); /* DMA crossing DRAM end */
    assert(!amc_decode_dma(&s,0x08000001));
    amc_decoder_close(&s); assert(!s.decoder);
    /* A large compressed input stays in the codec until PCM consumers catch up. */
    for (unsigned i=0;i<1000;i++) memcpy(dram+0x1000+i*7,silence,7);
    word(4,7000u<<16); word(8,0x08001000);
    assert(amc_decode_dma(&s,0x08000001));
    d=s.decoder; unsigned frames=0;
    while(d->input_pending) {
        amc_decode_drain(&s);
        assert(s.decoder == d && d->pcm->len <= 65536);
        frames += d->pcm->len / 4096;
        d->cursor = d->pcm->len;
        g_queue_clear(&d->output_sizes);
    }
    assert(frames == 1000);
    amc_decoder_close(&s);
    /* A final mono access unit must be published without waiting for another
     * frame to fill the stereo-sized buffer. */
    const uint8_t mono[] = {0x01,0x18,0x20,0x07};
    memcpy(dram+0x1000,mono,sizeof(mono));
    word(4,sizeof(mono)<<16);
    assert(amc_decode_dma(&s,0x08000001));
    amc_decode_drain(&s);
    d=s.decoder;
    assert(d && d->channels == 1 && d->pcm->len == 2048);
    memset(header,0,0x12); s.pending=0;
    amc_decode_publish(&s);
    assert(d->cursor == 2048 && s.pending == 4);
    assert(lduw_le_p(header+0xc) == 1024);
    for(unsigned i=0;i<2048;i++) assert(header[0x100+i] == 0);
    amc_decoder_close(&s);
    /* MPEG-1 Layer III, 128 kbps, 44.1 kHz stereo; zero side information
     * describes silent granules. The output stride differs from AAC. */
    uint8_t mp3[417] = {0xff,0xfb,0x90,0x64};
    s.regs[0x940/4]=0x84004600; s.regs[0x960/4]=0xc6005c00;
    s.regs[0x964/4]=0x8544602f;
    stw_le_p(aperture+0x2ff00,1);
    memcpy(dram+0x1000,mp3,sizeof(mp3)); word(4,sizeof(mp3)<<16);
    assert(amc_decode_dma(&s,0x08000001));
    amc_decode_drain(&s); d=s.decoder;
    assert(d && d->pcm->len == 4608 && d->capacity == 4608);
    memset(header,0,0x12); s.pending=0; d->slot=1;
    amc_decode_publish(&s);
    assert(d->cursor == 4608 && lduw_le_p(header+0x10) == 2304);
    for(unsigned i=0;i<4608;i++) assert(header[0x1300+i] == 0);
    amc_decoder_close(&s);
    /* ALAC uncompressed mono element with three samples. A short final frame
     * reports its actual size, without padding to the 4096-frame capacity. */
    const uint8_t alac[] = {0,0,0x12,0,0,0,6,9,0xa5,0xed,0xae,0xff,0xff,0xc0};
    s.regs[0x940/4]=0x84007e00; s.regs[0x944/4]=0x85808240;
    s.regs[0x960/4]=0xc6008800; s.regs[0x964/4]=0x84d49848;
    stw_le_p(aperture+0x2ff00,0x1f); stw_le_p(aperture+0x2ff04,16);
    stw_le_p(aperture+0x2ff06,40); stw_le_p(aperture+0x2ff08,14);
    stw_le_p(aperture+0x2ff0a,10);
    memcpy(dram+0x1000,alac,sizeof(alac)); word(4,sizeof(alac)<<16);
    assert(amc_decode_dma(&s,0x08000001));
    amc_decode_drain(&s); d=s.decoder;
    assert(d && d->pcm->len == 6 && d->capacity == 16384);
    memset(header,0,0x12); s.pending=0;
    amc_decode_publish(&s);
    assert(d->cursor == 6 && lduw_le_p(header+0xc) == 3);
    assert(d->slot == 0 && lduw_le_p(header+2) == 1);
    assert(lduw_le_p(header+0x100) == 1234);
    assert((int16_t)lduw_le_p(header+0x102) == -2345);
    assert(lduw_le_p(header+0x104) == 32767);
    /* The next output must reuse the first region, leaving parameters intact. */
    s.pending=0; stw_le_p(header+0xa,0);
    assert(amc_decode_dma(&s,0x08000001)); amc_decode_drain(&s);
    amc_decode_publish(&s);
    assert(d->slot == 0 && lduw_le_p(header+0x100) == 1234);
    assert(lduw_le_p(aperture+0x2ff00) == 0x1f);
    amc_decoder_close(&s);
    /* Original HE-AAC silence fixture: LC core plus SBR FIL data. */
    const uint8_t heaac[] = {0x21,0,3,0x40,0x68,0x1b,0x77,0xdb,0,0x84,
                            0,0,0,0,0x0d,0x18,0,0x0c,0,0x38};
    s.regs[0x944/4]=0xa000ac40; s.regs[0x960/4]=0xc6008000;
    s.regs[0x964/4]=0x84fc8241; s.regs[0x968/4]=0xc600c043;
    s.regs[0x96c/4]=0x8464e464; s.regs[0x970/4]=0xbf212e73;
    s.regs[0x974/4]=0xc013f7fb;
    stw_le_p(aperture+0x2ff04,0); stw_le_p(aperture+0x2ff06,22050);
    stw_le_p(aperture+0x2ff08,0); stw_le_p(aperture+0x2ff0a,0);
    memcpy(dram+0x1000,heaac,sizeof(heaac)); word(4,sizeof(heaac)<<16);
    assert(amc_decode_dma(&s,0x08000001));
    amc_decode_drain(&s); d=s.decoder;
    assert(d && d->rate == 44100 && d->pcm->len == 8192);
    memset(header,0,0x12); s.pending=0;
    amc_decode_publish(&s);
    assert(d->cursor == 8192 && d->slot == 1 && lduw_le_p(header+0xc) == 4096);
    for(unsigned i=0;i<8192;i++) assert(header[0x100+i] == 0);
    amc_decoder_close(&s);
    /* The legacy guest negotiates HE-AAC v2's mono core. Do not hand its
     * mono output stream interleaved PS stereo (which doubles its duration). */
    const uint8_t ps[] = {0,0xd0,0,6,0xdd,0xf6,0xc1,0x3c,0x10,0,0,0,0,3,
                          0x84,0xa8,0x20,0x0e};
    /* Offline AudioQueue can request the LC program for the same HE file.
     * The physical 7E18 decoder accepts it and retains core rate/channels. */
    uint32_t he_program[14];
    memcpy(he_program,s.regs+0x940/4,sizeof(he_program));
    s.regs[0x940/4]=0x84006e00;s.regs[0x960/4]=0xc600b800;
    s.regs[0x964/4]=0x848cba5d;s.regs[0x968/4]=0xc013f7fb;
    stw_le_p(aperture+0x2ff00,7);stw_le_p(aperture+0x2ff06,7);
    memcpy(dram+0x1000,ps,sizeof(ps));word(4,sizeof(ps)<<16);
    assert(amc_decode_dma(&s,0x08000001));amc_decode_drain(&s);d=s.decoder;
    assert(d && !d->failed && d->rate==22050 && d->channels==1 && d->pcm->len==2048);
    amc_decoder_close(&s);
    memcpy(s.regs+0x940/4,he_program,sizeof(he_program));
    stw_le_p(aperture+0x2ff00,0x1f);stw_le_p(aperture+0x2ff06,22050);
    memcpy(dram+0x1000,ps,sizeof(ps)); word(4,sizeof(ps)<<16);
    assert(amc_decode_dma(&s,0x08000001)); amc_decode_drain(&s); d=s.decoder;
    assert(d && d->rate == 44100 && d->channels == 1 && d->pcm->len == 4096);
    /* Preserve decoded output before reporting a bad subsequent input. Both
     * normal and error completions obey the same ownership/ACK contract. */
    memset(header,0,0x12); s.pending=0;
    s.regs[0x100/4]=0x08000001;
    amc_decode_fail(&s);
    assert(d->failed && !d->codec && !d->frame && !d->input_pending);
    assert(!s.pending && d->dma_pending && !s.regs[0x100/4]);
    amc_decode_publish(&s);
    assert(!d->error_reported && d->cursor == 4096 && d->slot == 1);
    assert(!lduw_le_p(aperture+0x2ff28));
    amc_decode_publish(&s); assert(!d->error_reported);
    s.pending=0; stw_le_p(header+0xe,1);
    amc_decode_publish(&s); assert(!d->error_reported);
    stw_le_p(header+0xe,0);
    memset(header+0x2100,0xa5,8192);
    amc_decode_tick(&s);
    assert(d->error_reported && !d->dma_pending && s.pending == 0x40004);
    assert(lduw_le_p(aperture+0x2ff28) == 1);
    assert(lduw_le_p(aperture+0x2ff2a) == 100);
    for(unsigned i=0;i<8192;i++) assert(header[0x2100+i] == 0);
    s.pending=0; memset(header,0,0x12);
    amc_decode_publish(&s); assert(!s.pending); /* exactly one error */
    s.codec_decode=s.state_handshake=true;
    s.pending=s.int_mask[0]=0x40004;
    ipod_touch_amc_write(&s,0x110,0x20,4);
    assert(s.pending == 4 && irq_level);
    ipod_touch_amc_write(&s,AMC_INT_ACK,4,4);
    assert(!s.pending && !irq_level);
    s.pending=4;
    ipod_touch_amc_write(&s,AMC_INT_DISABLE,4,4);
    assert(s.pending == 4 && !irq_level);
    ipod_touch_amc_write(&s,AMC_INT_ENABLE,4,4);
    assert(irq_level);
    s.regs[0x100/4]=0x08000001;
    ipod_touch_amc_write(&s,AMC_JOB_CMD,3,4);
    assert(!s.pending && !irq_level && !s.regs[0x100/4] && timer_starts == 2);
    assert(!s.decoder && !lduw_le_p(aperture+0x2ff28));
    assert(!lduw_le_p(aperture+0x2ff2a));
    for(unsigned i=0;i<0x12;i++) assert(!header[i]);
    assert(amc_decode_dma(&s,0x08000001)); amc_decode_drain(&s);
    d=s.decoder; assert(d && !d->failed && d->pcm->len == 4096);
    amc_decoder_close(&s);
    /* Invalid first DMA has no allocated codec but must still complete. */
    assert(!amc_decode_dma(&s,0)); amc_decode_fail(&s);
    amc_decode_tick(&s);
    d=s.decoder; assert(d->error_reported && s.pending == 0x40004);
    amc_decoder_close(&s);
    /* End-of-input cannot overtake PCM still queued inside the emulator. */
    s.pending=0;memset(header,0,0x12);
    s.regs[0x940/4]=0x84006e00;s.regs[0x960/4]=0xc600b800;
    s.regs[0x964/4]=0x848cba5d;s.regs[0x968/4]=0xc013f7fb;
    stw_le_p(aperture+0x2ff00,7);stw_le_p(aperture+0x2ff06,4);
    for(unsigned i=0;i<3;i++)memcpy(dram+0x1000+i*sizeof(silence),silence,sizeof(silence));
    word(4,(3*sizeof(silence))<<16);s.regs[0x100/4]=0x08000001;
    for(unsigned i=0;i<3;i++) {
        amc_decode_tick(&s);
        assert(s.pending == (i==2 ? 0x40004 : 4));
        d=s.decoder;assert(d->dma_pending == (i!=2));
        s.pending=0;stw_le_p(header+0xa,0);stw_le_p(header+0xe,0);
    }
    amc_decode_tick(&s);assert(!s.pending);
    amc_decoder_close(&s);
    puts("PASS: AAC-LC/HE-AAC/MP3/ALAC, PCM layout/backpressure, DMA bounds and stream restart");
}
'''
with tempfile.TemporaryDirectory(prefix='amc-aac-') as directory:
    main = Path(directory) / 'check.c'
    exe = Path(directory) / 'check'
    main.write_text(constants + "\n" + prelude + code + mmio + check)
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'glib-2.0', 'libavcodec', 'libavutil'], text=True))
    rpaths = [f'-Wl,-rpath,{flag[2:]}' for flag in flags if flag.startswith('-L')]
    subprocess.run(['clang', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', str(main),
                    *flags, *rpaths, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
