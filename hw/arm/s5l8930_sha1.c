/*
 * S5L8930 ("A4") SHA-1 engine at 0x80100000, fed by CDMA channel 4.
 *
 * Contract from the 7B500 AppleS5L8920XSHA1 kext (start c06bf1fc, op
 * c06bf5ac, IV setter c06bf1ec, padding c06bf134, digest c06bf4fc): the
 * driver pads in software and only ever streams whole 64-byte blocks, so
 * the engine is a bare compression function.
 *   +0x00  command: bit1 start; bit3 continue from the state in +0x20..0x30
 *          (otherwise the standard initial value)
 *   +0x04  write 1: reset
 *   +0x10  mode/length; the kext writes 0
 *   +0x20  H0..H4, each stored byte-swapped (the kext rev()s on both the
 *          preload write and the digest read)
 *   +0xA0  data FIFO, written a word at a time by CDMA channel 4
 * Completion is signaled by the DMA channel, not by this block; the kext
 * never touches IRQ 0x25.
 *
 * iBoot (817.29 sha1 driver at 0x5ff090c4, register table 0x5ff296a8) feeds
 * it by PIO instead: one block in +0x40..+0x7C, then start (bit 3 as above),
 * polling bit 0 busy, which never shows since a block completes at once.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bitops.h"
#include "hw/core/sysbus.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930SHA1State, S5L8930_SHA1)

#define SHA1_CMD        0x00
#define SHA1_RESET      0x04
#define SHA1_MODE       0x10
#define SHA1_HASH       0x20
#define SHA1_FIFO       0xA0
#define SHA1_DATA       0x40        /* PIO block, 16 words */

#define CMD_START       (1u << 1)
#define CMD_CONTINUE    (1u << 3)

struct S5L8930SHA1State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t cmd, mode;
    uint32_t h[5];
    uint8_t block[64];
    uint32_t fill;
    uint64_t pio_words;     /* bitmap of the +0x40 words written */
};

static const uint32_t sha1_init[5] = {
    0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0,
};

static void sha1_compress(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 16; i++) {
        w[i] = ldl_be_p(block + 4 * i);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

static uint64_t sha1_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930SHA1State *s = opaque;

    switch (offset) {
    case SHA1_CMD:
        return s->cmd & ~CMD_START;
    case SHA1_MODE:
        return s->mode;
    case SHA1_HASH ... SHA1_HASH + 16:
        return bswap32(s->h[(offset - SHA1_HASH) >> 2]);
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8930.sha1: unimplemented read 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void sha1_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    S5L8930SHA1State *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case SHA1_CMD:
        s->cmd = v;
        if (v & CMD_START) {
            if (!(v & CMD_CONTINUE)) {
                memcpy(s->h, sha1_init, sizeof(s->h));
            }
            s->fill = 0;
            if (s->pio_words == 0xffff) {
                sha1_compress(s->h, s->block);
            }
            s->pio_words = 0;
        }
        break;
    case SHA1_DATA ... SHA1_DATA + 0x3C:
        stl_le_p(s->block + offset - SHA1_DATA, v);
        s->pio_words |= 1u << ((offset - SHA1_DATA) >> 2);
        break;
    case SHA1_RESET:
        s->cmd = s->mode = s->fill = 0;
        s->pio_words = 0;
        memcpy(s->h, sha1_init, sizeof(s->h));
        break;
    case SHA1_MODE:
        s->mode = v;
        break;
    case SHA1_HASH ... SHA1_HASH + 16:
        s->h[(offset - SHA1_HASH) >> 2] = bswap32(v);
        break;
    case SHA1_FIFO:
        stl_le_p(s->block + s->fill, v);
        s->fill += 4;
        if (s->fill == sizeof(s->block)) {
            sha1_compress(s->h, s->block);
            s->fill = 0;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s5l8930.sha1: unimplemented write 0x%"
                      HWADDR_PRIx " = 0x%x\n", offset, v);
    }
}

static const MemoryRegionOps sha1_ops = {
    .read = sha1_read,
    .write = sha1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8930_sha1_reset(DeviceState *dev)
{
    S5L8930SHA1State *s = S5L8930_SHA1(dev);

    s->cmd = s->mode = s->fill = 0;
    memcpy(s->h, sha1_init, sizeof(s->h));
}

static void s5l8930_sha1_init(Object *obj)
{
    S5L8930SHA1State *s = S5L8930_SHA1(obj);

    memory_region_init_io(&s->iomem, obj, &sha1_ops, s, "s5l8930.sha1",
                          S5L8930_SHA1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_s5l8930_sha1 = {
    .name = "s5l8930.sha1",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmd, S5L8930SHA1State),
        VMSTATE_UINT32(mode, S5L8930SHA1State),
        VMSTATE_UINT32_ARRAY(h, S5L8930SHA1State, 5),
        VMSTATE_UINT8_ARRAY(block, S5L8930SHA1State, 64),
        VMSTATE_UINT32(fill, S5L8930SHA1State),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8930_sha1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8930_sha1_reset);
    dc->vmsd = &vmstate_s5l8930_sha1;
}

static const TypeInfo s5l8930_sha1_info = {
    .name = TYPE_S5L8930_SHA1,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930SHA1State),
    .instance_init = s5l8930_sha1_init,
    .class_init = s5l8930_sha1_class_init,
};

static void s5l8930_sha1_register_types(void)
{
    type_register_static(&s5l8930_sha1_info);
}

type_init(s5l8930_sha1_register_types)
