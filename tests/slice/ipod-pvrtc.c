/* PVRTC1 reference vectors and tiny-mip/RGB policy under ASan and UBSan.
 *
 * Golden RGBA hashes were generated with PowerVR Native SDK's
 * framework/PVRCore/texture/PVRTDecompress.cpp at revision
 * 34f68e4c028e06704ff2f69f292a61f9ba0c53c4 (no runtime dependency):
 * https://github.com/powervr-graphics/Native_SDK/tree/34f68e4c028e06704ff2f69f292a61f9ba0c53c4
 * The deterministic compressed words below mix opaque/translucent endpoints.
 * The whole check builds as C++ so it can link the production C++ decoder.
 *
 * SLICE hw/arm/gles-host.c fn pvrtc_decode pvrtc_size
 * CFLAGS -O1 -I$ROOT/hw/arm -x c++ -std=gnu++17 $ROOT/hw/arm/powervr/pvrtc.cpp -lc++
 */
#include <assert.h>
#include "powervr/pvrtc.h"
#define MAX(a,b) ((a)>(b)?(a):(b))
#include <CommonCrypto/CommonDigest.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slice.h"

/* Decode exactly pvrtc_size bytes from a heap copy, so ASan catches any read past what the guest sent. */
static uint8_t *decode(const uint8_t *data, size_t n, unsigned w, unsigned h, int bpp, bool alpha)
{
    assert(w && w<=4096 && h && h<=4096 && (bpp==2 || bpp==4));
    assert(!pvrtc_size(0, 4, bpp) && !pvrtc_size(3, 4, bpp));
    assert(!pvrtc_size(4, 0, bpp) && !pvrtc_size(4, 3, bpp));
    size_t length = pvrtc_size(w, h, bpp);
    assert(length == n);
    uint8_t *src = (uint8_t *)malloc(length), *dst = (uint8_t *)malloc((size_t)w*h*4);
    assert(src && dst);
    memcpy(src, data, length);
    assert(pvrtc_decode(src, w, h, bpp, alpha, false, dst));
    free(src);
    return dst;
}

/* mode < 0: cycle through every modulation mode block by block. */
static uint8_t *compressed(unsigned w, unsigned h, int bpp, int mode, size_t *n)
{
    uint32_t state = 0x12345678;
    size_t blocks = (size_t)w * h * bpp / 64;
    uint8_t *data = (uint8_t *)malloc(blocks * 8);
    for (size_t i = 0; i < blocks; i++) {
        uint32_t words[2];
        for (int k = 0; k < 2; k++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            words[k] = state;
        }
        uint32_t modulation = words[0], color = words[1];
        int current = mode < 0 ? (int)(i % (bpp == 2 ? 4 : 2)) : mode;
        color = (color & ~1u) | (current != 0);
        if (bpp == 2) {
            if (current)
                modulation = (modulation & ~1u) | (current >= 2);
            if (current >= 2)
                modulation = (modulation & ~(1u << 20)) | ((uint32_t)(current == 3) << 20);
        }
        uint8_t le[8] = { (uint8_t)modulation, (uint8_t)(modulation >> 8), (uint8_t)(modulation >> 16),
                          (uint8_t)(modulation >> 24), (uint8_t)color, (uint8_t)(color >> 8),
                          (uint8_t)(color >> 16), (uint8_t)(color >> 24) };
        memcpy(data + i * 8, le, 8);
    }
    *n = blocks * 8;
    return data;
}

static void sha256_hex(const uint8_t *p, size_t n, char out[65])
{
    uint8_t md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(p, (CC_LONG)n, md);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(out + 2 * i, 3, "%02x", md[i]);
}

