/*
 * S5L8930 ("A4") CDMA: the shared descriptor DMA engine and its AES filter.
 *
 * Contract: docs/research/gap-kernel-platform-mmio.md §5 and
 * ref-a4-soc.md §1.10, re-checked against the 7B500 AppleCDMA kext
 * (start c044c2e8, channel set/clear c044c02c, _startChannel c044d590,
 * startChannel/resume c044d7e4, abort c044d46c, interrupt c044d74c,
 * AES ctx setup c044dc24/c044df74, IV c044d9b4, key c044da40,
 * _mapKeyID c044c850).
 *
 * Global window (offsets < 0x1000): +0x00/+0x04 channel-enable set,
 * +0x08/+0x0C clear, +0x10/+0x14 the enabled channels (read). Channel n lives
 * at n << 12 (n = 1..0x25; channel 0's slot is the global block):
 *   +0x00 ctrl/status: write bit0 go, bit1 reset, bit2 abort, bit3 interrupt, bit5 hold;
 *                      bits 16-17 state (1 = running, 2 = stopped by an
 *                      abort on the A6), 0x40000 error,
 *                      0x80000 done, 0x100000 descriptor flag, 0x200000
 *                      abort done; the 0x3C0000 bits are write-1-to-clear.
 *   +0x04 settings (bit1 = memory -> device, bits 2-3 width, 4-6 burst,
 *         16-21 peripheral id), +0x08 device FIFO address, +0x0C bytes left,
 *         +0x10 current address, +0x14 current descriptor, +0x18 error.
 * Descriptor (32 B): {next, flags, addr, len, driver private x4}. flags & 0xF
 * == 3 is a data segment, 0 ends the chain, 0x100 marks the last segment,
 * 0x10000 routes the data through the AES context bound to this channel and
 * 0x20000 restarts that context's IV. The kernel links a 128-entry ring.
 *
 * AES window: context n at n << 12, +0x00 setup (bits 8-15 DMA channel, 16
 * encrypt, 17 enable, 18-19 key length 128/192/256, 20 custom key, 21 GID,
 * 22 UID), +0x10 IV, +0x20 key. Context 0's word is the key-disable register
 * (bit n = key n is fused off; sticky).
 *
 * AES data operations use two channels as a pipeline (7B500 CDMAAES via
 * CDMAChannelM2M): channel 1, with the context number in ctrl bits 8-15 and
 * no device address, feeds its descriptors through the context into the
 * engine; channel 2, with neither, drains the engine's output into its own
 * descriptors. Each interrupts when its chain is done if control bit 3 asks
 * (cdma_update_irq): 7.x sets it on both, 3.2 on neither (it polls).
 *
 * A chain runs to completion inside the go write, so the guest never observes
 * the running state and the resume/abort handshakes degenerate to flag
 * bookkeeping. The exception is a channel whose device address is an I2S
 * FIFO: AppleARMIISAudio queues the 16-page (64 KiB) IOAudio ring as one
 * chain per go and expects it to take the ring's playing time, so that chain
 * streams to the FIFO at the port's rate (stereo S16) in virtual time. The UID key is a fixed made-up value (as on the iPod machine);
 * GID operations are answered from a table of this build's img3 KBAGs.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "crypto/cipher.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"
#include "migration/blocker.h"
#include "system/dma.h"
#include "qemu/timer.h"
#include "trace.h"
#include "hw/trace-printf.h"
#include "hw/arm/ipod_touch_guard.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930CDMAState, S5L8930_CDMA)

#define CDMA_CHANNELS       0x2c /* capacity: A6 DT max-channel 0x2b, inclusive */
#define CDMA_LEGACY_CHANNELS S5L8930_CDMA_CHANNELS
#define CDMA_CHAN_SHIFT     12
#define AES_CONTEXTS        9
#define CDMA_FIFO_MAX       CDMA_MAX_SEGMENT    /* engine output not yet drained; a snapshot holds no more */

#define CH_CTRL             0x00
#define CH_SETTINGS         0x04
#define CH_FIFO             0x08
#define CH_REMAIN           0x0C
#define CH_ADDR             0x10
#define CH_DESC             0x14
#define CH_ERROR            0x18

#define CTRL_GO             (1u << 0)
#define CTRL_RESET          (1u << 1)
#define CTRL_ABORT          (1u << 2)
#define CTRL_IRQ_EN         (1u << 3)       /* the done/error interrupt (cdma_update_irq) */
#define CTRL_HOLD           (1u << 5)
#define CTRL_CONFIG_MASK    0xFFF8u         /* bits the guest owns */
#define CTRL_AES_CTX(ctrl)  (((ctrl) >> 8) & 0xFF)
#define ST_RUNNING          (1u << 16)
#define ST_HELD             (1u << 17)      /* a running channel parked by CTRL_HOLD */
#define ST_STOPPED          (2u << 16)      /* the A6's state after an abort (CDMAChannel.stopped) */
#define ST_ERROR            (1u << 18)
#define ST_DONE             (1u << 19)
#define ST_FLAG             (1u << 20)
#define ST_ABORTED          (1u << 21)
#define ST_W1C              (ST_ERROR | ST_DONE | ST_FLAG | ST_ABORTED)

#define SET_TO_DEVICE       (1u << 1)

#define DESC_TYPE_MASK      0xFu
#define DESC_DATA           0x3u
/*
 * 2: words 4-7 load the bound AES context's IV (iBoot's NAND reads, one per
 * page). The page store keeps NAND data decrypted (the IOP model ignores the
 * key too). Its base AES-128 layer is omitted in the page store, while a
 * custom AES-256 content-protection layer is applied normally.
 */
#define DESC_IV             0x2u
#define DESC_LAST           0x100u
#define DESC_AES            0x10000u
#define DESC_AES_RESTART    0x20000u

#define AES_SETUP           0x00
#define AES_IV              0x10
#define AES_KEY             0x20
#define AES_CHAN(setup)     (((setup) >> 8) & 0xFF)
#define AES_ENCRYPT         (1u << 16)
#define AES_ENABLE          (1u << 17)
#define AES_KEYLEN(setup)   (((setup) >> 18) & 3)
#define AES_KEY_CUSTOM      (1u << 20)
#define AES_KEY_GID         (1u << 21)
#define AES_KEY_UID         (1u << 22)

/* Largest single segment the walker will move; a stray len cannot allocate GBs. */
#define CDMA_MAX_SEGMENT    (16 * 1024 * 1024)

typedef struct CDMAChannel {
    uint32_t ctrl, settings, fifo, remain, addr, desc, error;
    /* stalled inside the segment at desc; addr/remain live (migrated through
     * S5L8930CDMAState.in_seg_mig: the IOP firmware's NAND chains stall any time) */
    bool in_seg;
    /* The A6's: aborted since the last go, through the reset that follows (not migrated: the A6 blocks it) */
    bool stopped;
} CDMAChannel;

typedef struct AESContext {
    uint32_t setup;
    uint32_t iv[4];
    uint32_t key[8];
    uint8_t chain[16];      /* running CBC IV */
    /* Bytes of a block split across feeding segments (aes_feed). Always
     * empty between guest accesses -- a chain runs inside its go write --
     * so it is not migrated. */
    uint8_t carry[16];
    uint32_t carry_len;
} AESContext;

/* A device FIFO that paces the channels pointed at it (s5l8930_cdma_set_source). */
typedef struct CDMASource {
    hwaddr base, size;
    uint32_t (*avail)(void *opaque, hwaddr addr, bool to_device);
    void *opaque;
} CDMASource;

struct S5L8930CDMAState {
    SysBusDevice parent_obj;
    uint64_t dram_base;
    uint64_t dram_size;         /* property: device FIFO vs memory is by address */
    uint64_t uart_base;
    uint32_t uart_ports;
    uint32_t num_channels;
    bool a6;                    /* "a6": the A6's run, hold and UART-ring semantics (s5l8950.c); off, as qualified */
    Error *migration_blocker;
    MemoryRegion cdma_mem;
    MemoryRegion aes_mem;
    qemu_irq irq[CDMA_CHANNELS];

