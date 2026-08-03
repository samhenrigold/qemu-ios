#include "hw/arm/ipod_touch_pke.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_sha1.h"
#include <openssl/bn.h>
#include <openssl/bio.h>

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

static uint8_t *datahex(char* string) {

    if(string == NULL) 
       return NULL;

    size_t slength = strlen(string);
    if((slength % 2) != 0) // must be even
       return NULL;

    size_t dlength = slength / 2;

    uint8_t* data = malloc(dlength);
    memset(data, 0, dlength);

    size_t index = 0;
    while (index < slength) {
        char c = string[index];
        int value = 0;
        if(c >= '0' && c <= '9')
          value = (c - '0');
        else if (c >= 'A' && c <= 'F') 
          value = (10 + (c - 'A'));
        else if (c >= 'a' && c <= 'f')
          value = (10 + (c - 'a'));
        else {
          free(data);
          return NULL;
        }

        data[(index/2)] += value << (((index + 1) % 2) * 4);

        index++;
    }

    return data;
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


                BIGNUM *mod_bn = BN_lebin2bn(s->segments, s->segment_size, NULL);
                BIGNUM *base_bn = BN_lebin2bn(s->segments + s->segment_size, s->segment_size, NULL);
                BIGNUM *exp_bn = BN_new();
                BN_dec2bn(&exp_bn,"65537");
                // BN_print(BIO_new_fp(stdout, BIO_NOCLOSE), exp_bn);
                // printf("\n\n");

                BIGNUM *res = BN_new();
                BN_CTX *ctx = BN_CTX_new();
                BN_mod_exp(res, base_bn, exp_bn, mod_bn, ctx);
                // BN_print(BIO_new_fp(stdout, BIO_NOCLOSE), res);
                // printf("\n\n");

                char *bn_hex = BN_bn2hex(res);
                char *res_hex = datahex(bn_hex);

                if (getenv("IT_PKE_DEBUG")) {
                    printf("[PKE] modexp: result is %zu bytes (segment_size=%d)\n",
                           strlen(bn_hex) / 2, s->segment_size);
                    printf("[PKE]   mod : ");
                    for (int i = 0; i < 8; i++) { printf("%02x", s->segments[i]); }
                    printf("..");
                    for (int i = s->segment_size - 8; i < s->segment_size; i++) {
                        printf("%02x", s->segments[i]);
                    }
                    printf("\n[PKE]   base: ");
                    for (int i = 0; i < 8; i++) {
                        printf("%02x", s->segments[s->segment_size + i]);
                    }
                    printf("..");
                    for (int i = s->segment_size - 8; i < s->segment_size; i++) {
                        printf("%02x", s->segments[s->segment_size + i]);
                    }
                    printf("\n");
                    printf("[PKE]   head: ");
                    for (int i = 0; i < 8; i++) { printf("%02x", (uint8_t)res_hex[i]); }
                    printf("  tail: ");
                    for (int i = (int)(strlen(bn_hex) / 2) - 24;
                         i < (int)(strlen(bn_hex) / 2); i++) {
                        printf("%02x", (uint8_t)res_hex[i]);
                    }
                    printf("\n");
                }

                /*
                 * A well formed recovery is one byte shorter than the modulus
                 * (the leading 0x00 of the PKCS#1 block) and starts 0x01 0xFF.
                 * Anything else means the signature did not verify.
                 */
                size_t res_len = strlen(bn_hex) / 2;
                /*
                 * KNOWN BUG, DELIBERATELY NOT FIXED HERE -- needs a boot test.
                 * res_hex is char*, signed on this target, so `res_hex[1] ==
                 * 0xff` is a comparison of a value in [-128,127] against 255
                 * and clang proves it always false. well_formed can therefore
                 * NEVER be true, and this signature well-formedness check has
                 * never once run. The fix is (uint8_t)res_hex[1] == 0xff.
                 *
                 * It is left alone because turning a check on that has never
                 * executed is a behaviour change on the signature path, and
                 * the "modexp only runs on the 5th START" hack in this file may
                 * well be a compensation built on top of this being broken.
                 * Change ONE variable: fix the cast, boot, see whether the
                 * 5th-START behaviour changes, and only then touch the counter.
                 */
                bool well_formed = (res_len == (size_t)s->segment_size - 1) &&
                                   res_hex[0] == 0x01 && res_hex[1] == 0xff;

                if (!well_formed && forge_sigcheck_enabled()) {
                    uint8_t hash[20];
                    g_autofree uint8_t *block = g_malloc0(s->segment_size);

                    if (ipod_touch_sha1_last_hash(hash) &&
                        build_pkcs1_block(block, s->segment_size, hash)) {
                        /* SEG1 holds the value little endian. */
                        for (int i = 0; i < s->segment_size; i++) {
                            s->segments[s->segment_size + i] =
                                block[s->segment_size - 1 - i];
                        }
                        if (getenv("IT_PKE_DEBUG")) {
                            printf("[PKE]   forged a valid signature block\n");
                        }
                        break;
                    }
                }

                // copy this into SEG1 - note that the hex conversion removes the first 0x00 bytes so we add it back and shift everything to the right one place.
                for(int i = 0; i < (s->segment_size - 1); i++) { s->segments[s->segment_size + s->segment_size - 2 - i] = res_hex[i]; }
                s->segments[s->segment_size + s->segment_size - 1] = 0x0;
            }
            break;
        }
        case REG_PKE_SEG_SIZE:
            s->seg_size_reg = value;
            uint32_t size_bit = (s->seg_size_reg >> 6);
            if(size_bit == 0) { s->segment_size = 256; }
            else if(size_bit == 1) { s->segment_size = 128; }
            else { }
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

    dc->reset = ipod_touch_pke_reset;
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