/* S5L8930 CDMA (hw/arm/s5l8930_cdma.c): inline-AES symmetry on device-FIFO
 * channels, the enabled-channel status registers, and the FIFO-drained signal.
 *
 * Driven through cdma_read/cdma_write with a small fake physical memory (the
 * descriptor walker's dma_memory_* and address_space_*), no emulator. Contracts:
 *   - The base NAND-media AES-128 layer is omitted from the page store,
 *     symmetrically in both directions. A custom AES-256 protected-file layer
 *     is additional and must be filtered in both directions. IV descriptors
 *     load its per-page IV; bypassing this layer returns ciphertext to the OS.
 *   - Global +0x10/+0x14 report which channels are enabled (set by +0x00/+0x04,
 *     cleared by +0x08/+0x0C), a status word, not a pending-interrupt bitmap.
 *   - A to-device chain that fills a paced device FIFO stays RUNNING until the
 *     device drains it (s5l8930_cdma_sink_done), then goes DONE.
 *   - A memory-to-memory channel interrupts when done only with control bit 3:
 *     iOS 7 starts the AES feeder with it (0x188) and turns the feeder's clock
 *     off only in its interrupt (a feeder left clocked panicked AppleCDMA's
 *     sleep check, LightTouchMac issue 83); 3.2 polls its AES pair (0x180,
 *     0x80) and panics on an interrupt ("CDMA M2M unexpected interrupt").
 *
 * Mutations: bypass the AES-256 file layer and the protected read/write
 * assertions fail; ignore an IV descriptor and the captured-IV assertions fail;
 * apply the AES-128 media transform and the clear-page assertions fail.
 *
 * SLICE include/hw/arm/ipod_touch_guard.h range static inline uint64_t it_guard_idx | #endif /* HW_ARM_IPOD_TOUCH_GUARD_H
 * SLICE hw/arm/s5l8930_cdma.c range #define CDMA_CHANNELS  | /* Per-IPSW GID stand-in
 * SLICE hw/arm/s5l8930_cdma.c define GID_|CDMA_PACED_[A-Z_]+_NS
 * SLICE hw/arm/s5l8930_cdma.c fn aes_trace_tag gid_lookup aes_apply fifo_push aes_feed aes_for_channel cdma_is_memory cdma_src cdma_fifo_fed cdma_update_irq fifo_pop cdma_waits_for_uart cdma_fifo_xfer cdma_run cdma_is_paced cdma_paced_advance cdma_paced_pos cdma_pace_arm cdma_pace_tick cdma_paced_stop cdma_start_paced cdma_read cdma_write s5l8930_cdma_sink_done s5l8930_cdma_set_source s5l8930_cdma_kick cdma_kick_deferred cdma_post_load
 */
#include <errno.h>
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define trace_s5l8930_aes_transfer(...) ((void)0)
#define trace_s5l8930_cdma_iv(...) ((void)0)
#define trace_s5l8930_cdma_uart(...) ((void)0)
#define trace_s5l8930_cdma_register(...) ((void)0)
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define g_assert_not_reached() abort()
typedef uint64_t hwaddr;
typedef int SysBusDevice, MemoryRegion, QEMUTimer, qemu_irq;
typedef int QEMUBH;
static bool bh_scheduled;
static void qemu_bh_schedule(QEMUBH *bh) { (void)bh; bh_scheduled = true; }
typedef struct DeviceState DeviceState;
#define S5L8930_CDMA(s) ((S5L8930CDMAState *)(s))
#define S5L8930_DRAM_BASE 0x40000000ULL
#define S5L8930_DRAM_SIZE 0x10000000ULL
#define S5L8930_CDMA_CHANNELS 0x26
#define S5L8930_I2S_BASE(n) (0x82000000ULL + (n) * 0x1000)
#define S5L8930_UART_BASE(n) (0x83000000ULL + (n) * 0x1000)
#define NANOSECONDS_PER_SECOND 1000000000LL
#define SCALE_MS 1000000
#define QEMU_CLOCK_VIRTUAL 0
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
#define LOG_UNIMP 1
#define LOG_GUEST_ERROR 2
#define HWADDR_PRIx "llx"
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define le32_to_cpu(x) (x)
#define qemu_log_mask(...) ((void)0)
static int irq_level[0x2c];   /* includes the A6 display channel */
static void qemu_set_irq(int irq, int v) { irq_level[irq] = v; }
typedef struct S5L8930CDMAState S5L8930CDMAState;
typedef int Error, GError;
typedef struct QCryptoCipher QCryptoCipher;
typedef int QCryptoCipherAlgo;
#define QCRYPTO_CIPHER_ALGO_AES_128 0
#define QCRYPTO_CIPHER_ALGO_AES_192 1
#define QCRYPTO_CIPHER_ALGO_AES_256 2
#define QCRYPTO_CIPHER_MODE_CBC 0
static void error_report_err(Error *e) { (void)e; }
#define warn_report(...) ((void)0)
static int qcrypto_cipher_get_key_len(int a) { (void)a; return 32; }
static QCryptoCipher *qcrypto_cipher_new(int a, int m, const uint8_t *k, size_t l, Error **e) {
    (void)a; (void)m; (void)k; (void)l; (void)e; static int dummy; return (QCryptoCipher *)&dummy;
}
static uint8_t cipher_iv[16], cipher_ivs[4][16];
static bool record_ivs;
static unsigned cipher_calls;
static int qcrypto_cipher_setiv(QCryptoCipher *c, const uint8_t *iv, size_t l, Error **e) { assert(l == 16); memcpy(cipher_iv, iv, l); if(record_ivs){assert(cipher_calls<4);memcpy(cipher_ivs[cipher_calls++],iv,16);} return 0; }
/* Visible identity-ish transform: only ever reached under the mutation here. */
static int qcrypto_cipher_encrypt(QCryptoCipher *c, const void *in, void *out, size_t len, Error **e) {
    for (size_t i = 0; i < len; i++) ((uint8_t *)out)[i] = ((const uint8_t *)in)[i] ^ 0xff; return 0;
}
static int qcrypto_cipher_decrypt(QCryptoCipher *c, const void *in, void *out, size_t len, Error **e) {
    for (size_t i = 0; i < len; i++) ((uint8_t *)out)[i] = ((const uint8_t *)in)[i] ^ 0xff; return 0;
}
static void qcrypto_cipher_free(QCryptoCipher *c) { (void)c; }
static void stl_le_p(void *p, uint32_t v) {
    uint8_t *b = p; b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}
