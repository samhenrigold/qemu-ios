#include "hw/arm/ipod_touch_sha1.h"

/*
 * S5L8720 SHA1 engine.
 *
 * The block is a raw SHA1 compression engine, not a whole-message hasher:
 *
 *   - the guest loads a chaining state into the five hash registers at 0x20
 *     (each register holds one state word in big-endian byte order),
 *   - it feeds whole 64-byte message blocks, either a block at a time through
 *     the hardware buffer at 0x40 or by pointing the engine at physical
 *     memory (memory mode, 0x80/0x84/0x8c),
 *   - the resulting chaining state is read back out of the hash registers.
 *
 * Padding and finalisation belong to the guest. iBoot pre-pads its messages,
 * so for iBoot the state after the last block already is the digest. XNU
 * instead hands whole pages to the engine and then runs its own SHA1Final
 * over the padding block in software (libkern SHA1UpdateUsePhysicalAddress),
 * which only works if the engine returns the intermediate state.
 */

static const uint32_t sha1_iv[5] = {
    0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0
};

static uint8_t sha1_last_hash[20];
static bool sha1_last_hash_valid;

static bool sha1_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_SHA1_DEBUG") != NULL;
    }
    return on;
}

bool ipod_touch_sha1_last_hash(uint8_t out[20])
{
    if (!sha1_last_hash_valid) {
        return false;
    }
    memcpy(out, sha1_last_hash, sizeof(sha1_last_hash));
    return true;
}

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* One SHA1 compression round over a 64-byte block, in place on state[]. */
static void sha1_compress(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = ROTL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t tmp = ROTL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROTL(b, 30); b = a; a = tmp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

/* Publish the current chaining state for the PKE forge, big-endian. */
static void sha1_publish(IPodTouchSHA1State *s)
{
    for (int i = 0; i < 5; i++) {
        sha1_last_hash[i * 4 + 0] = s->state[i] >> 24;
        sha1_last_hash[i * 4 + 1] = s->state[i] >> 16;
        sha1_last_hash[i * 4 + 2] = s->state[i] >> 8;
        sha1_last_hash[i * 4 + 3] = s->state[i];
    }
    sha1_last_hash_valid = true;
}

static void sha1_reset(IPodTouchSHA1State *s)
{
	s->config = 0;
	s->memory_start = 0;
	s->memory_mode = 0;
	s->insize = 0;
    memcpy(s->state, sha1_iv, sizeof(s->state));
	memset(&s->hw_buffer, 0, sizeof(s->hw_buffer));
	s->hw_buffer_dirty = false;
}

/* Run the blocks the guest has queued through the compression function. */
static void sha1_run(IPodTouchSHA1State *s)
{
    if (s->hw_buffer_dirty) {
        sha1_compress(s->state, (const uint8_t *)s->hw_buffer);
        memset(s->hw_buffer, 0, sizeof(s->hw_buffer));
        s->hw_buffer_dirty = false;
    }

    if (s->memory_mode) {
        uint8_t block[64];
        for (uint32_t i = 0; i < s->insize / 0x40; i++) {
            cpu_physical_memory_read(s->memory_start + i * 0x40, block, 0x40);
            sha1_compress(s->state, block);
        }
    }

    sha1_publish(s);

    if (sha1_trace()) {
        printf("[SHA1] state=");
        for (int i = 0; i < 20; i++) {
            printf("%02x", sha1_last_hash[i]);
        }
        printf("\n");
        fflush(stdout);
    }
}

static uint64_t ipod_touch_sha1_read(void *opaque, hwaddr offset, unsigned size)
{
	IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

	switch(offset) {
		case SHA_CONFIG:
			return s->config;
		case SHA_RESET:
			return 0;
		case SHA_MEMORY_START:
			return s->memory_start;
		case SHA_MEMORY_MODE:
			return s->memory_mode;
		case SHA_INSIZE:
			return s->insize;
		case SHA_HASHOUT ... SHA_HASHOUT_END:
			/* Big-endian state word, matching what the guest writes in. */
			return bswap32(s->state[(offset - SHA_HASHOUT) / 4]);
	}

    return 0;
}

static void ipod_touch_sha1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchSHA1State *s = (IPodTouchSHA1State *)opaque;

    if (sha1_trace() && (offset < SHA_HWBUF || offset > SHA_HWBUF_END)) {
        printf("[SHA1] WR %#04x <- %#010llx\n", (unsigned)offset,
               (unsigned long long)value);
        fflush(stdout);
    }

	switch(offset) {
		case SHA_CONFIG:
			if(value == 0x2 || value == 0xa)
			{
                if (sha1_trace()) {
                    printf("[SHA1] GO config=%#llx memory_mode=%u start=%#x "
                           "insize=%#x hw_dirty=%d\n", (unsigned long long)value,
                           s->memory_mode, s->memory_start, s->insize,
                           (int)s->hw_buffer_dirty);
                    fflush(stdout);
                }
                sha1_run(s);
			} else {
				s->config = value;
			}
			break;
		case SHA_RESET:
			sha1_reset(s);
			break;
		case SHA_MEMORY_START:
			s->memory_start = value;
			break;
		case SHA_MEMORY_MODE:
			s->memory_mode = value;
			break;
		case SHA_INSIZE:
			s->insize = value;
			break;
		case SHA_HASHOUT ... SHA_HASHOUT_END:
			/* Load the chaining state to continue from. */
			s->state[(offset - SHA_HASHOUT) / 4] = bswap32((uint32_t)value);
			break;
		case SHA_HWBUF ... SHA_HWBUF_END:
			s->hw_buffer[(offset - SHA_HWBUF) / 4] = value;
			s->hw_buffer_dirty = true;
			break;
	}
}

static const MemoryRegionOps sha1_ops = {
    .read = ipod_touch_sha1_read,
    .write = ipod_touch_sha1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_sha1_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchSHA1State *s = IPOD_TOUCH_SHA1(dev);

    memory_region_init_io(&s->iomem, obj, &sha1_ops, s, "sha1", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);

    sha1_reset(s);
}

static void ipod_touch_sha1_class_init(ObjectClass *klass, void *data)
{

}

static const TypeInfo ipod_touch_sha1_info = {
    .name          = TYPE_IPOD_TOUCH_SHA1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchSHA1State),
    .instance_init = ipod_touch_sha1_init,
    .class_init    = ipod_touch_sha1_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_sha1_info);
}

type_init(ipod_touch_machine_types)
