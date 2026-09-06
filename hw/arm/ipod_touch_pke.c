#include "hw/arm/ipod_touch_pke.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_sha1.h"
#include "qemu/log.h"
#include <openssl/bn.h>

/*
 * Forging an img3 signature check.
 *
 * Every image the boot chain loads carries an RSA signature over its own
 * SHA1, and the bootrom and iBoot really do verify it: the Montgomery operations below
 * recovers a PKCS#1 v1.5 block whose tail is the digest the SHA1 engine just
 * produced, and the caller compares the two.
 *
 * That works for the stock 2.1.1 NOR, whose images were signed for this
 * device. It does not work for images taken straight out of an IPSW's
 * all_flash directory: from iOS 3.0 on, Apple personalised boot images per
 * device through its TSS signing server, and the copies shipped in the IPSW
 * carry a signature no device accepts. The golden NOR's iBoot signature, for
 * instance, appears nowhere in either IPSW -- it was issued at restore time.
 *
 * So retargeting the machine to a different firmware needs the same treatment
 * the GID key already gets: the emulated part vouches for the image instead of
 * checking it. When the recovered block is not well formed, synthesise the one
 * the caller is about to compare against, built from the last digest the SHA1
 * engine computed. Enabled with IT_FORGE_SIGCHECK=1; off by default, so the
 * stock NOR keeps booting through the genuine verification path.
 */
static bool forge_sigcheck_enabled(void)
{
    return getenv("IT_FORGE_SIGCHECK") != NULL;
}

/* ASN.1 DigestInfo prefix for SHA1, as it appears in a PKCS#1 v1.5 block. */
static const uint8_t sha1_digestinfo[] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
    0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14,
};

/* Build 00 01 FF..FF 00 || DigestInfo || hash, big endian, len bytes. */
static bool build_pkcs1_block(uint8_t *out, size_t len, const uint8_t hash[20])
{
    size_t tail = sizeof(sha1_digestinfo) + 20;

    if (len < tail + 11) {
        return false;
    }
    out[0] = 0x00;
    out[1] = 0x01;
    memset(out + 2, 0xff, len - tail - 3);
    out[len - tail - 1] = 0x00;
    memcpy(out + len - tail, sha1_digestinfo, sizeof(sha1_digestinfo));
    memcpy(out + len - 20, hash, 20);
    return true;
}

/* Each START performs A * B / R modulo the loaded modulus. The firmware
 * implements exponentiation by selecting operand segments between commands.
 * The radix includes the hardware's extra 16 bits per precision lane. */
static void pke_execute(IPodTouchPKEState *s, uint32_t command)
{
    if (!(command & 9)) { return; }
    unsigned size = s->segment_size;
    unsigned a = s->seg_id >> 24, b = s->seg_id >> 16 & 255;
    unsigned m = s->seg_id >> 8 & 255, dest = s->seg_id & 255;
    bool one = s->seg_size_reg & 2;
    if ((size != 64 && size != 128 && size != 256) ||
        a >= sizeof(s->segments) / size || dest >= sizeof(s->segments) / size ||
        (!one && b >= sizeof(s->segments) / size) || m >= sizeof(s->segments) / size) {
        qemu_log_mask(LOG_GUEST_ERROR, "[PKE] invalid segment selection\n");
        return;
    }
    if (command & 8) {
        memcpy(s->modulus, s->segments + m * size, size);
        s->modulus_size = size;
    }
    if (!(command & 1)) {
        return;
    }
    if (s->modulus_size != size) {
        qemu_log_mask(LOG_GUEST_ERROR, "[PKE] modulus has not been loaded\n");
        return;
    }
    uint8_t result[256], expected[256];
    BIGNUM *mod = BN_lebin2bn(s->modulus, size, NULL);
    BIGNUM *left = BN_lebin2bn(s->segments + a * size, size, NULL);
    BIGNUM *right = one ? BN_new() : BN_lebin2bn(s->segments + b * size, size, NULL);
    BIGNUM *radix = BN_new(), *inverse = BN_new(), *value = BN_new();
    BN_CTX *ctx = BN_CTX_new();
    unsigned bits = ((s->key_len & 3) + 1) * ((((s->key_len >> 3) & 15) + 1) * 32 + 16);
    bool ok = mod && left && right && radix && inverse && value && ctx;
    if (ok) {
        if (s->seg_sign & (1u << a)) { BN_set_negative(left, 1); }
        if (one) { ok = BN_one(right); }
        else if (s->seg_sign & (1u << b)) { BN_set_negative(right, 1); }
        /* ponytail: compute the radix inverse per operation; cache only if
         * boot profiling shows a cost, and invalidate on modulus reload. */
        ok = ok && BN_is_odd(mod) && !BN_is_one(mod) && BN_set_bit(radix, bits) &&
             BN_mod_inverse(inverse, radix, mod, ctx) &&
             BN_mod_mul(value, left, right, mod, ctx) &&
             BN_mod_mul(value, value, inverse, mod, ctx) &&
             BN_bn2binpad(value, result, size) == size;
    }
    BN_free(mod); BN_free(left); BN_free(right); BN_free(radix);
    BN_free(inverse); BN_free(value); BN_CTX_free(ctx);
    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR, "[PKE] Montgomery operation failed\n");
        return;
    }
    /* The known boot verifier exits Montgomery form from segment 2 into 1.
     * Keep its explicitly enabled unsigned-image compatibility at that final
     * conversion, never on intermediate products or a command counter. */
    if (one && a == 2 && dest == 1 && forge_sigcheck_enabled()) {
        bool valid = build_pkcs1_block(expected, size, result + size - 20) &&
                     !memcmp(expected, result, size);
        uint8_t hash[20];
        if (!valid && ipod_touch_sha1_last_hash(hash)) {
            build_pkcs1_block(result, size, hash);
        }
    }
    for (unsigned i = 0; i < size; i++) {
        s->segments[dest * size + i] = result[size - 1 - i];
    }
    s->seg_sign &= ~(1u << dest); /* Canonical positive modular residue. */
}