    uint32_t enabled[2];
    uint8_t version;        /* DT cdma-version: 1 (S5L8920) has no channel-enable registers */
    uint32_t paced_base, paced_ports;
    CDMAChannel ch[CDMA_CHANNELS];
    AESContext aes[AES_CONTEXTS];
    /* Experimental A6 SKG: register layout established from 10A405 CDMA.
     * The AES feedback candidate is not qualified against physical A6 vectors.
     * Opt-in only; never replace an unsupported operation with a success. */
    bool skg_probe;
    bool nand_file_aes;
    uint32_t secret_ctrl, secret_seed[8], skg_slot, skg_count, skg_control;
    uint32_t skg_status;
    uint8_t secret_key[8][32];
    uint8_t secret_valid;
    /* engine output queue: filled by the feeding channel, drained by ch2 */
    uint8_t *fifo;
    uint32_t fifo_len;
    bool gid_warned;
    char *gid_path;
    uint8_t *gid_data;
    size_t gid_size;
    /* device FIFOs that pace their channels (s5l8930_cdma_set_source):
     * the FMI's, the baseband SPI's, a UART's receive register and the
     * AMC's output port */
    CDMASource src[4];
    /* Audio channels (I2S FIFOs) play out in real time, not inside the go
     * write; paced[] marks a chain in flight. */
    bool paced[CDMA_CHANNELS];
    bool sink_pending[CDMA_CHANNELS];       /* to-device chain in a device FIFO, done when it drains */
    /* transient, inside cdma_run: the device drained the FIFO after this run's last push */
    bool in_run[CDMA_CHANNELS], sink_early[CDMA_CHANNELS];
    QEMUBH *kick_bh;
    bool in_seg_mig[CDMA_CHANNELS];         /* ch[].in_seg on the wire (vmstate 4) */
    int64_t paced_start[CDMA_CHANNELS];
    int64_t paced_end[CDMA_CHANNELS];       /* when the last chain's final byte played */
    uint32_t paced_desc[CDMA_CHANNELS];     /* chain head the go named */
    uint64_t paced_sent[CDMA_CHANNELS];     /* bytes already in the FIFO */
    uint32_t paced_bps[CDMA_CHANNELS];      /* bytes/s: 4 * port frame rate */
    QEMUTimer *pace_timer;
};

/* i2s0-2 TX/RX FIFOs; stereo S16 at the port's rate (see s5l8930_i2s.c). */
/* The I2S FIFOs, one 4 KiB slot per port ("paced-base"/"paced-ports"): the
 * A4's at 0x84500400, the S5L8920's i2s0 FIFO at 0x84500000. */
#define CDMA_PACED_LO       (s->paced_base)
#define CDMA_PACED_HI       (s->paced_base + s->paced_ports * 0x1000)

/* ---- AES filter ---- */

/* Not the fused UID: any fixed value works as long as this machine keeps it. */
static const uint8_t cdma_uid_key[32] = {
    0x4b, 0x34, 0x38, 0x41, 0x50, 0x2d, 0x55, 0x49, 0x44, 0x2d, 0x53, 0x35,
    0x4c, 0x38, 0x39, 0x33, 0x30, 0x2d, 0x69, 0x50, 0x61, 0x64, 0x31, 0x2d,
    0x37, 0x42, 0x35, 0x30, 0x30, 0x2d, 0x30, 0x31,
};

/* Per-IPSW GID stand-in: 96-byte records, encrypted KBAG followed by
 * plaintext IV || AES-256 key. Immutable machine configuration, not VM state. */
#define GID_BLOB_SIZE 48

static const uint8_t *gid_lookup(S5L8930CDMAState *s,
                                 const uint8_t *buf, uint32_t len)
{
    if (len != GID_BLOB_SIZE) {
        return NULL;
    }
    for (size_t i = 0; i < s->gid_size; i += 2 * GID_BLOB_SIZE) {
        if (!memcmp(s->gid_data + i, buf, GID_BLOB_SIZE)) {
            return s->gid_data + i + GID_BLOB_SIZE;
        }
    }
    return NULL;
}

static bool aes_apply(S5L8930CDMAState *s, AESContext *c, uint8_t *buf,
                      uint32_t len, bool restart);
static bool fifo_push(S5L8930CDMAState *s, const uint8_t *buf, uint32_t len);

/*
 * Feed a segment through the engine. The engine works on the stream, not per
 * segment: IOAESAccelerator maps a user buffer page by page, so a buffer that
 * is not 16-byte aligned arrives as segments like 4093 + 3 bytes, and one
 * block straddles the page boundary. aes_apply() on each segment rejected
 * those (len % 16), so every /dev/aes_0 request (CommonCrypto sends CBC of
 * more than 64 blocks there) on a misaligned buffer that crossed a page came
 * back wrong -- SecureTransport's large TLS records on the iPad, for one.
 * Carry the partial block over; RESTART on the first segment reloads the IV.
 * Checked by contrib/it-cctest + tests/ipad1/cctest.py (0 of 652 cases wrong).
 */
static bool aes_feed(S5L8930CDMAState *s, AESContext *c, const uint8_t *buf,
                     uint32_t len, bool restart)
{
    g_autofree uint8_t *work = g_malloc(c->carry_len + len);
    uint32_t n, whole;
    bool ok = true;

    if (restart) {
        c->carry_len = 0;
        for (int i = 0; i < 4; i++) {
            stl_le_p(c->chain + 4 * i, c->iv[i]);
        }
    }
    memcpy(work, c->carry, c->carry_len);
    memcpy(work + c->carry_len, buf, len);
    n = c->carry_len + len;
    whole = n & ~15u;
    if (whole) {
        ok = aes_apply(s, c, work, whole, false);
        ok = fifo_push(s, work, whole) && ok;
    }
    c->carry_len = n - whole;
    memcpy(c->carry, work + whole, c->carry_len);
    return ok;
}

static AESContext *aes_for_channel(S5L8930CDMAState *s, int ch)
{
    for (int i = 1; i < AES_CONTEXTS; i++) {
        AESContext *c = &s->aes[i];
        if ((c->setup & AES_ENABLE) && AES_CHAN(c->setup) == ch) {
            return c;
        }
    }
    return NULL;
}

/* Diagnostic fingerprints let native write/read traces be compared without
 * recording guest keys or plaintext. They are not cryptographic operations. */
static uint32_t aes_trace_tag(const uint8_t *bytes, size_t length)
{
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        value = (value ^ bytes[i]) * 16777619u;
    }
    return value;
}

/* CBC over one segment, chaining through c->chain. Returns false on error. */
static bool aes_apply(S5L8930CDMAState *s, AESContext *c, uint8_t *buf,
                      uint32_t len, bool restart)
{
    static const QCryptoCipherAlgo algos[] = {
        QCRYPTO_CIPHER_ALGO_AES_128, QCRYPTO_CIPHER_ALGO_AES_192,
        QCRYPTO_CIPHER_ALGO_AES_256, QCRYPTO_CIPHER_ALGO_AES_256,
    };
    const uint8_t *key;
    uint8_t keybuf[32];
    uint8_t last[16];
    QCryptoCipher *cipher;
    QCryptoCipherAlgo algo;
    bool encrypt = c->setup & AES_ENCRYPT;
    Error *err = NULL;
    uint32_t input_tag = 0, iv_tag = 0;
    bool trace_crypto = trace_event_get_state_backends(TRACE_S5L8930_AES_TRANSFER);

    if (len % 16) {
        return false;
    }
    if (restart) {
        for (int i = 0; i < 4; i++) {
            stl_le_p(c->chain + 4 * i, c->iv[i]);
        }
    }

    if (c->setup & AES_KEY_CUSTOM) {
        for (int i = 0; i < 8; i++) {
            stl_le_p(keybuf + 4 * i, c->key[i]);
        }
        key = keybuf;
        algo = algos[AES_KEYLEN(c->setup)];     /* bounded: a 2-bit field, 4 entries */
    } else if (s->skg_probe && ((c->setup >> 21) & 15) >= 8) {
        unsigned slot = ((c->setup >> 21) & 15) - 8;
        if (!(s->secret_valid & (1u << slot))) {
            return false;
        }
        key = s->secret_key[slot];
        /* UniquePlus, like Unique, has a fixed 256-bit hardware key even
         * though native CDMA leaves the custom-key length field zero. */
        algo = QCRYPTO_CIPHER_ALGO_AES_256;
    } else if (c->setup & AES_KEY_GID) {
        const uint8_t *b = encrypt ? NULL : gid_lookup(s, buf, len);
        if (b) {
            memcpy(buf, b, GID_BLOB_SIZE);
            if (len > GID_BLOB_SIZE) {
                memset(buf + GID_BLOB_SIZE, 0, len - GID_BLOB_SIZE);
            }
            return true;
        }
        if (!s->gid_warned) {
            s->gid_warned = true;
            warn_report("s5l8930.cdma: GID %scrypt of %u bytes not in the "
                        "gid-blobs file; supply this IPSW's keys with gid-blobs=",
                        encrypt ? "en" : "de", len);
        }
        key = cdma_uid_key;
        algo = QCRYPTO_CIPHER_ALGO_AES_256;
    } else {
        key = cdma_uid_key;
        algo = QCRYPTO_CIPHER_ALGO_AES_256;
    }

    if (trace_crypto) {
        input_tag = aes_trace_tag(buf, len);
        iv_tag = aes_trace_tag(c->chain, sizeof(c->chain));
    }

    cipher = qcrypto_cipher_new(algo, QCRYPTO_CIPHER_MODE_CBC, key,
                                qcrypto_cipher_get_key_len(algo), &err);
    if (!cipher) {
        error_report_err(err);
        return false;
    }
    if (qcrypto_cipher_setiv(cipher, c->chain, 16, NULL) != 0) {
        qcrypto_cipher_free(cipher);
        return false;
    }
    if (!encrypt) {
        memcpy(last, buf + len - 16, 16);
    }
    if ((encrypt ? qcrypto_cipher_encrypt(cipher, buf, buf, len, NULL)
                 : qcrypto_cipher_decrypt(cipher, buf, buf, len, NULL)) != 0) {
        qcrypto_cipher_free(cipher);
        return false;
    }
    if (encrypt) {
        memcpy(last, buf + len - 16, 16);
    }
    if (trace_crypto) {
        trace_s5l8930_aes_transfer(c - s->aes, c->setup, len, restart,
            aes_trace_tag(key, qcrypto_cipher_get_key_len(algo)), iv_tag,
            input_tag, aes_trace_tag(buf, len));
    }
    memcpy(c->chain, last, 16);
    qcrypto_cipher_free(cipher);
    return true;
}