/* bpp, width, height: one hash per uniform modulation mode, in encoding order. */
static const struct { int bpp; unsigned w, h; const char *hashes[4]; } golden[] = {
    {2, 16, 8, {
        "72c90e12d14c9374885a07b395af1b3f16f4879c9bb95654982e943ba3ab5307",
        "fba11314f25670c4c34354fc9322a77134e68c7e184942c7382b085faa65cec1",
        "ef7355f60b9b8f80f64831e1285641aef062ef146980acb768fb9867fddcbac5",
        "262a7d66828859b7998e47d183e2aa47f52cbe6af2397e47bca2dda37aa16651"}},
    {2, 32, 8, {
        "d42d791f714fb72ec3a5fe425c02bbefe98d514c18db3f4b590cc929d2a28c8f",
        "c2f6559b9cf4f6cdadcc1342aa22114c8bc6874925b0116007dd25258a19169b",
        "aa0b0e85bd60d88e7fc23d3433c3d4d3628b9064a8e040292eafba8bda464bd1",
        "4ba42dd3625bcc87c41a5676dec616ed2502608f48f486053a9a92a69c6dd3ca"}},
    {2, 16, 16, {
        "03ac7b9bc39028790a92e970848d0619a82d9295eda94c2825afeba1d36d7430",
        "a9d3dbacc32c60e8b18da4b084fe1c57a79eb56d21038179c3508924bce7fee0",
        "e0a943ce9cbe6cf9a1fd3cebb347b3e5b7e0fe100da2462ae302581af3331e36",
        "658ded23d78109bc4380e898ec128f33dffaa6f9bfd938db3c0edb5fd94e36fc"}},
    {4, 8, 8, {
        "463a67be5143569de1a28c58539936a0a51f822441c1f132271882461fd38438",
        "db9f25cf6b7a09b425387a9bbe1f71b9bffdc23a0669ab8fde5f6648e19103ba"}},
    {4, 32, 8, {
        "2a357e88ce96cce4cecec521ae49d22bf4b014bc980b46548d2d02b17751e4b3",
        "62b7cfee543c03b76954775cac5c592c465a9fce0e305d896b3b4d59aa9cbe59"}},
    {4, 8, 32, {
        "76c0d7d11e3a8ced873020f2bfe8e6c089b41dca2dd84663d06abfc8f8df9732",
        "31a323b4c8ecd1b4af066cbafe1703160963fa48948c1328d7c64437fbbf5888"}},
};

int main(void)
{
    char hex[65];
    size_t n;
    for (size_t g = 0; g < sizeof(golden) / sizeof(golden[0]); g++) {
        unsigned w = golden[g].w, h = golden[g].h;
        int bpp = golden[g].bpp;
        for (int mode = 0; mode < 4 && golden[g].hashes[mode]; mode++) {
            uint8_t *data = compressed(w, h, bpp, mode, &n);
            uint8_t *rgba = decode(data, n, w, h, bpp, true);
            sha256_hex(rgba, (size_t)w * h * 4, hex);
            if (strcmp(hex, golden[g].hashes[mode])) {
                fprintf(stderr, "bpp %d %ux%u mode %d: %s\n", bpp, w, h, mode, hex);
                abort();
            }
            uint8_t *rgb = decode(data, n, w, h, bpp, false);
            for (size_t i = 0; i < (size_t)w * h * 4; i++)
                assert(rgb[i] == (i % 4 == 3 ? 0xff : rgba[i]));
            free(data); free(rgba); free(rgb);
        }
    }
    static const struct { int bpp; unsigned w, h; const char *hash; } mixed[] = {
        {2, 32, 16, "2119ee5d991f927d67b789bfaf1faca7a1c0578853df33e4ac576fd544dec4d9"},
        {4, 16, 16, "511484ca0bf5ebb59ed6bf7ae25a9d181356f26dfb3ada030d40b43d31b745e3"},
    };
    for (int m = 0; m < 2; m++) {
        uint8_t *data = compressed(mixed[m].w, mixed[m].h, mixed[m].bpp, -1, &n);
        uint8_t *rgba = decode(data, n, mixed[m].w, mixed[m].h, mixed[m].bpp, true);
        sha256_hex(rgba, (size_t)mixed[m].w * mixed[m].h * 4, hex);
        assert(!strcmp(hex, mixed[m].hash));
        free(data); free(rgba);
    }

    /* Preserve the guest driver's compact one-word mip tails. Heap redzones
     * detect accidental reads of spec-padding blocks that the guest did not send. */
    const uint8_t one[8] = {0, 0, 0, 0, 0xfe, 0xff, 0xff, 0xff};
    for (int bpp = 2; bpp <= 4; bpp += 2)
        for (unsigned w = 1; w <= 4; w *= 2)
            for (unsigned h = 1; h <= 4; h *= 2) {
                uint8_t *out = decode(one, 8, w, h, bpp, true);
                for (size_t i = 0; i < (size_t)w * h * 4; i++)
                    assert(out[i] == 0xff);
                free(out);
            }
    const uint8_t punch[8] = {0xaa, 0xaa, 0xaa, 0xaa, 0xff, 0xff, 0xff, 0xff};
    uint8_t *out = decode(punch, 8, 4, 4, 4, true);
    for (int i = 0; i < 64; i++)
        assert(out[i] == (i % 4 == 3 ? 0 : 0xff));
    free(out);
    out = decode(punch, 8, 4, 4, 4, false);
    for (int i = 0; i < 64; i++)
        assert(out[i] == 0xff);
    free(out);
    puts("PASS: 20 vendor vectors, all 2/4bpp modes, mixed neighbors, RGB alpha, tiny mips; ASan/UBSan");
}