static uint32_t s5l8930_i2s_sync_rate(int port) { (void)port; return 44100; }
static unsigned s5l8930_i2s_frame_bytes(unsigned port, bool input) { return port == 6 ? (input ? 8 : 2) : 4; }
static int64_t g_now;
static int64_t qemu_clock_get_ns(int c) { (void)c; return g_now; }
static void timer_mod(QEMUTimer *t, int64_t at) { (void)t; (void)at; }
static void timer_del(QEMUTimer *t) { (void)t; }
static void auto_free(void *p) { free(*(void **)p); }
#define g_autofree __attribute__((cleanup(auto_free)))
#define g_malloc(n) malloc(n)
#define g_realloc(p,n) realloc((p),(n))
#define g_free(p) free(p)

/* ---- fake physical memory: a DRAM window (dma_memory_*) and a device FIFO
 * register (address_space_*, the only user of it: cdma_fifo_xfer). A real FIFO
 * is one address, so writes append to a sink and reads pop from a source. ---- */
#define DRAM_WIN 0x10000
#define FIFO_A   0x81200014ULL
static uint8_t DRAM[DRAM_WIN];
static int address_space_memory;
static uint8_t fifo_sink[0x1000]; static unsigned fifo_sink_len;
static uint8_t fifo_src[0x1000];  static unsigned fifo_src_pos;
static void (*fifo_write_hook)(void);
static unsigned uart_available;
static uint32_t uart_avail(void *opaque, hwaddr addr, bool to_device)
{
    return to_device ? 0 : uart_available;
}
static uint8_t *mem_ptr(uint64_t addr, unsigned len) {
    if (addr >= S5L8930_DRAM_BASE && addr + len <= S5L8930_DRAM_BASE + DRAM_WIN)
        return DRAM + (addr - S5L8930_DRAM_BASE);
    fprintf(stderr, "mem_ptr: unmapped 0x%llx+%u\n", (unsigned long long)addr, len); abort();
}
static int dma_memory_read(void *as, uint64_t addr, void *buf, unsigned len, int attrs) {
    memcpy(buf, mem_ptr(addr, len), len); return MEMTX_OK;
}
static int dma_memory_write(void *as, uint64_t addr, const void *buf, unsigned len, int attrs) {
    memcpy(mem_ptr(addr, len), buf, len); return MEMTX_OK;
}
static void address_space_read(void *as, uint64_t addr, int attrs, void *buf, unsigned len) {
    if (addr == 0x32600024) {
        assert(len <= uart_available);
        uart_available -= len;
    }
    memcpy(buf, fifo_src + fifo_src_pos, len); fifo_src_pos += len;   /* device FIFO drain */
}
static void address_space_write(void *as, uint64_t addr, int attrs, const void *buf, unsigned len) {
    memcpy(fifo_sink + fifo_sink_len, buf, len); fifo_sink_len += len; /* device FIFO fill */
    if (fifo_write_hook) fifo_write_hook();
}
#include "slice.h"