/* ---- channel engine ---- */

static bool cdma_is_memory(S5L8930CDMAState *s, uint32_t addr);

/* The pacing source whose window holds addr, or NULL. */
static CDMASource *cdma_src(S5L8930CDMAState *s, uint32_t addr)
{
    for (int i = 0; i < ARRAY_SIZE(s->src); i++) {
        if (s->src[i].avail && addr >= s->src[i].base &&
            addr - s->src[i].base < s->src[i].size) {
            return &s->src[i];
        }
    }
    return NULL;
}

/* Reading a device FIFO that fills as it goes (iBoot's NAND), not feeding
 * the AES engine. Distinct from the time-paced audio channels below. */
static bool cdma_fifo_fed(S5L8930CDMAState *s, CDMAChannel *c)
{
    return !cdma_is_memory(s, c->fifo) && !(c->settings & SET_TO_DEVICE) &&
           cdma_src(s, c->fifo);
}

static void cdma_update_irq(S5L8930CDMAState *s, int ch)
{
    CDMAChannel *c = &s->ch[ch];
    bool en = s->enabled[ch >> 5] & (1u << (ch & 31));

    /*
     * Control bit 3 enables a channel's done/error interrupt. Every driver
     * path that waits on the interrupt sets it (AppleCDMA's channel start
     * |= 0x18, 3.2 c044d5c8 and 7.0b1 80a75e52; 7.x's CDMAChannelM2M start
     * 0x88, 80a7743a), and 3.2's AES pair, which polls, runs with 0x180 and
     * 0x80. Each CDMAChannelM2M has its own handler (3.2 c044e0cc, 7.0b1
     * 80a76af0) that panics ("CDMA M2M unexpected interrupt") if its channel
     * is not clocked, and 7.x turns the channel's clock off only there: with
     * the AES feeder's line held low, as this model had it, its clock stayed
     * on after the first AES operation and AppleCDMA's sleep check panicked
     * ("client driver(s) left channel(s) 0x2 active when sleeping";
     * LightTouchMac issue 83), while with it raised 3.2 panicked in the
     * handler. ponytail: the gate applies to memory-to-memory channels only;
     * the device channels (and the IOP firmware's) interrupt as before
     * whatever bit 3 says.
     */
    if (!(c->ctrl & CTRL_IRQ_EN) && (!c->fifo || cdma_is_memory(s, c->fifo))) {
        en = false;
    }
    qemu_set_irq(s->irq[ch], en && (c->ctrl & (ST_DONE | ST_ERROR)));
}

/*
 * The engine's output waits here for a draining channel. A feeding channel the
 * guest never drains would grow it without end (and wrap fifo_len), so past
 * CDMA_FIFO_MAX the push fails and the channel reports an error.
 */
static bool fifo_push(S5L8930CDMAState *s, const uint8_t *buf, uint32_t len)
{
    if (!IT_RANGE("s5l8930.cdma", s->fifo_len, len, CDMA_FIFO_MAX)) {
        return false;
    }
    s->fifo = g_realloc(s->fifo, s->fifo_len + len);
    memcpy(s->fifo + s->fifo_len, buf, len);
    s->fifo_len += len;
    return true;
}

static uint32_t fifo_pop(S5L8930CDMAState *s, uint8_t *buf, uint32_t len)
{
    len = MIN(len, s->fifo_len);
    memcpy(buf, s->fifo, len);
    memmove(s->fifo, s->fifo + len, s->fifo_len - len);
    s->fifo_len -= len;
    return len;
}

/*
 * ponytail: a device-side address inside DRAM is treated as a memory buffer
 * (increments: the M2M/AES copy channel), anything else as a fixed FIFO
 * written in settings-width units. Add a real M2M mode bit if a client's
 * settings word shows one.
 */
/*
 * A receive chain from a UART's URXH (+0x24) completes only as bytes arrive,
 * and no UART feeds this model. Run instantly, it filled the ring with the
 * empty register's zeros at CPU speed: BlueTool's HCI reader on UART3
 * (channel 0xd) spun at 100% CPU and never let SpringBoard power off.
 * ponytail: stays running forever. A UART with something behind it
 * registers its URXH as a source instead (s5l8920.c's Bluetooth HCI) and
 * the chain is paced by its receive FIFO like the FMI's.
 */
static bool cdma_waits_for_uart(S5L8930CDMAState *s, const CDMAChannel *c)
{
    return !(c->settings & SET_TO_DEVICE) &&
           c->fifo >= s->uart_base &&
           (uint64_t)c->fifo - s->uart_base < ((uint64_t)s->uart_ports << 20) &&
           (((uint64_t)c->fifo - s->uart_base) & 0xfffff) == 0x24;
}

static bool cdma_is_memory(S5L8930CDMAState *s, uint32_t addr)
{
    return addr >= s->dram_base &&
           (uint64_t)addr < s->dram_base + s->dram_size;
}

static void cdma_fifo_xfer(uint32_t fifo, uint32_t width, uint8_t *buf,
                           uint32_t len, bool to_device)
{
    for (uint32_t off = 0; off < len; off += width) {
        uint32_t n = MIN(width, len - off);
        if (to_device) {
            address_space_write(&address_space_memory, fifo,
                                MEMTXATTRS_UNSPECIFIED, buf + off, n);
        } else {
            address_space_read(&address_space_memory, fifo,
                               MEMTXATTRS_UNSPECIFIED, buf + off, n);
        }
    }
}

