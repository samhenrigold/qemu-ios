#include "hw/arm/ipod_touch_pke.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_sha1.h"
#include "qemu/log.h"
#include <openssl/bn.h>

/*
 * Forging an img3 signature check.
 *
 * Every image the boot chain loads carries an RSA signature over its own
 * SHA1, and the bootrom and iBoot really do verify it: the modexp below
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

static uint64_t ipod_touch_pke_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchPKEState *s = (IPodTouchPKEState *)opaque;

    //printf("%s: offset 0x%08x\n", __FUNCTION__, offset);

    switch(offset) {
        case REG_PKE_SEG_SIZE:
            return s->seg_size_reg;
        /* +1023, not +1024: segments[] is 1024 bytes = 256 words, so an offset
         * of exactly +1024 indexes word 256 -- four bytes past the array, into
         * seg_size_reg. Inclusive range, so the last legal offset is +1023. */
        case REG_PKE_SEG_START ... (REG_PKE_SEG_START + 1023):
        {
            uint32_t *res = (uint32_t *)s->segments;
            return res[(offset - REG_PKE_SEG_START) / 4];
        }
        default:
            break;
    }

    return 0;
}

static void ipod_touch_pke_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchPKEState *s = (IPodTouchPKEState *)opaque;

    //printf("%s: offset 0x%08x value 0x%08x\n", __FUNCTION__, offset, value);

    switch(offset) {
        case 0x0:
            if (getenv("IT_PKE_DEBUG")) { printf("[PKE] reset via reg 0x0\n"); }
            s->num_started = 0;
            break;
        case 0x10:
            break;
        /* +1023, not +1024: segments[] is 1024 bytes = 256 words, so an offset
         * of exactly +1024 indexes word 256 -- four bytes past the array, into
         * seg_size_reg. Inclusive range, so the last legal offset is +1023. */
        case REG_PKE_SEG_START ... (REG_PKE_SEG_START + 1023):
        {
            uint32_t *segments_cast = (uint32_t *)s->segments;
            segments_cast[(offset - REG_PKE_SEG_START) / 4] = value;
            break;
        }
        case REG_PKE_START:
        {
            s->num_started++;

            if (getenv("IT_PKE_DEBUG")) {
                printf("[PKE] START #%d (segment_size=%d)\n",
                       s->num_started, s->segment_size);
            }

            if(s->num_started == 5) { // TODO this is arbitrary!

                if (s->segment_size != 128 && s->segment_size != 256) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "[PKE] unsupported segment size %u; ignored\n",
                                  s->segment_size);
                    break;
                }

                uint8_t result[256], expected[256];
                BIGNUM *mod = BN_lebin2bn(s->segments, s->segment_size, NULL);
                BIGNUM *base = BN_lebin2bn(s->segments + s->segment_size,
                                         s->segment_size, NULL);
                BIGNUM *exponent = BN_new(), *recovered = BN_new();
                BN_CTX *ctx = BN_CTX_new();
                bool ok = mod && base && exponent && recovered && ctx &&
                          !BN_is_zero(mod) && BN_set_word(exponent, 65537) &&
                          BN_mod_exp(recovered, base, exponent, mod, ctx) &&
                          BN_bn2binpad(recovered, result, s->segment_size) == s->segment_size;
                BN_free(mod);
                BN_free(base);
                BN_free(exponent);
                BN_free(recovered);
                BN_CTX_free(ctx);
                if (!ok) {
                    qemu_log_mask(LOG_GUEST_ERROR, "[PKE] modular exponentiation failed\n");
                    break;
                }

                /* Validate the entire SHA1 DigestInfo/padding, using the
                 * recovered digest. The old signed-char 0xff comparison was
                 * always false, so even valid signatures were forged. */
                bool well_formed = build_pkcs1_block(expected, s->segment_size,
                                        result + s->segment_size - 20) &&
                                   !memcmp(result, expected, s->segment_size);
                if (!well_formed && forge_sigcheck_enabled()) {
                    uint8_t hash[20];
                    if (ipod_touch_sha1_last_hash(hash)) {
                        build_pkcs1_block(result, s->segment_size, hash);
                        if (getenv("IT_PKE_DEBUG")) {
                            printf("[PKE] forged a signature block\n");
                        }
                    }
                }
                /* Fixed-width conversion preserves leading zeros and short
                 * results. SEG1 holds the same integer little-endian. */
                for (size_t i = 0; i < s->segment_size; i++) {
                    s->segments[s->segment_size + i] = result[s->segment_size - 1 - i];
                }
            }
            break;
        }
        case REG_PKE_SEG_SIZE:
            s->seg_size_reg = value;
            uint32_t size_bit = (s->seg_size_reg >> 6);
            if(size_bit == 0) { s->segment_size = 256; }
            else if(size_bit == 1) { s->segment_size = 128; }
            else {
                /*
                 * Only 0 and 1 are decoded, so any other encoding leaves
                 * segment_size at whatever it was -- and that is ZERO after
                 * reset. The copy-out loop at the end of START is
                 * `for (i = 0; i < segment_size - 1; i++)`, unsigned, so a zero
                 * there makes the bound 0xFFFFFFFF and the destination index
                 * `segments[-2 - i]`: a backwards walk off the front of the
                 * array, through the heap, for as long as it takes to crash.
                 *
                 * segment_size is deliberately NOT changed here. Measured: the
                 * 3.1.3 boot chain writes SEG_SIZE = 0x81 (encoding 2) exactly
                 * once per boot, so this is a live path, not a latent one, and
                 * zeroing on it would have changed a value the boot signature
                 * path already runs against. The bound is enforced where it is
                 * actually unsafe -- at START, below -- which leaves every
                 * reachable sequence bit-identical to before (verified by
                 * diffing a full IT_PKE_DEBUG boot trace, 522 lines, against
                 * the previous binary).
                 */
                qemu_log_mask(LOG_GUEST_ERROR,
                              "[PKE] reserved segment size encoding %u "
                              "(SEG_SIZE=0x%08x); size left at %u\n",
                              size_bit, s->seg_size_reg, s->segment_size);
            }
            break;
        case REG_PKE_SWRESET:
            if (getenv("IT_PKE_DEBUG")) { printf("[PKE] SWRESET\n"); }
            s->num_started = 0;
            break;
        default:
            break;
    }
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

/* Stateless modexp engine: every field is loaded by the guest before a run,
 * so the reset values are simply zero. num_started in particular is the
 * counter behind the "modexp only runs on the 5th START" hack -- carried into
 * a second boot it would put the engine out of phase with the new iBoot. */
static void ipod_touch_pke_reset(DeviceState *dev)
{
    IPodTouchPKEState *s = IPOD_TOUCH_PKE(dev);

    memset(s->segments, 0, sizeof(s->segments));
    s->seg_size_reg = 0;
    s->segment_size = 0;
    s->num_started = 0;
}

static const VMStateDescription vmstate_ipod_touch_pke = {
    .name = "ipod_touch_pke",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(segments, IPodTouchPKEState, 1024),
        VMSTATE_UINT32(seg_size_reg, IPodTouchPKEState),
        VMSTATE_UINT32(segment_size, IPodTouchPKEState),
        VMSTATE_UINT8(num_started, IPodTouchPKEState),
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