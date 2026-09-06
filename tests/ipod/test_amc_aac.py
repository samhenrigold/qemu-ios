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
serialization = source[source.index('static int amc_put_decoder('):source.index('static const VMStateInfo vmstate_amc_decoder')]
serialization += '\n' + function('amc_pre_save(')
helpers = r'''
static AMCDecoder *snapshot_decoder(AMCDecoder *d)
{
    QEMUFile wire = { .bytes = g_byte_array_new() };
    AMCDecoder *copy = NULL;
    assert(amc_put_decoder(&wire, &d, 0, NULL, NULL) == 0);
    assert(amc_get_decoder(&wire, &copy, 0, NULL) == 0);
    assert(wire.pos == wire.bytes->len);
    assert(amc_replay_restore(copy));
    g_byte_array_unref(wire.bytes);
    return copy;
}
static GByteArray *finish_stream(IPodTouchAMCState *s)
{
    GByteArray *pcm = g_byte_array_new();
    for (unsigned tick = 0; tick < 2000; tick++) {
        AMCDecoder *d = s->decoder;
        if (s->pending & 4) {
            unsigned slot = (d->slot + d->buffers - 1) % d->buffers;
            uint8_t *header = aperture + AMC_RESULT_OFFSET;
            unsigned bytes = lduw_le_p(header + 0xc + slot * 4) * 2;
            assert(lduw_le_p(header + 0xa + slot * 4) && bytes);
            g_byte_array_append(pcm, header + 0x100 + slot * d->capacity, bytes);
            stw_le_p(header + 0xa + slot * 4, 0);
            s->pending &= ~4u;
        }
        if (s->pending & 0x40000) {
            assert(!d->input_pending && !d->dma_pending && g_queue_is_empty(&d->output_sizes));
            return pcm;
        }
        amc_decode_tick(s);
    }
    assert(0); return NULL;
}
'''

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
#include <errno.h>
typedef struct { GByteArray *bytes; size_t pos; int error; } QEMUFile;
typedef void VMStateField;
typedef void JSONWriter;
static void qemu_put_be32(QEMUFile *f, uint32_t v) { v = GUINT32_TO_BE(v); g_byte_array_append(f->bytes, (uint8_t *)&v, 4); }
static void qemu_put_buffer(QEMUFile *f, const uint8_t *p, size_t n) { if(n) g_byte_array_append(f->bytes, p, n); }
static size_t qemu_get_buffer(QEMUFile *f, uint8_t *p, size_t n) {
 if (n > f->bytes->len-f->pos) { f->error = -EIO; return 0; }
 if(n) memcpy(p, f->bytes->data+f->pos, n); f->pos += n; return n;
}
static uint32_t qemu_get_be32(QEMUFile *f) { uint32_t v=0; qemu_get_buffer(f, (uint8_t *)&v, 4); return GUINT32_FROM_BE(v); }
static int qemu_file_get_error(QEMUFile *f) { return f->error; }
#define error_report(...) fprintf(stderr, __VA_ARGS__)
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