static uint64_t ipod_touch_pke_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchPKEState *s = opaque;
    if (size && size <= 4 && offset >= REG_PKE_SEG_START &&
        offset - REG_PKE_SEG_START <= sizeof(s->segments) - size) {
        uint32_t value = 0;
        for (unsigned i = 0; i < size; i++) {
            value |= (uint32_t)s->segments[offset - REG_PKE_SEG_START + i] << (8 * i);
        }
        return value;
    }
    switch (offset) {
    case 0: return s->key_len;
    case 0xc: return s->seg_id;
    case 0x10: return s->seg_sign;
    case REG_PKE_SEG_SIZE: return s->seg_size_reg;
    default: return 0; /* Synchronous completion clears START. */
    }
}

static void ipod_touch_pke_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchPKEState *s = opaque;
    if (getenv("IT_PKE_DEBUG") && offset < REG_PKE_SEG_START) {
        printf("[PKE] write +0x%03x = 0x%08x\n", (unsigned)offset, (unsigned)value);
    }
    if (size && size <= 4 && offset >= REG_PKE_SEG_START &&
        offset - REG_PKE_SEG_START <= sizeof(s->segments) - size) {
        for (unsigned i = 0; i < size; i++) {
            s->segments[offset - REG_PKE_SEG_START + i] = value >> (8 * i);
        }
        return;
    }
    switch (offset) {
    case 0: s->key_len = value; break;
    case 0xc: s->seg_id = value; break;
    case 0x10: s->seg_sign = value; break;
    case REG_PKE_SEG_SIZE:
        s->seg_size_reg = value;
        s->segment_size = ((value >> 6) & 3) == 3 ? 0 : 256 >> ((value >> 6) & 3);
        break;
    case REG_PKE_START: pke_execute(s, value); break;
    case REG_PKE_SWRESET:
        s->modulus_size = 0;
        s->seg_sign = 0;
        break;
    default: break;
    }
}

static int pke_post_load(void *opaque, int version_id)
{
    IPodTouchPKEState *s = opaque;
    if ((s->segment_size && s->segment_size != 64 && s->segment_size != 128 && s->segment_size != 256) ||
        (s->modulus_size && s->modulus_size != 64 && s->modulus_size != 128 && s->modulus_size != 256)) {
        return -EINVAL;
    }
    return 0;
}

static const MemoryRegionOps pke_ops = {
    .read = ipod_touch_pke_read,
    .write = ipod_touch_pke_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_pke_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchPKEState *s = IPOD_TOUCH_PKE(dev);

    memory_region_init_io(&s->iomem, obj, &pke_ops, s, "pke", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

/* Reset registers, operand SRAM and the preloaded modulus. */
static void ipod_touch_pke_reset(DeviceState *dev)
{
    IPodTouchPKEState *s = IPOD_TOUCH_PKE(dev);

    memset(s->segments, 0, sizeof(s->segments));
    s->seg_size_reg = 0;
    s->segment_size = 0;
    s->key_len = s->seg_id = s->seg_sign = s->modulus_size = 0;
    memset(s->modulus, 0, sizeof(s->modulus));
}

static const VMStateDescription vmstate_ipod_touch_pke = {
    .name = "ipod_touch_pke",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = pke_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(segments, IPodTouchPKEState, 2048),
        VMSTATE_UINT8_ARRAY(modulus, IPodTouchPKEState, 256),
        VMSTATE_UINT32(modulus_size, IPodTouchPKEState),
        VMSTATE_UINT32(key_len, IPodTouchPKEState),
        VMSTATE_UINT32(seg_id, IPodTouchPKEState),
        VMSTATE_UINT32(seg_sign, IPodTouchPKEState),
        VMSTATE_UINT32(seg_size_reg, IPodTouchPKEState),
        VMSTATE_UINT32(segment_size, IPodTouchPKEState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_pke_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ipod_touch_pke_reset);
    dc->vmsd = &vmstate_ipod_touch_pke;
}

static const TypeInfo ipod_touch_pke_info = {
    .name          = TYPE_IPOD_TOUCH_PKE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchPKEState),
    .instance_init = ipod_touch_pke_init,
    .class_init    = ipod_touch_pke_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_pke_info);
}

type_init(ipod_touch_machine_types)