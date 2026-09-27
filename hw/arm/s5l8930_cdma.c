/*
 * S5L8930 ("A4") CDMA: the shared descriptor DMA engine and its AES filter.
 *
 * Contract: docs/ipad1/research/gap-kernel-platform-mmio.md §5 and
 * ref-a4-soc.md §1.10, re-checked against the 7B500 AppleCDMA kext
 * (start c044c2e8, channel set/clear c044c02c, _startChannel c044d590,
 * startChannel/resume c044d7e4, abort c044d46c, interrupt c044d74c,
 * AES ctx setup c044dc24/c044df74, IV c044d9b4, key c044da40,
 * _mapKeyID c044c850).
 *
 * Global window (offsets < 0x1000): +0x00/+0x04 channel-enable set,
 * +0x08/+0x0C clear, +0x10/+0x14 pending-interrupt bitmap. Channel n lives
 * at n << 12 (n = 1..0x25; channel 0's slot is the global block):
 *   +0x00 ctrl/status: write bit0 go, bit1 reset, bit2 abort, bit5 hold;
 *                      bits 16-17 state (1 = running), 0x40000 error,
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
 * descriptors. Only the draining channel interrupts; the feeding channel's
 * done bit is polled/reset by the driver, never acked.
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
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "crypto/cipher.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "qemu/timer.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930CDMAState, S5L8930_CDMA)

#define CDMA_CHANNELS       S5L8930_CDMA_CHANNELS
#define CDMA_CHAN_SHIFT     12
#define AES_CONTEXTS        9

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
#define CTRL_HOLD           (1u << 5)
#define CTRL_CONFIG_MASK    0xFFF8u         /* bits the guest owns */
#define CTRL_AES_CTX(ctrl)  (((ctrl) >> 8) & 0xFF)
#define ST_RUNNING          (1u << 16)
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
 * key too), so on a FIFO-fed NAND channel the IV and the AES step are skipped.
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
    /* stalled inside the segment at desc; addr/remain live. Not migrated:
     * only iBoot's NAND reads stall, never anything a snapshot catches. */
    bool in_seg;
} CDMAChannel;

typedef struct AESContext {
    uint32_t setup;
    uint32_t iv[4];
    uint32_t key[8];
    uint8_t chain[16];      /* running CBC IV */
} AESContext;

struct S5L8930CDMAState {
    SysBusDevice parent_obj;
    MemoryRegion cdma_mem;
    MemoryRegion aes_mem;
    qemu_irq irq[CDMA_CHANNELS];

    uint32_t enabled[2];
    CDMAChannel ch[CDMA_CHANNELS];
    AESContext aes[AES_CONTEXTS];
    /* engine output queue: filled by the feeding channel, drained by ch2 */
    uint8_t *fifo;
    uint32_t fifo_len;
    bool gid_warned;
    /* a device FIFO that paces its channel (s5l8930_cdma_set_source) */
    hwaddr src_base, src_size;
    uint32_t (*src_avail)(void *opaque, hwaddr addr);
    void *src_opaque;
    /* Audio channels (I2S FIFOs) play out in real time, not inside the go
     * write; paced[] marks a chain in flight. */
    bool paced[CDMA_CHANNELS];
    int64_t paced_start[CDMA_CHANNELS];
    int64_t paced_end[CDMA_CHANNELS];       /* when the last chain's final byte played */
    uint32_t paced_desc[CDMA_CHANNELS];     /* chain head the go named */
    uint64_t paced_sent[CDMA_CHANNELS];     /* bytes already in the FIFO */
    uint32_t paced_bps[CDMA_CHANNELS];      /* bytes/s: 4 * port frame rate */
    QEMUTimer *pace_timer;
};

/* i2s0-2 TX/RX FIFOs; stereo S16 at the port's rate (see s5l8930_i2s.c). */
#define CDMA_PACED_LO       S5L8930_I2S_BASE(0)
#define CDMA_PACED_HI       (S5L8930_I2S_BASE(2) + 0x1000)

/* ---- AES filter ---- */

/* Not the fused UID: any fixed value works as long as this machine keeps it. */
static const uint8_t cdma_uid_key[32] = {
    0x4b, 0x34, 0x38, 0x41, 0x50, 0x2d, 0x55, 0x49, 0x44, 0x2d, 0x53, 0x35,
    0x4c, 0x38, 0x39, 0x33, 0x30, 0x2d, 0x69, 0x50, 0x61, 0x64, 0x31, 0x2d,
    0x37, 0x42, 0x35, 0x30, 0x30, 0x2d, 0x30, 0x31,
};

