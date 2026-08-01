#include "hw/arm/ipod_touch_pke.h"
#include <openssl/bn.h>
#include <openssl/bio.h>

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
        case REG_PKE_SEG_START ... (REG_PKE_SEG_START + 1024):
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
        case REG_PKE_SEG_START ... (REG_PKE_SEG_START + 1024):
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

static void ipod_touch_pke_class_init(ObjectClass *klass, void *data)
{

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