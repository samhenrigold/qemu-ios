/* Actual AES candidate-mode vectors and MMIO permission/error tests, no guest boot.
 * SLICE hw/arm/s5l8930_cdma.c range #define CDMA_CHANNELS  | /* Per-IPSW GID stand-in
 * SLICE hw/arm/s5l8930_cdma.c define GID_
 * SLICE hw/arm/s5l8930_cdma.c fn aes_trace_tag gid_lookup aes_apply aes_skg_candidate aes_read aes_write
 */
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define trace_s5l8930_aes_transfer(...) ((void)0)
#define trace_s5l8930_cdma_iv(...) ((void)0)
#define TRACE_PRINTF(fn, ...)                                                  \
    do {                                                                       \
        if (0)                                                                 \
            printf(__VA_ARGS__);                                               \
    } while (0)
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
static void qemu_bh_schedule(QEMUBH *bh)
{
    (void)bh;
    bh_scheduled = true;
}
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
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define le32_to_cpu(x) (x)
#define qemu_log_mask(...) ((void)0)
static int irq_level[0x2c]; /* includes the A6 display channel */
static void qemu_set_irq(int irq, int v)
{
    irq_level[irq] = v;
}
typedef struct S5L8930CDMAState S5L8930CDMAState;
typedef int Error, GError;

#include <CommonCrypto/CommonCryptor.h>
#define QCRYPTO_CIPHER_ALGO_AES_128 0
#define QCRYPTO_CIPHER_ALGO_AES_192 1
#define QCRYPTO_CIPHER_ALGO_AES_256 2
#define QCRYPTO_CIPHER_MODE_CBC 0
static void error_report_err(Error *e)
{
    (void)e;
}
#define warn_report(...) ((void)0)
typedef int QCryptoCipherAlgo;
typedef struct QCryptoCipher {
    uint8_t key[32], iv[16];
    size_t keylen;
} QCryptoCipher;
static int qcrypto_cipher_get_key_len(int a)
{
    return 16 + a * 8;
}
static QCryptoCipher *qcrypto_cipher_new(int a, int mode, const uint8_t *key,
                                         size_t n, Error **e)
{
    assert(n == qcrypto_cipher_get_key_len(a));
    QCryptoCipher *c = calloc(1, sizeof(*c));
    memcpy(c->key, key, n);
    c->keylen = n;
    return c;
}
static int qcrypto_cipher_setiv(QCryptoCipher *c, const uint8_t *iv, size_t n,
                                Error **e)
{
    assert(n == 16);
    memcpy(c->iv, iv, 16);
    return 0;
}
static int cipher_op(QCryptoCipher *c, const void *in, void *out, size_t n,
                     int op)
{
    uint8_t last[16];
    size_t done;
    if (op == kCCDecrypt)
        memcpy(last, (const uint8_t *)in + n - 16, 16);
    int result = CCCrypt(op, kCCAlgorithmAES, 0, c->key, c->keylen, c->iv, in,
                         n, out, n, &done);
    if (result != kCCSuccess || done != n)
        return -1;
    memcpy(c->iv, op == kCCEncrypt ? (uint8_t *)out + n - 16 : last, 16);
    return 0;
}
static int qcrypto_cipher_encrypt(QCryptoCipher *c, const void *i, void *o,
                                  size_t n, Error **e)
{
    return cipher_op(c, i, o, n, kCCEncrypt);
}
static int qcrypto_cipher_decrypt(QCryptoCipher *c, const void *i, void *o,
                                  size_t n, Error **e)
{
    return cipher_op(c, i, o, n, kCCDecrypt);
}
static void qcrypto_cipher_free(QCryptoCipher *c)
{
    free(c);
}
static void stl_le_p(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
}
#include "slice.h"
int main(void)
{
    S5L8930CDMAState s = {0};
    s.skg_probe = true;
    const uint8_t expected[2][16] = {
        {0xc9, 0xc3, 0x3f, 0x2a, 0xed, 0xef, 0x86, 0x7f, 0xe8, 0xcc, 0xf9, 0x70,
         0x05, 0xb9, 0x3a, 0x8a},
        {0x6e, 0xd2, 0xc3, 0x3d, 0x12, 0x9a, 0xf5, 0x65, 0x3b, 0x5e, 0xae, 0xd7,
         0xac, 0x82, 0xf7, 0x86}};
    aes_write(&s, 0xfc, 0x80, 4);
    for (unsigned i = 0; i < 8; i++)
        aes_write(&s, 0x400 + 4 * i, i < 5 ? 0x03020100 + 0x04040404 * i : 0,
                  4);
    aes_write(&s, 0x420, 7, 4);
    for (unsigned count = 1; count <= 2; count++) {
        aes_write(&s, 0x430, 3, 4);
        aes_write(&s, 0x424, count, 4);
        aes_write(&s, 0x428, 1, 4);
        assert(aes_read(&s, 0x430, 4) == 2 && aes_read(&s, 0x42c, 4) == 0);
        assert(aes_read(&s, 0x400, 4) == 0);
        AESContext c = {.setup = (15 << 21) | AES_ENCRYPT};
        uint8_t block[16] = {0};
        assert(aes_apply(&s, &c, block, 16, true));
        assert(!memcmp(block, expected[count - 1], 16));
        c.setup = 15 << 21;
        assert(aes_apply(&s, &c, block, 16, true));
        for (int i = 0; i < 16; i++)
            assert(block[i] == 0);
    }
    aes_write(&s, 0x430, 3, 4);
    aes_write(&s, 0x424, 0, 4);
    aes_write(&s, 0x428, 1, 4);
    assert(aes_read(&s, 0x430, 4) == 1 && !(s.secret_valid & 0x80));
    AESContext c = {.setup = 15 << 21};
    uint8_t block[16] = {0};
    assert(!aes_apply(&s, &c, block, 16, true));
    aes_write(&s, 0x430, 3, 4);
    assert(!aes_read(&s, 0x430, 4));
    aes_write(&s, 0x100, 0x12345678, 4);
    assert(s.secret_valid & 1);
    assert(aes_read(&s, 0x100, 4) == 0);
    aes_write(&s, 0xfc, 1 << 8, 4);
    aes_write(&s, 0x100, 0xffffffff, 4);
    assert(s.secret_key[0][0] == 0x78);
    /* Explicit custom key retains precedence even with selector15 bits set. */
    c = (AESContext){.setup =
                         (15 << 21) | AES_KEY_CUSTOM | AES_ENCRYPT | (2 << 18)};
    memset(block, 0, 16);
    assert(aes_apply(&s, &c, block, 16, true));
    const uint8_t zero_key_aes[16] = {0xdc, 0x95, 0xc0, 0x78, 0xa2, 0x40,
                                      0x89, 0x89, 0xad, 0x48, 0xa2, 0x14,
                                      0x92, 0x84, 0x20, 0x87};
    assert(!memcmp(block, zero_key_aes, 16));
    puts(
        "PASS: actual AES SKG candidate vectors counts1/2, selector15 encrypt/decrypt, hidden seed/key reads, error/W1C, slot lock, custom AES256 precedence");
}