/*
 * GID stand-in, the iPod machine's approach: the boot chain only ever runs
 * the GID key over an img3's KBAG (IV || 256-bit key, 48 bytes), and those
 * plaintexts are published per build. Ciphertexts read from the 7B500 IPSW.
 */
#define GID_BLOB_SIZE 48
typedef struct GidBlob {
    const char *name;
    uint8_t kbag[GID_BLOB_SIZE];
    uint8_t plain[GID_BLOB_SIZE];
} GidBlob;

static const GidBlob gid_blobs[] = {
#include "s5l8930_cdma_kbags.inc"
};

static const GidBlob *gid_lookup(const uint8_t *buf, uint32_t len)
{
    if (len < GID_BLOB_SIZE) {
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(gid_blobs); i++) {
        if (memcmp(gid_blobs[i].kbag, buf, GID_BLOB_SIZE) == 0) {
            return &gid_blobs[i];
        }
    }
    return NULL;
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
        algo = algos[AES_KEYLEN(c->setup)];
    } else if (c->setup & AES_KEY_GID) {
        const GidBlob *b = encrypt ? NULL : gid_lookup(buf, len);
        if (b) {
            memcpy(buf, b->plain, GID_BLOB_SIZE);
            if (len > GID_BLOB_SIZE) {
                memset(buf + GID_BLOB_SIZE, 0, len - GID_BLOB_SIZE);
            }
            return true;
        }
        if (!s->gid_warned) {
            s->gid_warned = true;
            warn_report("s5l8930.cdma: GID %scrypt of %u bytes not in the "
                        "KBAG table; using a stand-in key",
                        encrypt ? "en" : "de", len);
        }
        key = cdma_uid_key;
        algo = QCRYPTO_CIPHER_ALGO_AES_256;
    } else {
        key = cdma_uid_key;
        algo = QCRYPTO_CIPHER_ALGO_AES_256;
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
    memcpy(c->chain, last, 16);
    qcrypto_cipher_free(cipher);
    return true;
}

/* ---- channel engine ---- */

static bool cdma_is_memory(uint32_t addr);

/* Reading a device FIFO that fills as it goes (iBoot's NAND), not feeding
 * the AES engine. Distinct from the time-paced audio channels below. */
static bool cdma_fifo_fed(S5L8930CDMAState *s, CDMAChannel *c)
{
    return s->src_avail && !cdma_is_memory(c->fifo) &&
           !(c->settings & SET_TO_DEVICE) &&
           c->fifo >= s->src_base && c->fifo - s->src_base < s->src_size;
}

static void cdma_update_irq(S5L8930CDMAState *s, int ch)
{
    bool en = s->enabled[ch >> 5] & (1u << (ch & 31));
    /* The AES-feeding channel's line is never acked by the driver: an
     * asserted level there re-enters the draining channel's handler after it
     * has finished and panics ("CDMA M2M unexpected interrupt"). */
    if (CTRL_AES_CTX(s->ch[ch].ctrl) && !cdma_fifo_fed(s, &s->ch[ch])) {
        en = false;
    }
    qemu_set_irq(s->irq[ch], en && (s->ch[ch].ctrl & (ST_DONE | ST_ERROR)));
}