static void cdma_run(S5L8930CDMAState *s, int ch)
{
    if (s->a6) {
        if (s->in_run[ch]) {
            qemu_bh_schedule(s->kick_bh);
            return;
        }
        s->in_run[ch] = true;
    }
    CDMAChannel *c = &s->ch[ch];
    int ctx = CTRL_AES_CTX(c->ctrl);
    uint32_t dev = c->fifo;
    bool dev_mem = cdma_is_memory(s, dev);
    bool fed = cdma_fifo_fed(s, c);
    /* A context on a memory-to-memory channel feeds the engine (the AP's
     * AppleCDMA pair); on a channel whose far end is a device FIFO it is an
     * inline filter. The page store omits the base AES-128 media layer,
     * while custom AES-256 file encryption must remain effective. */
    bool dev_fifo = c->fifo && !dev_mem;
    AESContext *aes = (ctx > 0 && ctx < AES_CONTEXTS && !fed && !dev_fifo) ?
                      &s->aes[ctx] : NULL;
    bool feeds = aes != NULL;                   /* memory -> AES engine */
    bool drains = !feeds && !c->fifo && !c->settings;   /* engine -> memory */
    bool to_device = feeds || (c->settings & SET_TO_DEVICE);
    bool uart_counter = s->a6 && cdma_waits_for_uart(s, c);
    uint32_t width = 1u << ((c->settings >> 2) & 3);
    uint32_t error = 0;

    c->ctrl &= ~(ST_RUNNING | ST_ERROR);
    for (int n = 0; n < 4096 && !error; n++) {
        uint32_t d[4];
        uint32_t flags, len;
        bool resume = c->in_seg;

        if (dma_memory_read(&address_space_memory, c->desc, d, sizeof(d),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            error = 1;
            break;
        }
        flags = le32_to_cpu(d[1]);
        len = le32_to_cpu(d[3]);
        if ((flags & DESC_TYPE_MASK) == 0) {
            break;                              /* empty slot: chain ends */
        }
        if ((flags & DESC_TYPE_MASK) == DESC_IV) {
            AESContext *bound = aes;
            uint32_t iv_words[4];
            if (!bound && ctx > 0 && ctx < AES_CONTEXTS) {
                bound = &s->aes[ctx];
            }
            if (!bound) {
                bound = aes_for_channel(s, ch);
            }
            if (s->a6 && bound) {
                if (dma_memory_read(&address_space_memory, c->desc + 16,
                        iv_words, sizeof(iv_words), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                    error = 1;
                    break;
                }
                for (int word = 0; word < 4; word++) {
                    bound->iv[word] = le32_to_cpu(iv_words[word]);
                    stl_le_p(bound->chain + 4 * word, bound->iv[word]);
                }
                bound->carry_len = 0;
            }
            if (trace_event_get_state_backends(TRACE_S5L8930_CDMA_IV)) {
                uint8_t iv[16];
                if (dma_memory_read(&address_space_memory, c->desc + 16,
                        iv, sizeof(iv), MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
                    uint8_t key_bytes[32];
                    uint32_t key_tag = 0;
                    if (bound && (bound->setup & AES_KEY_CUSTOM)) {
                        for (int word = 0; word < 8; word++) {
                            stl_le_p(key_bytes + word * 4, bound->key[word]);
                        }
                        key_tag = aes_trace_tag(key_bytes,
                            16 + MIN(AES_KEYLEN(bound->setup), 2) * 8);
                    }
                    trace_s5l8930_cdma_iv(ch, dev_fifo, c->desc, dev,
                        bound ? bound - s->aes : -1,
                        bound ? bound->setup : 0, key_tag,
                        aes_trace_tag(iv, sizeof(iv)));
                }
            }
            /* The descriptor loads the next page's IV even when the base
             * NAND-media transform is represented by clear stored pages. */
            c->desc = le32_to_cpu(d[0]);
            continue;
        }
        if ((flags & DESC_TYPE_MASK) != DESC_DATA || len > CDMA_MAX_SEGMENT) {
            error = 2;
            break;
        }
        if (!resume) {
            c->addr = le32_to_cpu(d[2]);
            if (!uart_counter) {
                c->remain = len;
            }
        }
        c->in_seg = false;
        if (uart_counter) {
            uint32_t transferred = c->addr - le32_to_cpu(d[2]);

            /* BC is the byte counter across the ring, not this
             * descriptor's length. Derive segment progress from MAR so
             * resumptions and migration preserve both independently. */
            if (transferred > len) {
                error = 2;
                break;
            }
            /* On a streaming UART ring BC is a wrapping byte counter,
             * not a transfer limit. Native 10A405 seeds it with 0x40000000,
             * while 11A465 seeds it with 0 and subtracts the sampled value
             * from its previous sample (AppleCDMA80a12da0/80a13390).
             * The descriptor bounds the current segment in both cases. */
            len -= transferred;
        } else {
            len = c->remain;
        }
        if (fed || (to_device && dev_fifo && cdma_src(s, dev))) {
            /* Take what the device has (or has room for); stall (still running) for the rest. */
            uint32_t avail = cdma_src(s, dev)->avail(cdma_src(s, dev)->opaque, dev,
                                                     to_device) & ~(width - 1);
            if (uart_counter) {
                trace_s5l8930_cdma_uart(ch, "available", c->ctrl, c->remain,
                                      c->addr, c->desc, avail);
            }
            if (avail < len) {
                len = avail;
                c->in_seg = true;
            }
        }
        if (len) {
            g_autofree uint8_t *buf = g_malloc(len);
            bool ok = true;
            /* A restart belongs to the descriptor's first transferred byte,
             * not every FIFO-sized burst. A zero-room stall has transferred
             * nothing yet, even though the channel is marked as resumed. */
            bool restart_crypto = (flags & DESC_AES_RESTART) &&
                                  (!s->a6 || c->remain == le32_to_cpu(d[3]));

            /* The backing store omits the base NAND-media AES-128 layer.
             * Native content protection adds a distinct custom AES-256 file
             * layer, including accelerated reads on the same NAND FIFO. That
             * layer must remain encrypted at rest and be filtered normally.
             * The original driver identifies it through the context setup,
             * not an OS-version or filesystem-content test. */
            AESContext *bound = aes;
            if (!bound && ctx > 0 && ctx < AES_CONTEXTS) {
                bound = &s->aes[ctx];
            }
            if (!bound) {
                bound = aes_for_channel(s, ch);
            }
            bool file_layer = s->nand_file_aes && bound &&
                              (bound->setup & AES_KEY_CUSTOM) &&
                              AES_KEYLEN(bound->setup) >= 2;
            bool crypt = (flags & DESC_AES) && (!dev_fifo || file_layer);

            if (crypt) {
                aes = s->a6 ? bound : aes ? aes : aes_for_channel(s, ch);
                if (!aes) {
                    error = 3;
                    break;
                }
            }
            if (to_device) {
                dma_memory_read(&address_space_memory, c->addr, buf, len,
                                MEMTXATTRS_UNSPECIFIED);
                if (feeds && (flags & DESC_AES)) {
                    ok = aes_feed(s, aes, buf, len, restart_crypto);
                } else if (crypt) {
                    ok = aes_apply(s, aes, buf, len, restart_crypto);
                }
                if (feeds) {
                    if (!(flags & DESC_AES)) {
                        ok = fifo_push(s, buf, len) && ok;
                    }
                } else if (dev_mem) {
                    dma_memory_write(&address_space_memory, dev, buf, len,
                                     MEMTXATTRS_UNSPECIFIED);
                    dev += len;
                } else {
                    /* The push can complete the device's transfer at once
                     * (the FMI already in write mode), which drains the FIFO
                     * before this run gets to wait for that. */
                    s->sink_early[ch] = false;
                    if (!s->a6) {
                        s->in_run[ch] = true;   /* the A6 holds it for the whole run */
                    }
                    cdma_fifo_xfer(dev, width, buf, len, true);
                    if (!s->a6) {
                        s->in_run[ch] = false;
                    }
                }
            } else {
                if (drains) {
                    if (fifo_pop(s, buf, len) != len) {
                        error = 5;              /* engine underrun */
                        break;
                    }
                } else if (dev_mem) {
                    dma_memory_read(&address_space_memory, dev, buf, len,
                                    MEMTXATTRS_UNSPECIFIED);
                    dev += len;
                } else {
                    cdma_fifo_xfer(dev, width, buf, len, false);
                }
                if (crypt) {
                    ok = aes_apply(s, aes, buf, len, restart_crypto && (s->a6 || !resume));
                }
                dma_memory_write(&address_space_memory, c->addr, buf, len,
                                 MEMTXATTRS_UNSPECIFIED);
            }
            if (!ok) {
                error = 4;
                break;
            }
            c->addr += len;
            c->remain -= len;
        }
        if (c->in_seg) {
            c->ctrl |= ST_RUNNING;
            goto out;
        }
        c->desc = le32_to_cpu(d[0]);
        if (flags & DESC_LAST) {
            break;
        }
    }

    if (error) {
        uint32_t dd[4] = { 0 };
        dma_memory_read(&address_space_memory, c->desc, dd, sizeof(dd), MEMTXATTRS_UNSPECIFIED);
        qemu_log_mask(LOG_GUEST_ERROR, "s5l8930.cdma: ch %d error %u at "
                      "descriptor 0x%08x {next 0x%08x flags 0x%08x addr 0x%08x len 0x%x} settings 0x%x fifo 0x%x\n",
                      ch, error, c->desc, le32_to_cpu(dd[0]), le32_to_cpu(dd[1]), le32_to_cpu(dd[2]), le32_to_cpu(dd[3]), c->settings, c->fifo);
        c->error = error;
        c->ctrl |= ST_ERROR;
    }
    if (!error && to_device && dev_fifo && cdma_src(s, dev)) {
        /* The chain has filled the device's FIFO; it completes when the
         * device has taken it (s5l8930_cdma_sink_done from the FMI), as the
         * IOP firmware waits for after its NAND write. */
        if (!s->sink_early[ch]) {
            s->sink_pending[ch] = true;
            c->ctrl |= ST_RUNNING;
            goto out;
        }
        s->sink_early[ch] = false;      /* drained already: done now */
    }
    c->ctrl |= ST_DONE;
    cdma_update_irq(s, ch);
out:
    if (s->a6) {
        s->in_run[ch] = false;
    }
}

void s5l8930_cdma_sink_done(DeviceState *dev, uint32_t fifo_base, uint32_t size)
{
    S5L8930CDMAState *s = S5L8930_CDMA(dev);

    for (int ch = 1; ch < CDMA_CHANNELS; ch++) {
        CDMAChannel *c = &s->ch[ch];

        if (s->in_run[ch] && c->fifo >= fifo_base && c->fifo < fifo_base + size) {
            s->sink_early[ch] = true;   /* cdma_run completes it when it stops pushing */
        }
        if (s->sink_pending[ch] && !c->in_seg && c->fifo >= fifo_base && c->fifo < fifo_base + size) {
            s->sink_pending[ch] = false;
            c->ctrl = (c->ctrl & ~ST_RUNNING) | ST_DONE;
            cdma_update_irq(s, ch);
        }
    }
}

/*
 * Audio channels (device address in an I2S block) play out in real time: the
 * chain stays running and its data reaches the FIFO in CDMA_PACED_STEP_NS
 * steps at the port's rate (stereo S16), so the IOAudio engine's period interrupt and
 * position arrive when a real I2S would have consumed the ring.
 */
#define CDMA_PACED_STEP_NS  (1 * SCALE_MS)
#define CDMA_PACED_CONTINUE_NS (50 * SCALE_MS)

static bool cdma_is_paced(S5L8930CDMAState *s, int ch)
{
    return s->paced[ch];
}

/*
 * Walk the chain from its head. Move bytes [sent, upto) to the FIFO, point
 * +0x14/+0x10/+0x0C at the segment containing `upto` (AppleCDMA's stop path,
 * c044d517, panics unless +0x10 lies inside the segment +0x14 names), and
 * return true once `upto` has reached the end of the chain.
 */
static bool cdma_paced_advance(S5L8930CDMAState *s, int ch, uint64_t upto)
{
    CDMAChannel *c = &s->ch[ch];
    uint32_t width = 1u << ((c->settings >> 2) & 3);
    bool to_device = c->settings & SET_TO_DEVICE;
    uint32_t desc = s->paced_desc[ch];
    uint64_t base = 0;

    for (int n = 0; n < 4096; n++) {
        uint32_t d[4], len, addr;

        if (dma_memory_read(&address_space_memory, desc, d, sizeof(d),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
            (le32_to_cpu(d[1]) & DESC_TYPE_MASK) != DESC_DATA) {
            return true;
        }
        len = le32_to_cpu(d[3]);
        addr = le32_to_cpu(d[2]);
        if (to_device && s->paced_sent[ch] < base + len && upto > base) {
            uint32_t from = MAX(s->paced_sent[ch], base) - base;
            uint32_t to = MIN(upto, base + len) - base;
            g_autofree uint8_t *buf = g_malloc(to - from);

            dma_memory_read(&address_space_memory, addr + from, buf, to - from,
                            MEMTXATTRS_UNSPECIFIED);
            cdma_fifo_xfer(c->fifo, width, buf, to - from, true);
        } else if (!to_device && s->paced_sent[ch] < base + len && upto > base) {
            /* Capture (i2s RX FIFO -> memory), same pacing. */
            uint32_t from = MAX(s->paced_sent[ch], base) - base;
            uint32_t to = MIN(upto, base + len) - base;
            g_autofree uint8_t *buf = g_malloc(to - from);

            cdma_fifo_xfer(c->fifo, width, buf, to - from, false);
            dma_memory_write(&address_space_memory, addr + from, buf, to - from,
                             MEMTXATTRS_UNSPECIFIED);
        }
        if (upto < base + len) {
            c->desc = desc;
            c->addr = addr + (upto - base);
            c->remain = base + len - upto;
            s->paced_sent[ch] = upto;
            return false;
        }
        base += len;
        desc = le32_to_cpu(d[0]);
        if (le32_to_cpu(d[1]) & DESC_LAST) {
            /* Finished: +0x14 names the next descriptor, so +0x10/+0x0C must
             * describe it too, not the end of the last segment. A stop that
             * lands between this completion and the driver's next go reads
             * them and panics ("CDMA stop with MAR ... but command MA ...")
             * unless MAR lies inside that next segment. */
            c->desc = desc;
            c->addr = addr + len;
            c->remain = 0;
            if (dma_memory_read(&address_space_memory, desc, d, sizeof(d),
                                MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
                c->addr = le32_to_cpu(d[2]);
                c->remain = le32_to_cpu(d[3]);
            }
            s->paced_sent[ch] = base;
            return true;
        }
    }
    return true;
}

static uint64_t cdma_paced_pos(S5L8930CDMAState *s, int ch)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->paced_start[ch];
    CDMAChannel *c = &s->ch[ch];
    unsigned frame = s5l8930_i2s_frame_bytes((c->fifo - CDMA_PACED_LO) >> 12,
                                           !(c->settings & SET_TO_DEVICE));

    /* Whole PCM frames only: a stop mid-frame would leave the I2S
     * stream a byte or two out of step for every later sound. */
    uint64_t bytes = MAX(0, elapsed) * (uint64_t)s->paced_bps[ch] /
                     NANOSECONDS_PER_SECOND;
    return bytes - bytes % frame;
}

static void cdma_pace_arm(S5L8930CDMAState *s)
{
    for (int i = 0; i < CDMA_CHANNELS; i++) {
        if (cdma_is_paced(s, i)) {
            timer_mod(s->pace_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                     CDMA_PACED_STEP_NS);
            return;
        }
    }
    timer_del(s->pace_timer);
}

static void cdma_pace_tick(void *opaque)
{
    S5L8930CDMAState *s = opaque;

    for (int i = 0; i < CDMA_CHANNELS; i++) {
        if (cdma_is_paced(s, i) && cdma_paced_advance(s, i, cdma_paced_pos(s, i))) {
            s->paced[i] = false;
            s->paced_end[i] = s->paced_start[i] +
                s->paced_sent[i] * NANOSECONDS_PER_SECOND / s->paced_bps[i];
            s->ch[i].ctrl = (s->ch[i].ctrl & ~ST_RUNNING) | ST_DONE;
            cdma_update_irq(s, i);
        }
    }
    cdma_pace_arm(s);
}

/* Stop a paced chain where playback is, delivering what has played. */
static bool cdma_paced_stop(S5L8930CDMAState *s, int ch)
{
    if (!cdma_is_paced(s, ch)) {
        return false;
    }
    cdma_paced_advance(s, ch, cdma_paced_pos(s, ch));
    s->paced[ch] = false;
    s->paced_end[ch] = 0;               /* a stopped stream does not continue */
    cdma_pace_arm(s);
    return true;
}

static bool cdma_start_paced(S5L8930CDMAState *s, int ch)
{
    CDMAChannel *c = &s->ch[ch];

    if (c->fifo < CDMA_PACED_LO || c->fifo >= CDMA_PACED_HI) {
        return false;
    }
    c->ctrl = (c->ctrl & ~ST_ERROR) | ST_RUNNING;
    s->paced_desc[ch] = c->desc;
    s->paced_sent[ch] = 0;
    unsigned port = (c->fifo - CDMA_PACED_LO) >> 12;
    s->paced_bps[ch] = s5l8930_i2s_frame_bytes(port, !(c->settings & SET_TO_DEVICE)) *
                       s5l8930_i2s_sync_rate(port);
    s->paced_start[ch] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /*
     * A chain re-armed from the previous one's completion interrupt continues
     * the same sample clock: the I2S never paused, only the driver's go was
     * late (interrupt latency plus our 10 ms tick). Starting the clock at the
     * go instead dropped ~3% of the stream at every 64 KiB chain boundary --
     * gaps in playback and missing frames in capture.
     */
    if (s->paced_start[ch] - s->paced_end[ch] < CDMA_PACED_CONTINUE_NS) {
        s->paced_start[ch] = s->paced_end[ch];
    }
    s->paced[ch] = true;
    cdma_pace_arm(s);
    return true;
}

static uint64_t cdma_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930CDMAState *s = opaque;
    int ch = offset >> CDMA_CHAN_SHIFT;
    hwaddr reg = offset & 0xFFF;

    if (ch == 0) {
        switch (reg) {
        case 0x00: case 0x04:
            return s->enabled[reg >> 2];
        case 0x08: case 0x0C:
            return 0;
        case 0x10: case 0x14:
            /* Which channels are enabled: iBoot's and the IOP firmware's
             * enable helpers (iBoot-817 0x5ff012e0, EmbeddedIOP-20 fw 0xe724)
             * read it to set or clear only a bit that differs, and
             * AppleCDMA-300.8 (cdma-version 2) checks it before a channel's
             * CSR (9B206 807a47d0). */
            return s->enabled[(reg - 0x10) >> 2];
        default:
            break;
        }
    } else if (ch < s->num_channels) {
        CDMAChannel *c = &s->ch[ch];
        if (cdma_is_paced(s, ch)) {
            cdma_paced_advance(s, ch, cdma_paced_pos(s, ch));
        }
        switch (reg) {
        case CH_CTRL:
            return c->ctrl | (c->stopped ? ST_STOPPED : 0);
        case CH_SETTINGS: return c->settings;
        case CH_FIFO:     return c->fifo;
        case CH_REMAIN:   return c->remain;
        case CH_ADDR:     return c->addr;
        case CH_DESC:     return c->desc;
        case CH_ERROR:    return c->error;
        default:          break;
        }
    }
    qemu_log_mask(LOG_UNIMP, "s5l8930.cdma: unimplemented read 0x%" HWADDR_PRIx
                  "\n", offset);
    return 0;
}

static void cdma_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    S5L8930CDMAState *s = opaque;
    int ch = offset >> CDMA_CHAN_SHIFT;
    hwaddr reg = offset & 0xFFF;
    uint32_t v = value;

    trace_s5l8930_cdma_register(ch, reg, v);

    if (ch == 0) {
        switch (reg) {
        case 0x00: case 0x04:
            s->enabled[reg >> 2] |= v;
            for (int i = 1; i < CDMA_CHANNELS; i++) {
                cdma_update_irq(s, i);      /* the lines are levels: a completion that came first shows now */
            }
            break;
        case 0x08: case 0x0C:
            s->enabled[(reg - 8) >> 2] &= ~v;
            for (int i = 1; i < CDMA_CHANNELS; i++) {
                cdma_update_irq(s, i);
            }
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "s5l8930.cdma: unimplemented write 0x%"
                          HWADDR_PRIx " = 0x%x\n", offset, v);
            return;
        }
        for (int i = 1; i < CDMA_CHANNELS; i++) {
            cdma_update_irq(s, i);
        }
        return;
    }
    if (ch >= s->num_channels) {
        qemu_log_mask(LOG_UNIMP, "s5l8930.cdma: unimplemented write 0x%"
                      HWADDR_PRIx " = 0x%x\n", offset, v);
        return;
    }

    CDMAChannel *c = &s->ch[ch];
    if (trace_event_get_state_backends(TRACE_S5L8930_CDMA_LOG) && c->fifo >= CDMA_PACED_LO &&
        c->fifo < CDMA_PACED_HI) {
        TRACE_PRINTF(trace_s5l8930_cdma_log, "[CDMA] ch 0x%x W +%02x <- 0x%x (ctrl 0x%x paced %d)\n",
                ch, (unsigned)reg, v, c->ctrl, s->paced[ch]);
    }
    switch (reg) {
    case CH_CTRL: {
        bool was_held = c->ctrl & ST_HELD;

        if (cdma_waits_for_uart(s, c)) {
            trace_s5l8930_cdma_uart(ch, "control", c->ctrl, c->remain,
                                  c->addr, c->desc, v);
        }
        if (v & CTRL_RESET) {
            /* Configuration goes, but the descriptor and address pointers
             * survive: the abort sequence (|=4, poll 0x200000, write 2,
             * |=0x18, c044d46c) then reads +0x14 to find the descriptor it
             * stopped at and derefs the ring entry it computes from it. */
            cdma_paced_stop(s, ch);
            c->ctrl = c->settings = c->fifo = c->remain = c->error = 0;
            c->in_seg = false;
            break;
        }
        c->ctrl = (c->ctrl & ~(CTRL_CONFIG_MASK | (v & ST_W1C))) |
                  (v & CTRL_CONFIG_MASK);
        /*
         * Hold on a running channel parks it: the S5L8920 kernel's CDMA
         * driver stops a channel by setting HOLD and polling until the state
         * (bits 16-17) is no longer plain "running" (0x80509918 on 8C148a:
         * BTServer stopping uart3's RX). Unheld, the bit goes again.
         */
        c->ctrl &= ~ST_HELD;
        if ((c->ctrl & CTRL_HOLD) && (c->ctrl & ST_RUNNING)) {
            c->ctrl |= ST_HELD;
        }
        /* CDMA v2+ get-current-position (native10A4058088f730) requests
         * HOLD|ABORT, samples MAR/BC, then restores the original CSR without
         * GO. This is a pause/flush of the active transfer, not cancellation
         * of its descriptor. An unheld ABORT remains a terminal stop. */
        bool pause = s->a6 && (v & CTRL_HOLD) &&
                     (c->ctrl & ST_RUNNING);

        if ((v & CTRL_ABORT) && !pause) {
            c->ctrl = (c->ctrl & ~ST_RUNNING) | ST_ABORTED;
            c->in_seg = false;
            /*
             * The channel reads state 2 until its next go, the reset and the |= 0x18 of the abort sequence
             * included. 14G61's AppleCDMA interrupt action (0x80da1d30) runs on a work loop after its filter;
             * finding the channel's clock off, as a stop that came between the two leaves it, it takes the
             * interrupt as that stop's and panics ("CDMA %d should be stopped: %08x") unless the state is 2.
             * The channel start accepts 2 as well (0x80da1eca). The position query's HOLD | ABORT (0x80da2282) on a
             * channel that has finished leaves the state as it was.
             */
            c->stopped = s->a6 && !(v & CTRL_HOLD);
            cdma_paced_stop(s, ch);
        }
        if ((v & CTRL_GO) && !(v & CTRL_HOLD)) {
            c->stopped = false;
            if (trace_event_get_state_backends(TRACE_S5L8930_CDMA_LOG)) {
                TRACE_PRINTF(trace_s5l8930_cdma_log, "[CDMA] %.4f go ch 0x%x ctrl 0x%x set 0x%x fifo 0x%x "
                        "desc 0x%x\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                        ch, v, c->settings, c->fifo, c->desc);
            }
            if (cdma_waits_for_uart(s, c) && !cdma_src(s, c->fifo)) {
                uint32_t d[4];

                /* Parked on the first segment: stopping it checks MAR/BC.
                 * CDMA v2+ keeps the guest-programmed streaming byte counter
                 * across descriptors (AppleCDMA seeds it with 0x40000000).
                 * With no input, its initial value must remain unchanged.
                 * Replacing it with this segment's length invents progress
                 * when AppleSimpleUARTSync computes its receive-ring count.
                 */
                dma_memory_read(&address_space_memory, c->desc, d, sizeof(d),
                                MEMTXATTRS_UNSPECIFIED);
                c->addr = le32_to_cpu(d[2]);
                if (!s->a6) {
                    c->remain = le32_to_cpu(d[3]);
                }
                c->ctrl |= ST_RUNNING;
            } else if (!cdma_start_paced(s, ch)) {
                cdma_run(s, ch);
            }
        }
        if (s->a6 && was_held && !(c->ctrl & CTRL_HOLD) &&
            (c->ctrl & ST_RUNNING)) {
            qemu_bh_schedule(s->kick_bh);
        }
        break;
    }
    case CH_SETTINGS: c->settings = v; break;
    case CH_FIFO:     c->fifo = v; break;
    case CH_REMAIN:   c->remain = v; break;
    case CH_ADDR:     c->addr = v; break;
    case CH_DESC:     c->desc = v; break;
    case CH_ERROR:    c->error = 0; break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8930.cdma: unimplemented write 0x%"
                      HWADDR_PRIx " = 0x%x\n", offset, v);
        return;
    }
    cdma_update_irq(s, ch);
}

static const MemoryRegionOps cdma_ops = {
    .read = cdma_read,
    .write = cdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void s5l8930_cdma_set_source(DeviceState *dev, hwaddr base, hwaddr size,
                             uint32_t (*avail)(void *opaque, hwaddr addr, bool to_device),
                             void *opaque)
{
    S5L8930CDMAState *s = S5L8930_CDMA(dev);

    for (int i = 0; i < ARRAY_SIZE(s->src); i++) {
        if (!s->src[i].avail || s->src[i].base == base) {
            s->src[i].base = base;
            s->src[i].size = size;
            s->src[i].avail = avail;
            s->src[i].opaque = opaque;
            return;
        }
    }
    g_assert_not_reached();                     /* more sources than src[] */
}

/* The FIFO-fed device has more data: resume every channel stalled on it. */
void s5l8930_cdma_kick(DeviceState *dev)
{
    S5L8930CDMAState *s = S5L8930_CDMA(dev);

    for (int ch = 1; ch < CDMA_CHANNELS; ch++) {
        /* A FIFO can free space while its current push/pop is still on the
         * stack. Resume after the descriptor address and remaining byte count
         * have been committed, never recursively against their old values. */
        if (s->a6 && s->in_run[ch]) {
            qemu_bh_schedule(s->kick_bh);
            continue;
        }
        if ((s->ch[ch].ctrl & ST_RUNNING) && s->ch[ch].in_seg &&
            !(s->a6 && (s->ch[ch].ctrl & CTRL_HOLD))) {
            cdma_run(s, ch);
        }
    }
}

static void cdma_kick_deferred(void *opaque)
{
    s5l8930_cdma_kick(opaque);
}

/* ---- AES context registers ---- */

/* Apple US20120288089A1 describes iterative AES under a device key, with
 * a 32-byte seed/result and feedback across the two 16-byte blocks. This
 * candidate uses AES-256-CBC, a zero initial IV, continuous CBC feedback
 * between iterations, little-endian register words, and exactly COUNT
 * iterations. Those mode/feedback details remain an A6 hardware inference.
 * Native create and cold-load tests establish consistency, not fidelity. */
static bool aes_skg_candidate(S5L8930CDMAState *s)
{
    uint8_t data[32], iv[16] = { 0 };
    Error *err = NULL;
    QCryptoCipher *cipher;

    if (s->skg_slot != 7 || !s->skg_count || s->skg_count > 0x100000 ||
        !(s->secret_ctrl & (1u << 7))) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        stl_le_p(data + i * 4, s->secret_seed[i]);
    }
    cipher = qcrypto_cipher_new(QCRYPTO_CIPHER_ALGO_AES_256,
                               QCRYPTO_CIPHER_MODE_CBC,
                               cdma_uid_key, sizeof(cdma_uid_key), &err);
    if (!cipher) {
        error_report_err(err);
        return false;
    }
    if (qcrypto_cipher_setiv(cipher, iv, sizeof(iv), NULL) < 0) {
        qcrypto_cipher_free(cipher);
        return false;
    }
    for (uint32_t i = 0; i < s->skg_count; i++) {
        if (qcrypto_cipher_encrypt(cipher, data, data, sizeof(data), NULL) < 0) {
            qcrypto_cipher_free(cipher);
            return false;
        }
    }
    qcrypto_cipher_free(cipher);
    memcpy(s->secret_key[7], data, sizeof(data));
    s->secret_valid |= 1u << 7;
    return true;
}

static uint64_t aes_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930CDMAState *s = opaque;
    int ctx = offset >> CDMA_CHAN_SHIFT;
    hwaddr reg = offset & 0xFFF;

    if (!ctx && s->skg_probe) {
        switch (reg) {
        case 0xfc: return s->secret_ctrl;
        case 0x420: return s->skg_slot;
        case 0x424: return s->skg_count;
        case 0x428: return s->skg_control;
        case 0x42c: return 0; /* synchronous candidate has no queued work */
        case 0x430: return s->skg_status;
        }
        if ((reg >= 0x100 && reg < 0x200) ||
            (reg >= 0x400 && reg < 0x420)) {
            return 0; /* seed and secret keys are not readable by software */
        }
    }
    if (ctx < AES_CONTEXTS) {
        AESContext *c = &s->aes[ctx];
        if (reg == AES_SETUP) {
            return c->setup;
        }
        if (reg >= AES_IV && reg < AES_IV + 16) {
            return c->iv[(reg - AES_IV) >> 2];
        }
        if (reg >= AES_KEY && reg < AES_KEY + 32) {
            return 0;                       /* keys are write-only */
        }
    }
    qemu_log_mask(LOG_UNIMP, "s5l8930.aes: unimplemented read 0x%" HWADDR_PRIx
                  "\n", offset);
    return 0;
}

