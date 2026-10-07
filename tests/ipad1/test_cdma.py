#!/usr/bin/env python3
"""S5L8930 CDMA (hw/arm/s5l8930_cdma.c): inline-AES symmetry on device-FIFO
channels, the enabled-channel status registers, and the FIFO-drained signal.

Driven through cdma_read/cdma_write with a small fake physical memory (the
descriptor walker's dma_memory_* and address_space_*), no emulator. Contracts:
  - A channel whose far end is a device FIFO treats its bound AES context as an
    inline filter that the page store leaves in the clear, so data passes
    through UNCHANGED in BOTH directions (to-device and from-device). Before the
    4.3.5 remount fix this was asymmetric and remounts read back garbage.
  - Global +0x10/+0x14 report which channels are enabled (set by +0x00/+0x04,
    cleared by +0x08/+0x0C), a status word, not a pending-interrupt bitmap.
  - A to-device chain that fills a paced device FIFO stays RUNNING until the
    device drains it (s5l8930_cdma_sink_done), then goes DONE.

Mutation (named, must fail this test): in cdma_run, drop the device-FIFO guard
    bool crypt = (flags & DESC_AES) && !dev_fifo;
to
    bool crypt = (flags & DESC_AES);
so a device-FIFO channel tries to AES its data (both directions) -- the
asymmetric-remount bug.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/s5l8930_cdma.c').read_text()
consts = '\n'.join(re.findall(r'^#define (?:CH_|CTRL_|ST_|SET_|DESC_|AES_|CDMA_|GID_)\w+[^\\\n]*$', source, re.M))
uid_key = re.search(r'static const uint8_t cdma_uid_key\[32\] = \{.*?\};', source, re.S).group()
structs = []
for pat in (r'typedef struct CDMAChannel \{.*?\} CDMAChannel;',
            r'typedef struct AESContext \{.*?\} AESContext;',
            r'typedef struct CDMASource \{.*?\} CDMASource;',
            r'struct S5L8930CDMAState \{.*?\n\};'):
    structs.append(re.search(pat, source, re.S).group())

order = ('gid_lookup', 'aes_apply', 'fifo_push', 'aes_feed', 'aes_for_channel',
         'cdma_is_memory', 'cdma_src', 'cdma_fifo_fed', 'cdma_update_irq', 'fifo_pop',
         'cdma_waits_for_uart', 'cdma_fifo_xfer', 'cdma_run', 'cdma_is_paced',
         'cdma_paced_advance', 'cdma_paced_pos', 'cdma_pace_arm', 'cdma_pace_tick',
         'cdma_paced_stop', 'cdma_start_paced', 'cdma_read', 'cdma_write',
         's5l8930_cdma_sink_done', 's5l8930_cdma_set_source', 's5l8930_cdma_kick')
funcs = []
for name in order:
    m = re.search(r'^(?:static |inline |bool |void |int |uint\w+ |hwaddr |const )[^\n]*\b' +
                  name + r'\((?:[^()]|\([^()]*\))*\)\s*\{.*?^}', source, re.M | re.S)
    assert m, name
    funcs.append(m.group())

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define g_assert_not_reached() abort()
typedef uint64_t hwaddr;
typedef int SysBusDevice, MemoryRegion, QEMUTimer, qemu_irq;
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
static void qemu_set_irq(int irq, int v) { (void)irq; (void)v; }
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
static int qcrypto_cipher_setiv(QCryptoCipher *c, const uint8_t *iv, size_t l, Error **e) { return 0; }
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
static uint32_t s5l8930_i2s_rate(int port) { (void)port; return 44100; }
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
    memcpy(buf, fifo_src + fifo_src_pos, len); fifo_src_pos += len;   /* device FIFO drain */
}
static void address_space_write(void *as, uint64_t addr, int attrs, const void *buf, unsigned len) {
    memcpy(fifo_sink + fifo_sink_len, buf, len); fifo_sink_len += len; /* device FIFO fill */
}
'''

tests = r'''
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
int main(void) {
    S5L8930CDMAState *s = calloc(1, sizeof *s);

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

    /* ---- FIFO-drained signal: a paced device FIFO waits for sink_done ---- */
    S5L8930CDMAState *s2 = calloc(1, sizeof *s2);
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

    free(s); free(s2);
    puts("PASS: CDMA inline-AES bypass symmetric both ways, enabled-channel status regs, FIFO-drained signal");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + consts + '\n' + '\n'.join(structs) + '\n' + uid_key + '\n' +
                 '\n'.join(funcs) + tests)
    binary = str(Path(tmp) / 'check')
    subprocess.run(['clang', '-std=gnu11', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