struct DeviceState { int unused; };
#define CR(a)    cdma_read(s, (a), 4)
#define CW(a,v)  cdma_write(s, (a), (v), 4)
#define CHREG(ch,reg) (((ch) << CDMA_CHAN_SHIFT) | (reg))
static void wr32(uint64_t addr, uint32_t v) { stl_le_p(mem_ptr(addr, 4), v); }
static void setup_chan(S5L8930CDMAState *s, int ch, uint32_t settings, uint64_t fifo, uint64_t desc) {
    cdma_write(s, CHREG(ch, CH_SETTINGS), settings, 4);
    cdma_write(s, CHREG(ch, CH_FIFO), fifo, 4);
    cdma_write(s, CHREG(ch, CH_DESC), desc, 4);
}
#define DESC   (S5L8930_DRAM_BASE + 0x100)
#define SRC    (S5L8930_DRAM_BASE + 0x400)
#define DST    (S5L8930_DRAM_BASE + 0x800)
static uint32_t big_avail(void *o, hwaddr a, bool td) { (void)o; (void)a; (void)td; return ~0u; }
static S5L8930CDMAState *resume_state;
static uint32_t burst_avail(void *o, hwaddr a, bool td) { return 2048; }
static void drain_burst(void) {
    if (fifo_sink_len % 2048 == 0) {
        s5l8930_cdma_sink_done((DeviceState *)resume_state, FIFO_A, 4);
        s5l8930_cdma_kick((DeviceState *)resume_state);
    }
}
static uint32_t protected_avail(void *opaque, hwaddr addr, bool td) {
    return *(bool *)opaque ? 0 : 2048;
}
static void check_protected_resume(bool zero_first, bool to_device) {
    S5L8930CDMAState *state=calloc(1,sizeof *state);
    state->num_channels=S5L8930_CDMA_CHANNELS;
    state->nand_file_aes=true;state->a6=true;
    AESContext *aes=&state->aes[2];
    aes->setup=(1<<8)|AES_ENABLE|AES_KEY_CUSTOM|(2<<18)|(to_device?AES_ENCRYPT:0);
    uint8_t initial_iv[16];
    for(unsigned i=0;i<4;i++){aes->iv[i]=0x51423324+i*0x01010101;stl_le_p(initial_iv+4*i,aes->iv[i]);}
    memset(aes->chain,0xcc,sizeof aes->chain);
    bool blocked=zero_first;
    s5l8930_cdma_set_source((DeviceState *)state,FIFO_A,4,protected_avail,&blocked);
    for(unsigned i=0;i<4096;i++) {
        DRAM[SRC-S5L8930_DRAM_BASE+i]=(i*71)^(i>>8);
        fifo_src[i]=(i*53)^(i>>7);
    }
    fifo_sink_len=fifo_src_pos=0;cipher_calls=0;record_ivs=true;
    resume_state=state;fifo_write_hook=to_device?drain_burst:NULL;bh_scheduled=false;
    wr32(DESC,0);wr32(DESC+4,DESC_DATA|DESC_LAST|DESC_AES|DESC_AES_RESTART);
    wr32(DESC+8,to_device?SRC:DST);wr32(DESC+12,4096);
    setup_chan(state,1,to_device?SET_TO_DEVICE:0,FIFO_A,DESC);
    cdma_write(state,CHREG(1,CH_CTRL),CTRL_GO,4);
    if(blocked) {
        assert(!cipher_calls && state->ch[1].in_seg && state->ch[1].remain==4096);
        blocked=false;s5l8930_cdma_kick((DeviceState *)state);
    }
    assert(cipher_calls==1 && state->ch[1].remain==2048);
    assert(!memcmp(cipher_ivs[0],initial_iv,16));
    if(to_device){bh_scheduled=false;cdma_kick_deferred(state);}
    else s5l8930_cdma_kick((DeviceState *)state);
    assert(cipher_calls==2);
    const uint8_t *previous_cipher=to_device?fifo_sink+2032:fifo_src+2032;
    assert(!memcmp(cipher_ivs[1],previous_cipher,16));
    assert(memcmp(cipher_ivs[1],initial_iv,16));
    assert((state->ch[1].ctrl&ST_DONE) && !(state->ch[1].ctrl&ST_ERROR));
    record_ivs=false;fifo_write_hook=NULL;bh_scheduled=false;free(state);
}
/* a6: the A6's ring byte counter and pause (s5l8950.c sets "a6"); off, every board keeps its abort and BC. */
static void check_uart_receive(uint64_t base, unsigned ports, unsigned port,
                               unsigned version, bool a6)
{
    S5L8930CDMAState *s = calloc(1, sizeof *s);
    const unsigned ch = 16;
    const size_t dst_offset = DST - S5L8930_DRAM_BASE;
    uint32_t rx = base + ((uint64_t)port << 20) + 0x24;
    uint32_t budget = a6 ? 0x40000000 : 64;
    s->num_channels = 44;
    s->uart_base = base;
    s->uart_ports = ports;
    s->version = version;
    s->a6 = a6;
    for (unsigned i = 0; i < 44; i++) { s->irq[i] = i; }
    irq_level[ch] = 0;
    memset(DRAM + dst_offset, 0xa5, 64);
    fifo_src_pos = 0;
    wr32(DESC, 0); wr32(DESC + 4, DESC_DATA | DESC_LAST);
    wr32(DESC + 8, DST); wr32(DESC + 12, 64);
    cdma_write(s, 0, 1u << ch, 4);
    setup_chan(s, ch, 0x110080, rx, DESC);
    cdma_write(s, CHREG(ch, CH_REMAIN), 0x40000000, 4);
    cdma_write(s, CHREG(ch, CH_CTRL), 0xb000019, 4);
    assert((s->ch[ch].ctrl & ST_RUNNING) && !(s->ch[ch].ctrl & ST_DONE));
    assert(!irq_level[ch] && !fifo_src_pos);
    assert(cdma_read(s, CHREG(ch, CH_REMAIN), 4) == budget);
    assert(cdma_read(s, CHREG(ch, CH_ADDR), 4) == DST);
    assert(cdma_read(s, CHREG(ch, CH_DESC), 4) == DESC);
    for (unsigned i = 0; i < 64; i++) {
        assert(DRAM[dst_offset + i] == 0xa5);
        fifo_src[i] = i ^ 0x6c;
    }
    /* Native v2+ position snapshot pauses, then restores CSR without GO. */
    uint32_t saved_csr = cdma_read(s, CHREG(ch, CH_CTRL), 4);
    cdma_write(s, CHREG(ch, CH_CTRL), 0x18 | CTRL_HOLD | CTRL_ABORT, 4);
    if (a6) {
        assert((s->ch[ch].ctrl & (ST_RUNNING | ST_HELD)) == (ST_RUNNING | ST_HELD));
        assert(!(s->ch[ch].ctrl & ST_ABORTED));
    } else {
        assert(!(s->ch[ch].ctrl & ST_RUNNING));
    }
    assert(!irq_level[ch]);
    assert(cdma_read(s, CHREG(ch, CH_REMAIN), 4) == budget);
    assert(cdma_read(s, CHREG(ch, CH_ADDR), 4) == DST);
    assert(cdma_read(s, CHREG(ch, CH_DESC), 4) == DESC);
    if (a6) {
        cdma_write(s, CHREG(ch, CH_CTRL), saved_csr, 4);
        assert((s->ch[ch].ctrl & ST_RUNNING) && !(s->ch[ch].ctrl & ST_HELD));
    }
    cdma_write(s, CHREG(ch, CH_CTRL), CTRL_RESET, 4);
    /* A connected source retains the existing receive/completion path. */
    s5l8930_cdma_set_source((DeviceState *)s, rx, 4, big_avail, NULL);
    setup_chan(s, ch, 0x110080, rx, DESC);
    cdma_write(s, CHREG(ch, CH_REMAIN), budget, 4);
    cdma_write(s, CHREG(ch, CH_CTRL), CTRL_GO | CTRL_IRQ_EN, 4);
    assert(fifo_src_pos == 64 && (s->ch[ch].ctrl & ST_DONE) && irq_level[ch]);
    assert(!memcmp(DRAM + dst_offset, fifo_src, 64));
    cdma_write(s, CHREG(ch, CH_CTRL), CTRL_RESET, 4);
    assert(!irq_level[ch]);
    /* Direction and register matter: TX must retain its normal completion. */
    fifo_sink_len = 0;
    setup_chan(s, ch, SET_TO_DEVICE, rx - 4, DESC);
    cdma_write(s, CHREG(ch, CH_CTRL), CTRL_GO | CTRL_IRQ_EN, 4);
    assert((s->ch[ch].ctrl & ST_DONE) && fifo_sink_len == 64);
    assert(!memcmp(fifo_sink, fifo_src, 64));
    free(s);
}

