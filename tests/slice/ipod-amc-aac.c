/* Exercise the real gated audio DMA/PCM code. Requires glib and libavcodec.
 *
 * Uses original silent/short codec packets and synthetic PCM to check the hardware
 * buffer contract independently of any guest app or copyrighted media asset.
 *
 * SLICE include/hw/arm/ipod_touch_amc.h define AMC_
 * SLICE:amc hw/arm/ipod_touch_amc.c range typedef enum AMCProgram | \n#else
 * SLICE:amc hw/arm/ipod_touch_amc.c fn amc_update_irq amc_ctrl_of
 * SLICE:amc hw/arm/ipod_touch_amc.c range static const uint32_t amc_banks[] | \nstatic int amc_bank_cmd
 * SLICE:amc hw/arm/ipod_touch_amc.c fn amc_bank_cmd amc_engine0 ipod_touch_amc_write amc_decode_tick
 * SLICE:amc hw/arm/ipod_touch_amc.c range static int amc_put_decoder( | static const VMStateInfo vmstate_amc_decoder
 * SLICE:amc hw/arm/ipod_touch_amc.c fn amc_pre_save
 * PKG glib-2.0 libavcodec libavutil
 */
#include "slice.h"
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
    bool codec_decode, state_handshake, irq_armed; int irq; uint64_t buf_base;
    uint32_t dram_base, dram_size; bool rev21; uint32_t result_offset, dma_done;
    uint32_t xfers, xfer[AMC_XFER_QUEUE * 3], e0_resume; uint8_t port[AMC_PORT_BYTES];
    uint32_t port_len; void (*port_kick)(void *); void *port_opaque;
} IPodTouchAMCState;
static void ipod_touch_amc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);
#define IPOD_TOUCH_AMC(s) ((IPodTouchAMCState *)(s))
#define AMC_REG(off) (s->regs[(off)/4])
static bool irq_level;
static unsigned timer_starts;
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int clock) { return 1000; }
static void timer_mod(void *timer, int64_t ns) {
    assert(ns == 1001000); timer_starts++;
}
static bool timer_pending(void *timer) { return true; }   /* the tick is running, as during a stream */
static void qemu_set_irq(int irq, bool level) { irq_level = level; }
static void amc_log_caller(hwaddr addr, uint32_t value) {}
static void amc_write_result_block(IPodTouchAMCState *s) {}
#define MEMTXATTRS_UNSPECIFIED 0
#define AMCT(...) ((void)0)
static bool amc_trace(void) { return false; }
#define warn_report(...) fprintf(stderr, __VA_ARGS__)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define le32_to_cpu(x) GUINT32_FROM_LE(x)
static unsigned char dram[65536], aperture[AMC_APERTURE_21];
static int address_space_memory;
static void stw_le_p(void *p, uint16_t v) { v = GUINT16_TO_LE(v); memcpy(p, &v, 2); }
static void stl_be_p(void *p, uint32_t v) { v = GUINT32_TO_BE(v); memcpy(p, &v, 4); }
static uint16_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v,p,2); return GUINT16_FROM_LE(v); }
static uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v,p,4); return GUINT32_FROM_LE(v); }
static void stl_le_p(void *p, uint32_t v) { v = GUINT32_TO_LE(v); memcpy(p, &v, 4); }
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
#include "amc.h"
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
/* Locally generated 733 Hz AAC-LC tone: ffmpeg sine, 44.1 kHz mono, 16 kb/s. */
static const unsigned tone_sizes[] = {69,89,51,49,54,57,5};
static const uint8_t tone[] = {
    0xdc,0x00,0x4c,0x61,0x76,0x63,0x36,0x33,0x2e,0x31,0x2e,0x31,0x30,0x31,0x00,0x02,0x90,0x97,0x25,0xca,0x66,0xf7,0xe3,0x9d,
    0x7b,0x7c,0x6b,0x85,0xcb,0x85,0xc0,0x12,0x12,0x12,0x12,0x13,0x4e,0x9d,0xb4,0x95,0x41,0xd1,0x1b,0x37,0x5c,0x18,0x19,0x12,
    0x20,0x63,0x66,0xcd,0x83,0x03,0x22,0x06,0x06,0x36,0x6e,0x59,0x65,0x52,0xa5,0x96,0x59,0x65,0x96,0x59,0x78,0x01,0x28,0x8c,
    0xda,0xc9,0x5b,0x25,0x6c,0x95,0xb2,0x95,0xff,0xd7,0xff,0x7f,0x6b,0xe3,0xfa,0x7f,0xe3,0xff,0x7f,0xae,0x2f,0xfa,0xff,0xf8,
    0x7f,0xef,0xed,0xd6,0xbf,0xb7,0xfd,0x3f,0xf3,0xf6,0xe2,0xc2,0x6e,0xed,0x23,0x50,0x6a,0x00,0x51,0x86,0xf4,0x40,0xbe,0x04,
    0x40,0x19,0x50,0xd1,0x3f,0xca,0x89,0xfe,0x46,0x0a,0x1f,0x25,0x0b,0x87,0x09,0xb9,0xd9,0x57,0x63,0x31,0x81,0x13,0x22,0x26,
    0x71,0x27,0xb8,0xc4,0x41,0xe0,0x37,0x7c,0xa9,0xa0,0x90,0x90,0x9c,0xb8,0x01,0x28,0xeb,0x8a,0xea,0x9e,0x3b,0xca,0xf1,0xfe,
    0xbf,0xbf,0xc7,0x9f,0x69,0xd3,0x86,0xb5,0x6b,0x0f,0xfc,0x36,0xef,0xbf,0xbf,0xba,0x5a,0x2f,0x78,0x74,0x01,0xf1,0x98,0x0c,
    0xfe,0xe0,0x3f,0xf0,0x03,0xbe,0xe4,0x1f,0xf8,0xcc,0x33,0xfb,0xc2,0x07,0xc6,0x60,0x38,0x00,0xf6,0x2b,0x89,0x42,0xd5,0xd3,
    0x7c,0xfe,0x7f,0xfe,0xcf,0xf6,0xfd,0xff,0xfc,0xbf,0x5f,0xbc,0xce,0x7c,0xd7,0x32,0x55,0x8f,0x91,0x7c,0xa2,0x5e,0xe7,0x52,
    0xb2,0xd4,0x81,0xbe,0xf7,0xfd,0xae,0xc3,0x4a,0x2b,0x44,0x53,0x98,0xc8,0x4e,0xd7,0x72,0x8f,0x01,0x2e,0x4b,0x8a,0xee,0x3c,
    0x64,0xf1,0xeb,0xe7,0xf4,0xfb,0xfc,0x7d,0xfd,0xa5,0xea,0x5c,0xbb,0xb8,0x3e,0x75,0xf8,0x7d,0xb6,0xc9,0xd8,0x95,0x97,0xd8,
    0x5c,0x3e,0x7f,0x38,0xc6,0x1b,0x6e,0x12,0x7c,0xe3,0x03,0xec,0x41,0x3f,0x33,0x03,0xfd,0xb6,0x84,0xfc,0xf7,0x83,0xfd,0xb8,
    0x01,0x24,0x8d,0xb2,0xc8,0xdb,0x23,0x43,0x06,0x7f,0xfd,0xaf,0xfd,0x7e,0x38,0x9e,0x3f,0xfe,0xdf,0xfc,0xdf,0x10,0x2f,0xd2,
    0x0f,0x73,0xe4,0x07,0x2c,0xed,0x1a,0x10,0x45,0x7d,0x97,0xdf,0xb1,0xbe,0x22,0xe7,0x6d,0xbd,0x05,0xb3,0xf6,0x3f,0xba,0xed,
    0xc8,0xc4,0x64,0x3d,0x66,0x30,0xf7,0x6c,0xe0,0x01,0x18,0x81,0xb4,0x70,
};
int main(void) {
    IPodTouchAMCState s = {.buf_base = AMC_BUF_BASE, .dram_base = 0x08000000, .dram_size = 0x08000000,
                           .result_offset = AMC_RESULT_OFFSET, .dma_done = AMC_DMA_DONE}; /* the iPod's defaults */
    assert(amc_program(&s) == AMC_UNKNOWN);
    s.regs[0x940/4]=0x84006e00; s.regs[0x960/4]=0xc600b800;
    s.regs[0x964/4]=0x848cba5d; s.regs[0x968/4]=0xc013f7fb;
    assert(!amc_dram(&s, 0x07ffffff, 1));
    assert(!amc_dram(&s, 0x10000000, 0));
    assert(!amc_dram(&s, 0x0fffffff, 2));
    assert(amc_dram(&s, 0x0fffffff, 1));
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
    /* AMC 2.1 (iPad 1, 7B500): the result block and parameters 64 KiB lower,
     * input from the engine's own memory (descriptor tagged 2, bounce buffer
     * at +0x20400), and each frame collected by an engine 0 list into the
     * output port, as the driver builds them (lists captured from the guest). */
    memset(aperture, 0, sizeof(aperture));
    IPodTouchAMCState a = {.buf_base = AMC_BUF_BASE, .rev21 = true, .codec_decode = true,
                           .result_offset = AMC_RESULT_OFFSET_21, .dma_done = AMC_DMA_DONE_21};
    a.int_mask[0] = AMC_E0_DONE | AMC_DMA_DONE_21 | 0x1000;      /* the sources 3.2 and later enable */
    a.regs[0x940/4]=0x84006e00; a.regs[0x960/4]=0xc600b800;
    a.regs[0x964/4]=0x848cba5d; a.regs[0x968/4]=0xc013f7fb;
    stw_le_p(aperture+0x1ff00,7); stw_le_p(aperture+0x1ff06,4);
    memcpy(aperture+0x20400,tone,tone_sizes[0]+tone_sizes[1]+tone_sizes[2]);
    stl_le_p(aperture+0x20004,(tone_sizes[0]+tone_sizes[1]+tone_sizes[2])<<16 | 0x11);
    stl_le_p(aperture+0x20008,0x20400);
    a.regs[0x100/4]=0x20002;
    amc_decode_tick(&a);
    d=a.decoder; assert(d && d->pcm->len && !a.regs[0x100/4]);
    assert(!lduw_le_p(aperture+AMC_RESULT_OFFSET+0xa));      /* nothing at the 2.0 block */
    assert(!a.port_len && !(a.pending & 4));                   /* no frame without a job */
    /* iOS 7's shape (n90ap 11D257): job, commit, completion, the slot status copied out (to 0x344d0), then
     * cleared. 7B500's has no copy-out. Engine 0 runs it in order: what follows the job runs once the frame moved. */
    static const uint32_t list[] = {
        0x34438,0x00140101,0x3448c,0x303060, 0x34454,0x00040001,0x344a0,0x303060,
        0x34470,0x00040009,0x344a4,0xc48,    0x34700,0x00080005,0x18008,0x344d0 };
    for (unsigned i=0;i<16;i++) stl_le_p(aperture+0x3441c+(i/4)*0x1c+(i%4)*4,list[i]);
    static const uint32_t clear[] = { 0,0x00080005,0x344a8,0x18008 };
    for (unsigned i=0;i<4;i++) stl_le_p(aperture+0x34700+i*4,clear[i]);
    static const uint32_t job[] = { 0, 4096, 0x18100, 0, AMC_PORT_LOCAL_21, 3, 4, 0, 0 };
    for (unsigned i=0;i<9;i++) stl_le_p(aperture+0x3448c+i*4,job[i]);
    ipod_touch_amc_write(&a, AMC_E0_HEAD, 0x3441e, 4);
    assert(a.xfers == 1 && !a.port_len);
    amc_decode_tick(&a);
    assert(!a.xfers && a.port_len == 4096);
    assert(!memcmp(a.port, aperture+0x18100, 4096));
    assert(lduw_le_p(aperture+0x344d0+4) == 1024);             /* the copied-out 2.1 block: this frame's */
    assert(!lduw_le_p(aperture+0x18000+0xc) && !a.e0_resume);  /* then cleared; the list is done */
    unsigned loud=0; for (unsigned i=0;i<4096;i++) loud+=a.port[i]!=0;
    assert(loud>100);
    /* iOS 7 (n90ap 11D257) chains several frames' commands into one list, 16 links and more: every link runs. */
    for (unsigned i=0;i<18;i++) {
        uint32_t at=0x34600+i*0x1c;
        stl_le_p(aperture+at, i<17 ? at+0x1c : 0);
        stl_le_p(aperture+at+4,0x00040005);                    /* 4 bytes to engine-local memory */
        stl_le_p(aperture+at+8,0x34800);
        stl_le_p(aperture+at+12,0x34900+i*4);
    }
    stl_le_p(aperture+0x34800,0x5a5a0000);
    ipod_touch_amc_write(&a, AMC_E0_HEAD, 0x34602, 4);
    assert(ldl_le_p(aperture+0x34900+17*4)==0x5a5a0000);       /* the 18th link ran */
    amc_decoder_close(&a);
    /* The parameter block names the codec, not the DE's memory windows. */
    uint8_t block[12] = {0x1f,0,0,0,0,0,0x44,0xac,0,0,0,0};    /* n90ap-10B329's ringtone */
    assert(amc_aac_rate(block) == 4);
    block[6] = 0x22; block[7] = 0x56; assert(amc_aac_rate(block) == 7);           /* 22050 */
    block[10] = 1; assert(amc_aac_rate(block) == -1); block[10] = 0;
    block[0] = 7; block[6] = 4; block[7] = 0; assert(amc_aac_rate(block) == 4);  /* 7E18's LC form */
    block[6] = 13; assert(amc_aac_rate(block) == -1);
    const uint8_t alac_block[12] = {0x1f,0,0,0,16,0,40,0,14,0,10,0};
    assert(amc_aac_rate(alac_block) == -1);
    block[0] = 1; assert(amc_aac_rate(block) == -1);                             /* MP3's */
    /* iOS 6's AAC program (n90ap-10B329, n88ap-10B500): its own windows, the rate in Hz, 8 KiB slots
     * of which a frame fills half. It decodes, and a frame goes to the slot its job names. */
    memset(aperture, 0, sizeof(aperture));
    IPodTouchAMCState b = {.buf_base = AMC_BUF_BASE, .rev21 = true, .codec_decode = true,
                           .result_offset = AMC_RESULT_OFFSET_21, .dma_done = AMC_DMA_DONE_21};
    b.int_mask[0] = 0x111000;                                   /* as 10B329 enables them */
    static const uint32_t ios6[] = { 0x83007020,0,0,0,0,0,0,0, 0xc6000000,0x85fc4001,0x85fc4221,
                                     0xc6807823,0x8554a844,0x8220d455,0,0xc013f7fb };
    memcpy(b.regs+0x940/4, ios6, sizeof(ios6));
    assert(amc_program(&b) == AMC_UNKNOWN && amc_output_capacity(amc_program(&b)) == 4096);
    stw_le_p(aperture+0x1ff00,0x1f); stw_le_p(aperture+0x1ff06,44100);
    unsigned tone3 = tone_sizes[0]+tone_sizes[1]+tone_sizes[2];
    memcpy(aperture+0x20400,tone,tone3);
    stl_le_p(aperture+0x20004,tone3<<16 | 0x11); stl_le_p(aperture+0x20008,0x20400);
    b.regs[0x100/4]=0x20002;
    amc_decode_tick(&b);
    d=b.decoder; assert(d && !d->failed && d->rate == 44100 && d->pcm->len >= 2048);
    assert(!b.port_len);                                        /* no frame without a job */
    static const uint32_t list6[] = { 0x34420,0x00140101,0x3448c,0x303060, 0,0x00040001,0x344a0,0x303060 };
    for (unsigned i=0;i<8;i++) stl_le_p(aperture+0x34404+(i/4)*0x1c+(i%4)*4,list6[i]);
    static const uint32_t job6[] = { 0, 0x2000, 0x1a100, 0, AMC_PORT_LOCAL_21, 3 };
    for (unsigned i=0;i<6;i++) stl_le_p(aperture+0x3448c+i*4,job6[i]);
    ipod_touch_amc_write(&b, AMC_E0_HEAD, 0x34406, 4);
    assert(b.xfers == 1);
    unsigned frame = GPOINTER_TO_UINT(g_queue_peek_head(&d->output_sizes));
    amc_decode_tick(&b);
    assert(!b.xfers && b.port_len == 0x2000);                  /* the whole second slot */
    assert(lduw_le_p(aperture+0x18000+0x10) == frame/2);       /* its count: one frame */
    assert(!memcmp(b.port, aperture+0x1a100, frame) && d->cursor == frame);
    amc_decoder_close(&b);
    /* 3.1 on the 3GS (n88ap-7E18) runs AMC 2.1 without engine 0: it enables the DE's completion and
     * takes each frame from its slot, as AMC 2.0 does. */
    memset(aperture, 0, sizeof(aperture));
    b.int_mask[0] = 0x101004; b.xfers = b.port_len = 0; b.pending = 0;
    memcpy(b.regs+0x940/4, (uint32_t[]){0x84006e00}, 4); memset(b.regs+0x944/4, 0, 0x38);
    b.regs[0x960/4]=0xc600b800; b.regs[0x964/4]=0x848cba5d; b.regs[0x968/4]=0xc013f7fb;
    assert(amc_program(&b) == AMC_AAC);
    stw_le_p(aperture+0x1ff00,7); stw_le_p(aperture+0x1ff06,4);
    memcpy(aperture+0x20400,tone,tone3);
    stl_le_p(aperture+0x20004,tone3<<16 | 0x11); stl_le_p(aperture+0x20008,0x20400);
    b.regs[0x100/4]=0x20002;
    amc_decode_tick(&b);
    d=b.decoder; assert(d && d->pcm->len && (b.pending & 4) && !b.port_len);
    assert(lduw_le_p(aperture+0x18000+0xa) == 1 && lduw_le_p(aperture+0x18000+0xc));
    amc_decoder_close(&b);
    puts("PASS: iOS 6's AAC program by its parameter block, slots of its jobs' size; 3.1's AMC 2.1 collection");
    puts("PASS: AMC 2.1 layout, engine-local input and engine 0 collection into the output port");
    puts("PASS: exact AMC replay, pending frames/PCM/slot/completion and bounded history");
    puts("PASS: AAC-LC/HE-AAC/MP3/ALAC, PCM layout/backpressure, DMA bounds and stream restart");
}
