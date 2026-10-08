/*
 * The SHA-1 compression function, shared by the S5L8720 and S5L8930 SHA-1
 * engines: both are bare block functions (the driver pads in software), so
 * QEMU's whole-message qcrypto_hash API does not fit. Plain C, so the host
 * unit tests can include it as it is.
 */
#ifndef HW_ARM_SHA1_COMPRESS_H
#define HW_ARM_SHA1_COMPRESS_H

#include <stdint.h>

#define SHA1_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* One compression round over a 64-byte block, in place on h[]. */
static inline void sha1_compress(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
               (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = SHA1_ROTL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
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
        uint32_t t = SHA1_ROTL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = SHA1_ROTL(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

#endif