int main(void) {
    check_uart_receive(S5L8930_UART_BASE(0), 6, 0, 1, false);
    check_uart_receive(S5L8930_UART_BASE(0), 6, 0, 2, false);
    check_uart_receive(0x32500000, 7, 3, 5, true); /* native N42 UART3 / CDMA16 */
    check_protected_resume(false,true);
    check_protected_resume(true,true);
    check_protected_resume(false,false);
    check_protected_resume(true,false);
    S5L8930CDMAState *s = calloc(1, sizeof *s);
    s->num_channels = S5L8930_CDMA_CHANNELS;

    /* ---- +0x10/+0x14 report enabled channels, a status word ---- */
    CW(0x00, 0x5);                       /* enable channels 0 and 2 */
    CW(0x04, 0x8);                       /* enable channel 35 (bit 3 of word 1) */
    assert(CR(0x10) == 0x5 && CR(0x14) == 0x8);
    assert(CR(0x08) == 0 && CR(0x0c) == 0);   /* the clear window reads back 0 */
    CW(0x08, 0x1);                       /* clear channel 0 */
    assert(CR(0x10) == 0x4);             /* only channel 2 left in word 0 */

    /* ---- inline AES bypass, to-device: data reaches the FIFO in the clear ---- */
    for (int i = 0; i < 64; i++) DRAM[SRC - S5L8930_DRAM_BASE + i] = i + 1;
    fifo_sink_len = 0;
    wr32(DESC + 0, 0);                                        /* next */
    wr32(DESC + 4, DESC_DATA | DESC_LAST | DESC_AES);         /* AES requested... */
    wr32(DESC + 8, SRC);
    wr32(DESC + 12, 64);
    setup_chan(s, 1, SET_TO_DEVICE, FIFO_A, DESC);           /* device FIFO channel */
    CW(CHREG(1, CH_CTRL), CTRL_GO);
    assert(s->ch[1].ctrl & ST_DONE);
    assert(fifo_sink_len == 64);
    for (int i = 0; i < 64; i++) assert(fifo_sink[i] == (uint8_t)(i + 1));   /* ...but NOT applied */

    /* ---- inline AES bypass, from-device: FIFO reaches memory in the clear ---- */
    fifo_src_pos = 0;
    for (int i = 0; i < 64; i++) fifo_src[i] = 0xA0 + (i & 0xf);
    memset(DRAM + (DST - S5L8930_DRAM_BASE), 0, 64);
    wr32(DESC + 0, 0);
    wr32(DESC + 4, DESC_DATA | DESC_LAST | DESC_AES);
    wr32(DESC + 8, DST);
    wr32(DESC + 12, 64);
    setup_chan(s, 2, 0, FIFO_A, DESC);                       /* from-device (no SET_TO_DEVICE) */
    CW(CHREG(2, CH_CTRL), CTRL_GO);
    assert(s->ch[2].ctrl & ST_DONE);
    for (int i = 0; i < 64; i++)
        assert(DRAM[DST - S5L8930_DRAM_BASE + i] == (uint8_t)(0xA0 + (i & 0xf)));

    /* Native file protection adds an AES-256 layer on the NAND FIFO,
     * with a descriptor-loaded per-page IV. Base media AES-128 stays clear. */
    S5L8930CDMAState *file_crypto = calloc(1, sizeof *file_crypto);
    file_crypto->num_channels = S5L8930_CDMA_CHANNELS;
    file_crypto->nand_file_aes = true;
    file_crypto->a6 = true;   /* IV descriptors load the context on the A6 only */
    AESContext *file_aes = &file_crypto->aes[2];
    file_aes->setup = (5 << 8) | AES_ENABLE | AES_KEY_CUSTOM | (2 << 18);
    wr32(DESC, DESC + 32); wr32(DESC + 4, DESC_IV);
    wr32(DESC + 8, 0); wr32(DESC + 12, 0);
    for (int i = 0; i < 4; i++) wr32(DESC + 16 + 4 * i, 0x04030201u + i * 0x04040404u);
    wr32(DESC + 32, 0); wr32(DESC + 36, DESC_DATA | DESC_LAST | DESC_AES);
    wr32(DESC + 40, DST); wr32(DESC + 44, 16);
    fifo_src_pos = 0;
    for (int i = 0; i < 16; i++) fifo_src[i] = i;
    setup_chan(file_crypto, 5, 0, FIFO_A, DESC);
    cdma_write(file_crypto, CHREG(5, CH_CTRL), CTRL_GO, 4);
    assert(file_crypto->ch[5].ctrl & ST_DONE);
    for (int i = 0; i < 16; i++) {
        assert(DRAM[DST - S5L8930_DRAM_BASE + i] == (uint8_t)(i ^ 0xff));
        assert(cipher_iv[i] == i + 1);
    }
    file_aes->setup |= AES_ENCRYPT;
    fifo_sink_len = 0;
    wr32(DESC + 40, SRC);
    setup_chan(file_crypto, 5, SET_TO_DEVICE, FIFO_A, DESC);
    cdma_write(file_crypto, CHREG(5, CH_CTRL), CTRL_GO, 4);
    assert(file_crypto->ch[5].ctrl & ST_DONE);
    assert(fifo_sink_len == 16);
    for (int i = 0; i < 16; i++) {
        assert(fifo_sink[i] == (uint8_t)(DRAM[SRC - S5L8930_DRAM_BASE + i] ^ 0xff));
        assert(cipher_iv[i] == i + 1);
    }
    /* Existing legacy clear-page images also omit AES-256 inline NAND
     * transforms. Reusing that image must not decrypt its plaintext again. */
    file_crypto->nand_file_aes = false;
    fifo_sink_len = 0;
    setup_chan(file_crypto, 5, SET_TO_DEVICE, FIFO_A, DESC);
    cdma_write(file_crypto, CHREG(5, CH_CTRL), CTRL_GO, 4);
    assert(fifo_sink_len == 16);
    assert(!memcmp(fifo_sink, DRAM + (SRC - S5L8930_DRAM_BASE), 16));
    file_crypto->nand_file_aes = true;
    file_aes->setup &= ~(3u << 18);
    fifo_sink_len = 0;
    memset(cipher_iv, 0x5a, sizeof(cipher_iv));
    setup_chan(file_crypto, 5, SET_TO_DEVICE, FIFO_A, DESC);
    cdma_write(file_crypto, CHREG(5, CH_CTRL), CTRL_GO, 4);
    assert(file_crypto->ch[5].ctrl & ST_DONE);
    assert(fifo_sink_len == 16);
    assert(!memcmp(fifo_sink, DRAM + (SRC - S5L8930_DRAM_BASE), 16));
    for (int i = 0; i < 16; i++) assert(cipher_iv[i] == 0x5a);
    free(file_crypto);

    /* ---- FIFO-drained signal: a paced device FIFO waits for sink_done ---- */
    S5L8930CDMAState *s2 = calloc(1, sizeof *s2);
    s2->num_channels = S5L8930_CDMA_CHANNELS;
    s5l8930_cdma_set_source((DeviceState *)s2, FIFO_A, sizeof fifo_sink, big_avail, NULL);
    for (int i = 0; i < 64; i++) DRAM[SRC - S5L8930_DRAM_BASE + i] = 0x30 + i;
    fifo_sink_len = 0;
    wr32(DESC + 0, 0);
    wr32(DESC + 4, DESC_DATA | DESC_LAST);
    wr32(DESC + 8, SRC);
    wr32(DESC + 12, 64);
    setup_chan(s2, 1, SET_TO_DEVICE, FIFO_A, DESC);
    cdma_write(s2, CHREG(1, CH_CTRL), CTRL_GO, 4);
    assert((s2->ch[1].ctrl & ST_RUNNING) && !(s2->ch[1].ctrl & ST_DONE));   /* still waiting */
    assert(s2->sink_pending[1]);
    assert(fifo_sink_len == 64);
    for (int i = 0; i < 64; i++) assert(fifo_sink[i] == (uint8_t)(0x30 + i));   /* bytes are in the FIFO */
    s5l8930_cdma_sink_done((DeviceState *)s2, FIFO_A, sizeof fifo_sink);
    assert((s2->ch[1].ctrl & ST_DONE) && !(s2->ch[1].ctrl & ST_RUNNING) && !s2->sink_pending[1]);

    /* ---- memory-to-memory channels interrupt only with control bit 3 ---- */
    S5L8930CDMAState *s3 = calloc(1, sizeof *s3);
    s3->num_channels = S5L8930_CDMA_CHANNELS;
    for (int i = 0; i < CDMA_CHANNELS; i++) s3->irq[i] = i;
    cdma_write(s3, 0x00, 3u << 1, 4);                          /* channels 1 and 2 enabled */
    wr32(DESC + 0, 0);
    wr32(DESC + 4, DESC_DATA | DESC_LAST);
    wr32(DESC + 8, SRC);
    wr32(DESC + 12, 64);
    setup_chan(s3, 1, 0, 0, DESC);                             /* memory to the engine, context 1 */
    cdma_write(s3, CHREG(1, CH_CTRL), 0x188 | CTRL_GO, 4);     /* iOS 7's feeder */
    assert((s3->ch[1].ctrl & ST_DONE) && irq_level[1]);
    cdma_write(s3, CHREG(1, CH_CTRL), cdma_read(s3, CHREG(1, CH_CTRL), 4), 4);   /* its handler acks */
    assert(!irq_level[1]);
    setup_chan(s3, 1, 0, 0, DESC);
    cdma_write(s3, CHREG(1, CH_CTRL), 0x180 | CTRL_GO, 4);     /* 3.2's feeder: polled */
    assert((s3->ch[1].ctrl & ST_DONE) && !irq_level[1]);
    wr32(DESC + 8, DST);
    setup_chan(s3, 2, 0, 0, DESC);                             /* the engine to memory */
    cdma_write(s3, CHREG(2, CH_CTRL), 0x80 | CTRL_GO, 4);      /* 3.2's drain: polled */
    assert((s3->ch[2].ctrl & ST_DONE) && !irq_level[2]);
    wr32(DESC + 8, SRC);
    setup_chan(s3, 1, 0, 0, DESC);
    cdma_write(s3, CHREG(1, CH_CTRL), 0x180 | CTRL_GO, 4);
    wr32(DESC + 8, DST);
    setup_chan(s3, 2, 0, 0, DESC);
    cdma_write(s3, CHREG(2, CH_CTRL), 0x88 | CTRL_GO, 4);      /* iOS 7's drain */
    assert(irq_level[2] && !(s3->ch[2].ctrl & ST_ERROR));
    assert(memcmp(DRAM + (DST - S5L8930_DRAM_BASE), DRAM + (SRC - S5L8930_DRAM_BASE), 64) == 0);

    /* A device consumes its first 2 KiB while CDMA is still pushing that
     * burst. The room notification must resume the remaining 2 KiB after
     * descriptor progress is committed, without duplicating either burst. */
    S5L8930CDMAState *s4 = calloc(1, sizeof *s4);
    s4->num_channels = S5L8930_CDMA_CHANNELS;
    s4->a6 = true;   /* the deferred resume is the A6's ("a6"); other boards keep their run */
    resume_state = s4;
    s5l8930_cdma_set_source((DeviceState *)s4, FIFO_A, 4, burst_avail, NULL);
    for (unsigned i = 0; i < 4096; i++) DRAM[SRC - S5L8930_DRAM_BASE + i] = (i * 71) ^ (i >> 8);
    fifo_sink_len = 0; fifo_write_hook = drain_burst; bh_scheduled = false;
    wr32(DESC + 0, 0); wr32(DESC + 4, DESC_DATA | DESC_LAST);
    wr32(DESC + 8, SRC); wr32(DESC + 12, 4096);
    setup_chan(s4, 1, SET_TO_DEVICE, FIFO_A, DESC);
    cdma_write(s4, CHREG(1, CH_CTRL), CTRL_GO, 4);
    assert(fifo_sink_len == 2048 && s4->ch[1].remain == 2048 && bh_scheduled);
    for (unsigned n = 0; bh_scheduled && n < 8; n++) {
        bh_scheduled = false; cdma_kick_deferred(s4);
    }
    assert(!bh_scheduled && fifo_sink_len == 4096);
    assert(memcmp(fifo_sink, DRAM + (SRC - S5L8930_DRAM_BASE), 4096) == 0);
    assert((s4->ch[1].ctrl & ST_DONE) && !(s4->ch[1].ctrl & ST_RUNNING));
    fifo_write_hook = NULL;
    /* A6's display channel 40 lives beyond the A4 register range. The
     * legacy variant must reject it; the extended variant transfers every
     * descriptor byte and uses the second enable bank and its own IRQ. */
    S5L8930CDMAState *s5 = calloc(1, sizeof *s5);
    s5->num_channels = S5L8930_CDMA_CHANNELS;
    for (int i = 0; i < CDMA_CHANNELS; i++) s5->irq[i] = i;
    fifo_sink_len = 0;
    wr32(DESC + 0, 0); wr32(DESC + 4, DESC_DATA | DESC_LAST);
    wr32(DESC + 8, SRC); wr32(DESC + 12, 64);
    setup_chan(s5, 40, SET_TO_DEVICE, FIFO_A, DESC);
    cdma_write(s5, CHREG(40, CH_CTRL), CTRL_GO | CTRL_IRQ_EN, 4);
    assert(!s5->ch[40].ctrl && !fifo_sink_len);
    s5->num_channels = 44;
    cdma_write(s5, 4, 1u << 8, 4);
    setup_chan(s5, 40, SET_TO_DEVICE, FIFO_A, DESC);
    cdma_write(s5, CHREG(40, CH_CTRL), CTRL_GO | CTRL_IRQ_EN, 4);
    assert(fifo_sink_len == 64 && irq_level[40]);
    assert(memcmp(fifo_sink, DRAM + (SRC - S5L8930_DRAM_BASE), 64) == 0);
    assert(s5->ch[40].ctrl & ST_DONE);
    cdma_write(s5, CHREG(40, CH_CTRL), ST_DONE, 4);
    assert(!irq_level[40]);
    /* A6 MCA speaker is mono S16, not the legacy stereo stream. Its DMA
     * must progress at 88,200 bytes/sec, retaining whole two-byte frames. */
    S5L8930CDMAState *s6 = calloc(1, sizeof *s6);
    s6->num_channels = 44; s6->paced_base = 0x34190000; s6->paced_ports = 8;
    fifo_sink_len = 0; g_now = 100000000;
    wr32(DESC + 12, 4096);
    setup_chan(s6, 35, SET_TO_DEVICE, 0x34196028, DESC);
    cdma_write(s6, CHREG(35, CH_CTRL), CTRL_GO, 4);
    assert(s6->paced[35] && s6->paced_bps[35] == 88200);
    assert(fifo_sink_len == 0 && (s6->ch[35].ctrl & ST_RUNNING));
    g_now += 31250; /* 1.378 frames: only one whole mono frame may pass. */
    assert(cdma_paced_pos(s6, 35) == 2);
    cdma_read(s6, CHREG(35, CH_CTRL), 4);
    assert(fifo_sink_len == 2);
    g_now = 101000000;
    cdma_read(s6, CHREG(35, CH_CTRL), 4);
    assert(fifo_sink_len == 88 && s6->ch[35].remain == 4008);
    assert(memcmp(fifo_sink, DRAM + (SRC - S5L8930_DRAM_BASE), 88) == 0);
    cdma_paced_stop(s6, 35);
    /* Native UART receive starts BC at40000000 over a circular ring.
     * Empty FIFO, a partial descriptor, and crossing to the next one must
     * consume exactly the bytes read, rather than replace BC with800. */
    S5L8930CDMAState *s7 = calloc(1, sizeof *s7);
    s7->num_channels = 44; s7->version = 5; s7->a6 = true;
    s7->uart_base = 0x32500000; s7->uart_ports = 7;
    for (int i = 0; i < CDMA_CHANNELS; i++) s7->irq[i] = i;
    s5l8930_cdma_set_source((DeviceState *)s7, 0x32600024, 4, uart_avail, NULL);
    wr32(DESC + 0, DESC + 16); wr32(DESC + 4, DESC_DATA);
    wr32(DESC + 8, DST); wr32(DESC + 12, 8);
    wr32(DESC + 16, DESC); wr32(DESC + 20, DESC_DATA);
    wr32(DESC + 24, DST + 8); wr32(DESC + 28, 8);
    setup_chan(s7, 13, 0, 0x32600024, DESC);
    cdma_write(s7, CHREG(13, CH_REMAIN), 0x40000000, 4);
    uart_available = 0; fifo_src_pos = 0;
    cdma_write(s7, CHREG(13, CH_CTRL), CTRL_GO, 4);
    assert(s7->ch[13].remain == 0x40000000 && s7->ch[13].in_seg);
    assert(s7->ch[13].addr == DST && !fifo_src_pos);
    uint32_t running_csr = s7->ch[13].ctrl;
    cdma_write(s7, CHREG(13, CH_CTRL), running_csr | CTRL_HOLD | CTRL_ABORT, 4);
    uart_available = 3; /* Bytes arrive while the native position query holds DMA. */
    s5l8930_cdma_kick((DeviceState *)s7);
    assert(!fifo_src_pos && s7->ch[13].in_seg && s7->ch[13].remain == 0x40000000);
    bh_scheduled = false;
    cdma_write(s7, CHREG(13, CH_CTRL), running_csr, 4);
    assert(bh_scheduled && !(s7->ch[13].ctrl & ST_HELD));
    bh_scheduled = false; cdma_kick_deferred(s7);
    assert(s7->ch[13].remain == 0x40000000 - 3 && s7->ch[13].addr == DST + 3);
    uart_available = 5;
    cdma_run(s7, 13);
    assert(s7->ch[13].remain == 0x40000000 - 8 && s7->ch[13].desc == DESC + 16);
    assert(s7->ch[13].addr == DST + 8 && s7->ch[13].in_seg);
    cdma_write(s7, CHREG(13, CH_CTRL), (s7->ch[13].ctrl & ~CTRL_HOLD) | CTRL_ABORT, 4);
    assert(!(s7->ch[13].ctrl & ST_RUNNING) && (s7->ch[13].ctrl & ST_ABORTED));
    assert(!s7->ch[13].in_seg);
    /* iOS7 seeds the same streaming counter at zero. The first bytes wrap
     * BC rather than terminating DMA; native subtraction recovers progress. */
    setup_chan(s7, 13, 0, 0x32600024, DESC);
    cdma_write(s7, CHREG(13, CH_REMAIN), 0, 4);
    uart_available = 0; fifo_src_pos = 0;
    cdma_write(s7, CHREG(13, CH_CTRL), CTRL_GO, 4);
    assert(s7->ch[13].remain == 0 && s7->ch[13].in_seg);
    assert((s7->ch[13].ctrl & ST_RUNNING) && !fifo_src_pos);
    uart_available = 3;
    s5l8930_cdma_kick((DeviceState *)s7);
    assert(s7->ch[13].remain == UINT32_MAX - 2 && s7->ch[13].addr == DST + 3);
    assert((uint32_t)(0 - s7->ch[13].remain) == 3);
    uart_available = 6;
    s5l8930_cdma_kick((DeviceState *)s7);
    assert(s7->ch[13].remain == UINT32_MAX - 8 && s7->ch[13].addr == DST + 9);
    assert(s7->ch[13].desc == DESC + 16 && s7->ch[13].in_seg);
    free(s7);
    free(s); free(s2); free(s3); free(s4); free(s5); free(s6);
    /* A snapshot restore re-runs stalled channels from a bottom half on the A6 only; the other boards restore as
     * they were qualified, with nothing scheduled. */
    S5L8930CDMAState *restored = calloc(1, sizeof *restored);
    bh_scheduled = false;
    assert(cdma_post_load(restored, 4) == 0 && !bh_scheduled);
    restored->a6 = true;
    assert(cdma_post_load(restored, 4) == 0 && bh_scheduled);
    bh_scheduled = false;
    free(restored);
    puts("PASS: CDMA inline-AES bypass symmetric both ways, enabled-channel status regs, FIFO-drained signal, "
         "memory-to-memory interrupt enable, reentrant FIFO room resumes partial DMA exactly once, "
         "A6 channel 40 transfer/IRQ and legacy rejection; AES-256 file-layer read/write, descriptor IV and zero-room/partial CBC continuation");
}