# Locally generated 733 Hz AAC-LC tone: ffmpeg sine, 44.1 kHz mono, 16 kb/s.
tone_packets = [
    bytes.fromhex('dc004c61766336332e312e3130310002909725ca66f7e39d7b7c6b85cb85c012121212134e9db49541d11b375c181912206366cd8303220606366e596552a5965965965978'),
    bytes.fromhex('01288cdac95b256c95b295ffd7ff7f6be3fa7fe3ff7fae2ffafff87fefedd6bfb7fd3ff3f6e2c26eed23506a005186f440be04401950d13fca89fe460a1f250b8709b9d9576331811322267127b8c441e0377ca9a090909cb8'),
    bytes.fromhex('0128eb8aea9e3bcaf1febfbfc79f69d386b56b0ffc36efbfbfba5a2f787401f1980cfee03ff003bee41ff8cc33fbc207c66038'),
    bytes.fromhex('00f62b8942d5d37cfe7ffecff6fdfffcbf5fbcce7cd732558f917ca25ee752b2d481bef7fdaec34a2b445398c84ed7728f'),
    bytes.fromhex('012e4b8aee3c64f1ebe7f4fbfc7dfda5ea5cbbb83e75f87db6c9d89597d85c3e7f38c61b6e127ce303ec413f3303fdb684fcf783fdb8'),
    bytes.fromhex('01248db2c8db2343067ffdaffd7e389e3ffedffcdf102fd20f73e4072ced1a10457d97dfb1be22e76dbd05b3f63fbaedc8c4643d6630f76ce0'),
    bytes.fromhex('011881b470'),
]
tone_code = 'static const unsigned tone_sizes[] = {' + ','.join(str(len(v)) for v in tone_packets) + '};\n'
tone_code += 'static const uint8_t tone[] = {' + ','.join(str(v) for v in b''.join(tone_packets)) + '};\n'

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
    unsigned saved_timer_starts = timer_starts;
    bool saved_irq_level = irq_level;
    d=s.decoder;
    amc_decode_drain(&s); d->dma_pending = true;
    memset(aperture+AMC_RESULT_OFFSET, 0, 0x12); s.pending = 0;
    amc_decode_publish(&s);
    assert(d->input_pending && d->cursor == 4096 && d->slot == 1 && s.pending == 4);
    IPodTouchAMCState restored = s; restored.decoder = snapshot_decoder(d);
    uint8_t *saved_aperture = g_memdup2(aperture, sizeof(aperture));
    GByteArray *expected = finish_stream(&s);
    memcpy(aperture, saved_aperture, sizeof(aperture)); g_free(saved_aperture);
    GByteArray *actual = finish_stream(&restored);
    assert(expected->len == 1000 * 4096 && actual->len == expected->len);
    assert(!memcmp(expected->data, actual->data, expected->len));
    assert(((AMCDecoder *)restored.decoder)->slot == d->slot);
    g_byte_array_unref(expected); g_byte_array_unref(actual); amc_decoder_close(&restored);
    amc_decoder_close(&s); s.pending = 0;
    timer_starts = saved_timer_starts; irq_level = saved_irq_level;
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
    /* Replay prior non-silent packets to recover AAC overlap, then compare
     * every sample of the following packets against uninterrupted decoding. */
    memset(s.regs,0,sizeof(s.regs));
    s.regs[0x940/4]=0x84006e00; s.regs[0x960/4]=0xc600b800;
    s.regs[0x964/4]=0x848cba5d; s.regs[0x968/4]=0xc013f7fb;
    stw_le_p(aperture+0x2ff00,7); stw_le_p(aperture+0x2ff06,4);
    unsigned tone_offset = 0;
    for (unsigned j=0; j<4; j++) {
        memcpy(dram+0x1000,tone+tone_offset,tone_sizes[j]); tone_offset += tone_sizes[j];
        word(4,tone_sizes[j]<<16); assert(amc_decode_dma(&s,0x08000001));
        amc_decode_drain(&s); d=s.decoder; assert(!d->input_pending);
        d->cursor=d->pcm->len; g_queue_clear(&d->output_sizes);
    }
    restored=s; restored.decoder=snapshot_decoder(d);
    unsigned nonzero=0;
    for (unsigned j=4; j<ARRAY_SIZE(tone_sizes); j++) {
        memcpy(dram+0x1000,tone+tone_offset,tone_sizes[j]); tone_offset += tone_sizes[j];
        word(4,tone_sizes[j]<<16); assert(amc_decode_dma(&s,0x08000001));
        assert(amc_decode_dma(&restored,0x08000001));
        amc_decode_drain(&s); amc_decode_drain(&restored);
        d=s.decoder; AMCDecoder *r=restored.decoder;
        assert(d->pcm->len && r->pcm->len==d->pcm->len);
        assert(!memcmp(d->pcm->data,r->pcm->data,d->pcm->len));
        for(unsigned k=0;k<d->pcm->len;k++) nonzero+=d->pcm->data[k]!=0;
        d->cursor=d->pcm->len; r->cursor=r->pcm->len;
        g_queue_clear(&d->output_sizes); g_queue_clear(&r->output_sizes);
    }
    assert(nonzero>100); amc_decoder_close(&restored); amc_decoder_close(&s);
    /* Truncated/malformed replay blobs reject before publishing a decoder. */
    QEMUFile bad={.bytes=g_byte_array_new()}; AMCDecoder *missing=NULL;
    qemu_put_be32(&bad,2); assert(amc_get_decoder(&bad,&missing,0,NULL)==-EINVAL && !missing);
    g_byte_array_set_size(bad.bytes,0); bad.pos=0; qemu_put_be32(&bad,1);
    assert(amc_get_decoder(&bad,&missing,0,NULL)==-EIO && !missing);
    g_byte_array_unref(bad.bytes);
    /* Cap rejection must not stop decoding and stream reset must clear it. */
    memcpy(dram+0x1000,silence,sizeof(silence)); word(4,sizeof(silence)<<16);
    assert(amc_decode_dma(&s,0x08000001)); amc_decode_drain(&s);
    d = s.decoder; d->history_bytes = AMC_REPLAY_MAX_BYTES;
    amc_replay_record(d, silence, sizeof(silence));
    assert(d->history_overflow && !d->history && amc_pre_save(&s) == -E2BIG);
    assert(amc_decode_dma(&s,0x08000001)); amc_decode_drain(&s);
    assert(!d->failed); amc_decoder_close(&s); assert(!amc_pre_save(&s));
    AMCDecoder limit={.history=g_ptr_array_new()};
    g_ptr_array_set_size(limit.history,AMC_REPLAY_MAX_PACKETS);
    amc_replay_record(&limit,silence,sizeof(silence));
    assert(limit.history_overflow && !limit.history);
    memset(&limit,0,sizeof(limit)); amc_replay_record(&limit,silence,sizeof(silence));
    limit.history_frames=AMC_REPLAY_MAX_FRAMES; amc_replay_received(&limit);
    assert(limit.history_overflow && !limit.history);
    puts("PASS: exact AMC replay, pending frames/PCM/slot/completion and bounded history");
    puts("PASS: AAC-LC/HE-AAC/MP3/ALAC, PCM layout/backpressure, DMA bounds and stream restart");
}
'''
with tempfile.TemporaryDirectory(prefix='amc-aac-') as directory:
    main = Path(directory) / 'check.c'
    exe = Path(directory) / 'check'
    main.write_text(constants + "\n" + prelude + code + mmio + serialization + helpers + tone_code + check)
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'glib-2.0', 'libavcodec', 'libavutil'], text=True))
    rpaths = [f'-Wl,-rpath,{flag[2:]}' for flag in flags if flag.startswith('-L')]
    subprocess.run(['clang', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', str(main),
                    *flags, *rpaths, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