static void fifo_push(S5L8930CDMAState *s, const uint8_t *buf, uint32_t len)
{
    s->fifo = g_realloc(s->fifo, s->fifo_len + len);
    memcpy(s->fifo + s->fifo_len, buf, len);
    s->fifo_len += len;
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
 * ponytail: stays running forever; hook the UART's receive path in here
 * when a device (the BCM4329) actually sends something.
 */
static bool cdma_waits_for_uart(const CDMAChannel *c)
{
    return !(c->settings & SET_TO_DEVICE) &&
           c->fifo >= S5L8930_UART_BASE(0) && c->fifo < S5L8930_UART_BASE(6) &&
           (c->fifo & 0xfffff) == 0x24;
}

static bool cdma_is_memory(uint32_t addr)
{
    return addr >= S5L8930_DRAM_BASE &&
           addr < S5L8930_DRAM_BASE + S5L8930_DRAM_SIZE;
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
    CDMAChannel *c = &s->ch[ch];
    int ctx = CTRL_AES_CTX(c->ctrl);
    uint32_t dev = c->fifo;
    bool dev_mem = cdma_is_memory(dev);
    bool fed = cdma_fifo_fed(s, c);
    AESContext *aes = (ctx > 0 && ctx < AES_CONTEXTS && !fed) ?
                      &s->aes[ctx] : NULL;
    bool feeds = aes != NULL;                   /* memory -> AES engine */
    bool drains = !feeds && !c->fifo && !c->settings;   /* engine -> memory */
    bool to_device = feeds || (c->settings & SET_TO_DEVICE);
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
        if ((flags & DESC_TYPE_MASK) == DESC_IV && fed) {
            /* iBoot's per-page IV for the NAND data key; see DESC_IV. */
            c->desc = le32_to_cpu(d[0]);
            continue;
        }
        if ((flags & DESC_TYPE_MASK) != DESC_DATA || len > CDMA_MAX_SEGMENT) {
            error = 2;
            break;
        }
        if (!resume) {
            c->addr = le32_to_cpu(d[2]);
            c->remain = len;
        }
        c->in_seg = false;
        len = c->remain;
        if (fed) {
            /* Take what the device has; stall (still running) for the rest. */
            uint32_t avail = s->src_avail(s->src_opaque, dev) & ~(width - 1);
            if (avail < len) {
                len = avail;
                c->in_seg = true;
            }
        }
        if (len) {
            g_autofree uint8_t *buf = g_malloc(len);
            bool ok = true;

            if ((flags & DESC_AES) && !fed) {
                aes = aes ? aes : aes_for_channel(s, ch);
                if (!aes) {
                    error = 3;
                    break;
                }
            }
            if (to_device) {
                dma_memory_read(&address_space_memory, c->addr, buf, len,
                                MEMTXATTRS_UNSPECIFIED);
                if (flags & DESC_AES) {
                    ok = aes_apply(s, aes, buf, len, flags & DESC_AES_RESTART);
                }
                if (feeds) {
                    fifo_push(s, buf, len);
                } else if (dev_mem) {
                    dma_memory_write(&address_space_memory, dev, buf, len,
                                     MEMTXATTRS_UNSPECIFIED);
                    dev += len;
                } else {
                    cdma_fifo_xfer(dev, width, buf, len, true);
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
                if ((flags & DESC_AES) && !fed) {
                    ok = aes_apply(s, aes, buf, len,
                                   !resume && (flags & DESC_AES_RESTART));
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
            return;
        }
        c->desc = le32_to_cpu(d[0]);
        if (flags & DESC_LAST) {
            break;
        }
    }

    if (error) {
        qemu_log_mask(LOG_GUEST_ERROR, "s5l8930.cdma: ch %d error %u at "
                      "descriptor 0x%08x\n", ch, error, c->desc);
        c->error = error;
        c->ctrl |= ST_ERROR;
    }
    c->ctrl |= ST_DONE;
    cdma_update_irq(s, ch);
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

    /* Whole stereo S16 frames only: a stop mid-frame would leave the I2S
     * stream a byte or two out of step for every later sound. */
    return (MAX(0, elapsed) * (uint64_t)s->paced_bps[ch] /
            NANOSECONDS_PER_SECOND) & ~3ull;
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
    s->paced_bps[ch] = 4 * s5l8930_i2s_rate((c->fifo - CDMA_PACED_LO) >> 12);
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
        case 0x10: case 0x14: {
            uint32_t pend = 0;
            int base = (reg == 0x10) ? 0 : 32;
            for (int i = base; i < MIN(base + 32, CDMA_CHANNELS); i++) {
                if (s->ch[i].ctrl & (ST_DONE | ST_ERROR)) {
                    pend |= 1u << (i - base);
                }
            }
            return pend;
        }
        default:
            break;
        }
    } else if (ch < CDMA_CHANNELS) {
        CDMAChannel *c = &s->ch[ch];
        if (cdma_is_paced(s, ch)) {
            cdma_paced_advance(s, ch, cdma_paced_pos(s, ch));
        }
        switch (reg) {
        case CH_CTRL:     return c->ctrl;
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

    if (ch == 0) {
        switch (reg) {
        case 0x00: case 0x04:
            s->enabled[reg >> 2] |= v;
            break;
        case 0x08: case 0x0C:
            s->enabled[(reg - 8) >> 2] &= ~v;
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
    if (ch >= CDMA_CHANNELS) {
        qemu_log_mask(LOG_UNIMP, "s5l8930.cdma: unimplemented write 0x%"
                      HWADDR_PRIx " = 0x%x\n", offset, v);
        return;
    }

    CDMAChannel *c = &s->ch[ch];
    if (getenv("S5L8930_CDMA_TRACE") && c->fifo >= CDMA_PACED_LO &&
        c->fifo < CDMA_PACED_HI) {
        fprintf(stderr, "[CDMA] ch 0x%x W +%02x <- 0x%x (ctrl 0x%x paced %d)\n",
                ch, (unsigned)reg, v, c->ctrl, s->paced[ch]);
    }
    switch (reg) {
    case CH_CTRL:
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
        if (v & CTRL_ABORT) {
            c->ctrl = (c->ctrl & ~ST_RUNNING) | ST_ABORTED;
            c->in_seg = false;
            cdma_paced_stop(s, ch);
        }
        if ((v & CTRL_GO) && !(v & CTRL_HOLD)) {
            if (getenv("S5L8930_CDMA_TRACE")) {
                fprintf(stderr, "[CDMA] %.4f go ch 0x%x ctrl 0x%x set 0x%x fifo 0x%x "
                        "desc 0x%x\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                        ch, v, c->settings, c->fifo, c->desc);
            }
            if (cdma_waits_for_uart(c)) {
                uint32_t d[4];

                /* Parked on the first segment: stopping it checks MAR/BC. */
                dma_memory_read(&address_space_memory, c->desc, d, sizeof(d),
                                MEMTXATTRS_UNSPECIFIED);
                c->addr = le32_to_cpu(d[2]);
                c->remain = le32_to_cpu(d[3]);
                c->ctrl |= ST_RUNNING;
            } else if (!cdma_start_paced(s, ch)) {
                cdma_run(s, ch);
            }
        }
        break;
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
                             uint32_t (*avail)(void *opaque, hwaddr addr),
                             void *opaque)
{
    S5L8930CDMAState *s = S5L8930_CDMA(dev);

    s->src_base = base;
    s->src_size = size;
    s->src_avail = avail;
    s->src_opaque = opaque;
}

/* The FIFO-fed device has more data: resume every channel stalled on it. */
void s5l8930_cdma_kick(DeviceState *dev)
{
    S5L8930CDMAState *s = S5L8930_CDMA(dev);

    for (int ch = 1; ch < CDMA_CHANNELS; ch++) {
        if ((s->ch[ch].ctrl & ST_RUNNING) && s->ch[ch].in_seg) {
            cdma_run(s, ch);
        }
    }
}

/* ---- AES context registers ---- */

static uint64_t aes_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930CDMAState *s = opaque;
    int ctx = offset >> CDMA_CHAN_SHIFT;
    hwaddr reg = offset & 0xFFF;

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

    memset(s->enabled, 0, sizeof(s->enabled));
    memset(s->ch, 0, sizeof(s->ch));
    memset(s->aes, 0, sizeof(s->aes));
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

static const VMStateDescription vmstate_s5l8930_cdma = {
    .name = "s5l8930.cdma",
    .version_id = 3,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(enabled, S5L8930CDMAState, 2),
        VMSTATE_UINT32(fifo_len, S5L8930CDMAState),
        VMSTATE_VBUFFER_ALLOC_UINT32(fifo, S5L8930CDMAState, 0, NULL, fifo_len),
        VMSTATE_STRUCT_ARRAY(ch, S5L8930CDMAState, CDMA_CHANNELS, 1,
                             vmstate_cdma_channel, CDMAChannel),
        VMSTATE_STRUCT_ARRAY(aes, S5L8930CDMAState, AES_CONTEXTS, 1,
                             vmstate_aes_context, AESContext),
        VMSTATE_BOOL_ARRAY_V(paced, S5L8930CDMAState, CDMA_CHANNELS, 2),
        VMSTATE_INT64_ARRAY_V(paced_start, S5L8930CDMAState,
                              CDMA_CHANNELS, 2),
        VMSTATE_INT64_ARRAY_V(paced_end, S5L8930CDMAState,
                              CDMA_CHANNELS, 3),
        VMSTATE_UINT32_ARRAY_V(paced_desc, S5L8930CDMAState,
                               CDMA_CHANNELS, 2),
        VMSTATE_UINT64_ARRAY_V(paced_sent, S5L8930CDMAState,
                               CDMA_CHANNELS, 2),
        VMSTATE_UINT32_ARRAY_V(paced_bps, S5L8930CDMAState,
                               CDMA_CHANNELS, 2),
        VMSTATE_TIMER_PTR_V(pace_timer, S5L8930CDMAState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8930_cdma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8930_cdma_reset);
    dc->vmsd = &vmstate_s5l8930_cdma;
}

static const TypeInfo s5l8930_cdma_info = {
    .name = TYPE_S5L8930_CDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930CDMAState),
    .instance_init = s5l8930_cdma_init,
    .class_init = s5l8930_cdma_class_init,
};

static void s5l8930_cdma_register_types(void)
{
    type_register_static(&s5l8930_cdma_info);
}

type_init(s5l8930_cdma_register_types)
