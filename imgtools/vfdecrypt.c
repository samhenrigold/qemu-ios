/* Decrypt an Apple 'encrcdsa' v2 disk image (iOS rootfs) with its 36-byte vfdecrypt key
 * (AES-128 key + HMAC-SHA1 key); per-chunk IV = HMAC-SHA1(hmac_key, be32(chunk))[:16].
 * macOS only (CommonCrypto):  cc -O2 -o vfdecrypt vfdecrypt.c */
#include <CommonCrypto/CommonCrypto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t be(const uint8_t *p, int n) { uint64_t v = 0; while (n--) v = v << 8 | *p++; return v; }

int main(int argc, char **argv)
{
    if (argc != 4 || strlen(argv[3]) != 72) {
        fprintf(stderr, "usage: %s in.dmg out.dmg <72 hex digit key>\n", argv[0]);
        return 2;
    }
    uint8_t key[36], h[0x100];
    for (int i = 0; i < 36; i++) sscanf(argv[3] + 2 * i, "%2hhx", &key[i]);
    FILE *f = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
    if (!f || !o) { perror("open"); return 1; }
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "encrcdsa", 8)) { fprintf(stderr, "not encrcdsa\n"); return 1; }
    uint32_t bs = be(h + 52, 4);
    uint64_t left = be(h + 56, 8);
    fseeko(f, be(h + 64, 8), SEEK_SET);
    uint8_t *ct = malloc(bs), *pt = malloc(bs);
    for (uint32_t n = 0; left; n++) {
        size_t r = fread(ct, 1, bs, f), got;
        if (!r) { fprintf(stderr, "short image\n"); return 1; }
        uint8_t nb[4] = { n >> 24, n >> 16, n >> 8, n }, iv[CC_SHA1_DIGEST_LENGTH];
        CCHmac(kCCHmacAlgSHA1, key + 16, 20, nb, 4, iv);
        CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0, key, 16, iv, ct, r, pt, bs, &got);
        size_t take = got < left ? got : left;
        fwrite(pt, 1, take, o);
        left -= take;
    }
    return fclose(o) ? 1 : 0;
}