static void aes_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    S5L8930CDMAState *s = opaque;
    int ctx = offset >> CDMA_CHAN_SHIFT;
    hwaddr reg = offset & 0xFFF;
    uint32_t v = value;

    if (!ctx && s->skg_probe) {
        if (reg >= 0x400 && reg < 0x420) {
            s->secret_seed[(reg - 0x400) >> 2] = v;
            return;
        }
        if (reg >= 0x100 && reg < 0x1e0) {
            unsigned slot = (reg - 0x100) / 32;
            unsigned word = ((reg - 0x100) % 32) / 4;
            if (!(s->secret_ctrl & ((1u << (slot + 8)) |
                                    (1u << (slot + 16))))) {
                stl_le_p(s->secret_key[slot] + word * 4, v);
                s->secret_valid |= 1u << slot;
            }
            return;
        }
        switch (reg) {
        case 0xfc: s->secret_ctrl |= v; return;
        case 0x420: s->skg_slot = v; return;
        case 0x424: s->skg_count = v; return;
        case 0x428:
            s->skg_control = v;
            if (v == 1) {
                s->secret_valid &= ~(1u << 7);
                s->skg_status |= aes_skg_candidate(s) ? 2 : 1;
            } else if (v) {
                s->skg_status |= 1;
            }
            return;
        case 0x430: s->skg_status &= ~(v & 3); return;
        }
    }
    if (ctx < AES_CONTEXTS) {
        AESContext *c = &s->aes[ctx];
        if (reg == AES_SETUP) {
            /* Context 0 holds the sticky key-disable bits. */
            c->setup = ctx ? v : (c->setup | v);
            return;
        }
        if (reg >= AES_IV && reg < AES_IV + 16) {
            c->iv[(reg - AES_IV) >> 2] = v;
            return;
        }
        if (reg >= AES_KEY && reg < AES_KEY + 32) {
            c->key[(reg - AES_KEY) >> 2] = v;
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "s5l8930.aes: unimplemented write 0x%"
                  HWADDR_PRIx " = 0x%x\n", offset, v);
}

static const MemoryRegionOps aes_ops = {
    .read = aes_read,
    .write = aes_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---- device ---- */

static void s5l8930_cdma_reset(DeviceState *dev)
{
    S5L8930CDMAState *s = S5L8930_CDMA(dev);
    qemu_bh_cancel(s->kick_bh);
    memset(s->in_run, 0, sizeof(s->in_run));
    memset(s->sink_early, 0, sizeof(s->sink_early));
    memset(s->sink_pending, 0, sizeof(s->sink_pending));

    /* Version 1 has no enable block: its drivers never write one, every channel is live. */
    memset(s->enabled, s->version == 1 ? 0xff : 0, sizeof(s->enabled));
    memset(s->ch, 0, sizeof(s->ch));
    memset(s->aes, 0, sizeof(s->aes));
    s->secret_ctrl = s->skg_slot = s->skg_count = 0;
    s->skg_control = s->skg_status = s->secret_valid = 0;
    memset(s->secret_seed, 0, sizeof(s->secret_seed));
    memset(s->secret_key, 0, sizeof(s->secret_key));
    g_free(s->fifo);
    s->fifo = NULL;
    s->fifo_len = 0;
    memset(s->paced, 0, sizeof(s->paced));
    memset(s->paced_end, 0, sizeof(s->paced_end));
    timer_del(s->pace_timer);
    for (int i = 0; i < CDMA_CHANNELS; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
}

static void s5l8930_cdma_init(Object *obj)
{
    S5L8930CDMAState *s = S5L8930_CDMA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->cdma_mem, obj, &cdma_ops, s, "s5l8930.cdma",
                          S5L8930_CDMA_SIZE);
    sysbus_init_mmio(sbd, &s->cdma_mem);
    memory_region_init_io(&s->aes_mem, obj, &aes_ops, s, "s5l8930.aes",
                          S5L8930_AES_SIZE);
    sysbus_init_mmio(sbd, &s->aes_mem);
    for (int i = 0; i < CDMA_CHANNELS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    s->pace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdma_pace_tick, s);
    s->kick_bh = qemu_bh_new(cdma_kick_deferred, s);
}

static const VMStateDescription vmstate_cdma_channel = {
    .name = "s5l8930.cdma.channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, CDMAChannel),
        VMSTATE_UINT32(settings, CDMAChannel),
        VMSTATE_UINT32(fifo, CDMAChannel),
        VMSTATE_UINT32(remain, CDMAChannel),
        VMSTATE_UINT32(addr, CDMAChannel),
        VMSTATE_UINT32(desc, CDMAChannel),
        VMSTATE_UINT32(error, CDMAChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_aes_context = {
    .name = "s5l8930.cdma.aes",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(setup, AESContext),
        VMSTATE_UINT32_ARRAY(iv, AESContext, 4),
        VMSTATE_UINT32_ARRAY(key, AESContext, 8),
        VMSTATE_UINT8_ARRAY(chain, AESContext, 16),
        VMSTATE_END_OF_LIST()
    }
};

/* Runs between fifo_len and the buffer it sizes, so a bad length fails
 * before anything is allocated. */
static bool cdma_fifo_len_ok(void *opaque, int version_id)
{
    return ((S5L8930CDMAState *)opaque)->fifo_len <= CDMA_MAX_SEGMENT;
}

static int cdma_pre_save(void *opaque)
{
    S5L8930CDMAState *s = opaque;

    for (int ch = 0; ch < CDMA_CHANNELS; ch++) {
        s->in_seg_mig[ch] = s->ch[ch].in_seg;
    }
    return cdma_fifo_len_ok(s, 0) ? 0 : -E2BIG;  /* what a load would refuse (and VALIDATE asserts on) */
}

static int cdma_pre_load(void *opaque)
{
    S5L8930CDMAState *s = opaque;

    qemu_bh_cancel(s->kick_bh);
    memset(s->in_run, 0, sizeof(s->in_run));
    memset(s->sink_early, 0, sizeof(s->sink_early));
    memset(s->in_seg_mig, 0, sizeof(s->in_seg_mig));     /* older streams: nothing stalled */
    memset(s->sink_pending, 0, sizeof(s->sink_pending));
    return 0;
}

static int cdma_post_load(void *opaque, int version_id)
{
    S5L8930CDMAState *s = opaque;

    for (int ch = 0; ch < CDMA_CHANNELS; ch++) {
        /* A stalled segment resumes with remain as its length (cdma_run's
         * bound); a paced chain divides by its rate. */
        if ((s->in_seg_mig[ch] && s->ch[ch].remain > CDMA_MAX_SEGMENT) ||
            (s->paced[ch] && !s->paced_bps[ch])) {
            return -EINVAL;
        }
        s->ch[ch].in_seg = s->in_seg_mig[ch];
    }
    /* A deferred FIFO notification is host execution state; reconstruct it
     * from the restored channels after all device state has been loaded. The A6's: the other boards restore
     * without one, as they were qualified. */
    if (s->a6) {
        qemu_bh_schedule(s->kick_bh);
    }
    return 0;
}

static const VMStateDescription vmstate_s5l8930_cdma = {
    .name = "s5l8930.cdma",
    .version_id = 4,
    .minimum_version_id = 1,
    .pre_save = cdma_pre_save,
    .pre_load = cdma_pre_load,
    .post_load = cdma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(enabled, S5L8930CDMAState, 2),
        VMSTATE_UINT32(fifo_len, S5L8930CDMAState),
        VMSTATE_VALIDATE("fifo_len", cdma_fifo_len_ok),
        VMSTATE_VBUFFER_ALLOC_UINT32(fifo, S5L8930CDMAState, 0, NULL, fifo_len),
        VMSTATE_STRUCT_SUB_ARRAY(ch, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 1,
                             vmstate_cdma_channel, CDMAChannel),
        VMSTATE_STRUCT_ARRAY(aes, S5L8930CDMAState, AES_CONTEXTS, 1,
                             vmstate_aes_context, AESContext),
        VMSTATE_SUB_ARRAY(paced, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 2, vmstate_info_bool, bool),
        VMSTATE_SUB_ARRAY(paced_start, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 2, vmstate_info_int64, int64_t),
        VMSTATE_SUB_ARRAY(paced_end, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 3, vmstate_info_int64, int64_t),
        VMSTATE_SUB_ARRAY(paced_desc, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 2, vmstate_info_uint32, uint32_t),
        VMSTATE_SUB_ARRAY(paced_sent, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 2, vmstate_info_uint64, uint64_t),
        VMSTATE_SUB_ARRAY(paced_bps, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 2, vmstate_info_uint32, uint32_t),
        VMSTATE_TIMER_PTR_V(pace_timer, S5L8930CDMAState, 2),
        /* 4: the IOP core's chains, mid-transfer between its instructions */
        VMSTATE_SUB_ARRAY(sink_pending, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 4, vmstate_info_bool, bool),
        VMSTATE_SUB_ARRAY(in_seg_mig, S5L8930CDMAState, 0, CDMA_LEGACY_CHANNELS, 4, vmstate_info_bool, bool),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8930_cdma_realize(DeviceState *dev, Error **errp)
{
    S5L8930CDMAState *s = S5L8930_CDMA(dev);
    g_autoptr(GError) error = NULL;

    if (s->num_channels < 2 || s->num_channels > CDMA_CHANNELS) {
        error_setg(errp, "CDMA num-channels must be between 2 and %u", CDMA_CHANNELS);
        return;
    }
    memory_region_set_size(&s->cdma_mem, (uint64_t)s->num_channels << CDMA_CHAN_SHIFT);
    if (s->num_channels != CDMA_LEGACY_CHANNELS || s->skg_probe) {
        /* Keep the existing A4 migration wire unchanged. The experimental
         * A6 variant must not silently lose its additional channel state. */
        error_setg(&s->migration_blocker, "CDMA extended-channel migration is not implemented");
        if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
            return;
        }
    }
    if (!s->gid_path) {
        return;
    }
    if (!g_file_get_contents(s->gid_path, (char **)&s->gid_data,
                             &s->gid_size, &error)) {
        error_setg(errp, "gid-blobs: %s", error->message);
        return;
    }
    if (!s->gid_size || s->gid_size % (2 * GID_BLOB_SIZE)) {
        error_setg(errp, "gid-blobs: expected nonempty 96-byte KBAG || IV-key records");
    }
}

static void s5l8930_cdma_finalize(Object *obj)
{
    S5L8930CDMAState *s = S5L8930_CDMA(obj);

    migrate_del_blocker(&s->migration_blocker);
    g_free(s->gid_data);
    timer_free(s->pace_timer);
    qemu_bh_delete(s->kick_bh);
    g_free(s->fifo);
}

static const Property s5l8930_cdma_properties[] = {
    DEFINE_PROP_STRING("gid-blobs", S5L8930CDMAState, gid_path),
    DEFINE_PROP_UINT64("dram-base", S5L8930CDMAState, dram_base, S5L8930_DRAM_BASE),
    DEFINE_PROP_UINT64("dram-size", S5L8930CDMAState, dram_size, S5L8930_DRAM_SIZE),   /* the board's */
    DEFINE_PROP_UINT64("uart-base", S5L8930CDMAState, uart_base, S5L8930_UART_BASE(0)),
    DEFINE_PROP_UINT32("uart-ports", S5L8930CDMAState, uart_ports, 6),
    DEFINE_PROP_UINT32("num-channels", S5L8930CDMAState, num_channels, CDMA_LEGACY_CHANNELS),
    DEFINE_PROP_UINT8("version", S5L8930CDMAState, version, 2),
    DEFINE_PROP_BOOL("a6", S5L8930CDMAState, a6, false),
    DEFINE_PROP_BOOL("skg-probe", S5L8930CDMAState, skg_probe, false),
    /* Legacy page-store images omit all inline NAND encryption. A6's native
     * protected images retain their additional encrypted-file layer. */
    DEFINE_PROP_BOOL("nand-file-aes", S5L8930CDMAState, nand_file_aes, false),
    DEFINE_PROP_UINT32("paced-base", S5L8930CDMAState, paced_base, S5L8930_I2S_BASE(0)),
    DEFINE_PROP_UINT32("paced-ports", S5L8930CDMAState, paced_ports, 3),
};

static void s5l8930_cdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8930_cdma_reset);
    dc->vmsd = &vmstate_s5l8930_cdma;
    dc->realize = s5l8930_cdma_realize;
    device_class_set_props(dc, s5l8930_cdma_properties);
}

static const TypeInfo s5l8930_cdma_info = {
    .name = TYPE_S5L8930_CDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930CDMAState),
    .instance_init = s5l8930_cdma_init,
    .instance_finalize = s5l8930_cdma_finalize,
    .class_init = s5l8930_cdma_class_init,
};

static void s5l8930_cdma_register_types(void)
{
    type_register_static(&s5l8930_cdma_info);
}

type_init(s5l8930_cdma_register_types)
