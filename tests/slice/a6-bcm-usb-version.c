/* Native HSIC version packet and USB short-transfer bounds.
 * SLICE hw/usb/dev-bcm-hsic.c define BCM_
 * SLICE hw/usb/dev-bcm-hsic.c fn bcm_download_reset bcm_download_data bcm_otp_word bcm_vendor_control
 * PKG glib-2.0 zlib
 * CFLAGS -Wall -Wextra -Werror
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <glib.h>
#include <zlib.h>
#include <stdbool.h>
#define qemu_log_mask(...) ((void)0)
typedef struct {
    GByteArray *download;
    unsigned log_count;
    uint32_t dl_state, dl_size;
    bool dl_started, running;
} BCMHSICState;
static uint32_t ldl_le_p(const void *v)
{
    const uint8_t *p = v;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void stl_le_p(void *v, uint32_t x)
{
    uint8_t *p = v;
    for (unsigned i = 0; i < 4; i++)
        p[i] = x >> (8 * i);
}
#include "slice.h"
int main(void)
{
    BCMHSICState s = {0};
    uint8_t b[32];
    memset(b, 0xcc, sizeof(b));
    assert(bcm_vendor_control(&s, 0xc105, 1, 0, 8, b + 1) == 8);
    assert(!memcmp(b + 1, "\x34\x43\0\0\2\0\0\0", 8));
    assert(b[0] == 0xcc && b[9] == 0xcc);
    memset(b, 0xcc, sizeof(b));
    assert(bcm_vendor_control(&s, 0xc105, 1, 0, 1, b + 1) == 1);
    assert(b[1] == 0x34 && b[2] == 0xcc);
    assert(bcm_vendor_control(&s, 0xc105, 1, 0, 0, b) == 0 && b[0] == 0xcc);
    assert(bcm_vendor_control(&s, 0xc105, 1, 0, 31, b) == 8 && b[8] == 0xcc);
    memset(b, 0xcc, sizeof(b));
    assert(bcm_vendor_control(&s, 0xc105, 0, 0, 8, b) == -1);
    assert(bcm_vendor_control(&s, 0xc105, 1, 1, 8, b) == -1);
    assert(bcm_vendor_control(&s, 0xc104, 1, 0, 8, b) == -1 && b[0] == 0xcc);
    /* Decode the native ROM read response and SW bit boundaries, then
     * reconstruct the CIS exactly as the driver does (two bytes/read). */
    assert(bcm_vendor_control(&s, 0xc111, 0x838, 0x1800, 16, b) == 16);
    unsigned begin = (ldl_le_p(b + 12) >> 3) & 0x1fff;
    assert(bcm_vendor_control(&s, 0xc111, 0x83a, 0x1800, 16, b) == 16);
    unsigned end = (ldl_le_p(b + 12) >> 3) & 0x1fff;
    assert(begin == 0x40 && end > begin && end - begin <= 512);
    uint8_t cis[512];
    for (unsigned offset = begin; offset < end; offset += 2) {
        assert(bcm_vendor_control(&s, 0xc111, 0x800 + offset, 0x1800, 16, b) ==
               16);
        for (unsigned i = 0; i < 12; i++)
            assert(b[i] == 0);
        cis[offset - begin] = b[12];
        cis[offset - begin + 1] = b[13];
    }
    assert(cis[0] == 0x15 && cis[1] + 2u < end - begin && cis[2] == 5);
    const char *identity = (const char *)cis + 4;
    /* Native 13A344 selector uses Manufacturer/ Product, not the later
     * ProductInfo fields, for HSICChipInfo and HSICRFHardwareInfo. */
    assert(!strcmp(identity, "s=B2"));
    identity += strlen(identity) + 1;
    assert(!strcmp(identity, "M=HEIN P=N41 V=t"));
    identity += strlen(identity) + 1;
    assert(!*identity && !identity[1] && (uint8_t)identity[2] == 0xff);
    assert(bcm_vendor_control(&s, 0xc111, 0x839, 0x1800, 16, b) == -1);
    assert(bcm_vendor_control(&s, 0xc111, 0x800, 0x1900, 16, b) == -1);
    assert(bcm_vendor_control(&s, 0xc111, 0x838, 0x1800, 17, b) == -1);
    uint8_t image[64] = {0};
    stl_le_p(image, BCM_TRX_MAGIC);
    stl_le_p(image + 4, sizeof(image));
    for (unsigned i = 12; i < sizeof(image); i++)
        image[i] = i;
    stl_le_p(image + 8, crc32(0, image + 12, sizeof(image) - 12) ^ UINT32_MAX);
    assert(bcm_vendor_control(&s, 0xc100, 1, 0, 8, b) == 8 &&
           ldl_le_p(b) == 0 && ldl_le_p(b + 4) == 0);
    assert(bcm_vendor_control(&s, 0xc102, 1, 0, 8, b) == -1);
    assert(bcm_download_data(&s, image, 64) && s.dl_state == BCM_DL_RUNNABLE);
    bcm_download_reset(&s);
    assert(bcm_vendor_control(&s, 0xc103, 1, 0, 8, b) == 8 && s.dl_started);
    assert(bcm_download_data(&s, image, 10) && s.dl_state == BCM_DL_WAITING);
    assert(bcm_download_data(&s, image + 10, 18) && s.dl_state == BCM_DL_READY);
    assert(bcm_vendor_control(&s, 0xc100, 1, 0, 8, b) == 8 &&
           ldl_le_p(b) == 1 && ldl_le_p(b + 4) == 28);
    assert(bcm_download_data(&s, image + 28, 36) &&
           s.dl_state == BCM_DL_RUNNABLE);
    assert(bcm_download_data(&s, image, 0) && s.download->len == 64);
    assert(bcm_vendor_control(&s, 0xc102, 1, 0, 8, b) == 8 && s.running &&
           !s.dl_started);
    assert(!bcm_download_data(&s, image, 64));
    bcm_vendor_control(&s, 0xc103, 1, 0, 8, b);
    image[63] ^= 1;
    assert(bcm_download_data(&s, image, 64) && s.dl_state == BCM_DL_BAD_CRC);
    assert(bcm_vendor_control(&s, 0xc102, 1, 0, 8, b) == -1 && !s.running);
    bcm_vendor_control(&s, 0xc103, 1, 0, 8, b);
    image[0] = 0;
    assert(!bcm_download_data(&s, image, 64) && s.dl_state == BCM_DL_BAD_HDR);
    bcm_vendor_control(&s, 0xc103, 1, 0, 8, b);
    stl_le_p(image, BCM_TRX_MAGIC);
    stl_le_p(image + 4, BCM_DL_MAX + 1);
    assert(!bcm_download_data(&s, image, 28) &&
           s.dl_state == BCM_DL_IMAGE_TOOBIG);
    bcm_vendor_control(&s, 0xc103, 1, 0, 8, b);
    assert(!bcm_download_data(&s, image, SIZE_MAX) &&
           s.dl_state == BCM_DL_IMAGE_TOOBIG);
    /* Apple rebuilds an uncompressed image's length/NVRAM, retaining its
     * on-disk CRC. Its explicit payload lengths, not that stale CRC, gate GO. */
    bcm_vendor_control(&s, 0xc103, 1, 0, 8, b);
    stl_le_p(image + 4, sizeof(image));
    stl_le_p(image + 12, 0x10020);
    stl_le_p(image + 16, 28);
    stl_le_p(image + 24, 8);
    stl_le_p(image + 8, 0xdeadbeef);
    assert(bcm_download_data(&s, image, 63) && s.dl_state == BCM_DL_READY);
    assert(bcm_vendor_control(&s, 0xc102, 1, 0, 8, b) == -1);
    assert(bcm_download_data(&s, image + 63, 1) &&
           s.dl_state == BCM_DL_RUNNABLE);
    bcm_vendor_control(&s, 0xc103, 1, 0, 8, b);
    stl_le_p(image + 24, 9);
    assert(!bcm_download_data(&s, image, 64) && s.dl_state == BCM_DL_BAD_HDR);
    bcm_download_reset(&s);
    assert(!s.dl_started && !s.running && !s.download->len);
    stl_le_p(image + 24, 8);
    assert(bcm_download_data(&s, image, sizeof(image)) &&
           s.dl_state == BCM_DL_RUNNABLE);
    assert(bcm_vendor_control(&s, 0xc102, 1, 0, 8, b) == 8);
    assert(bcm_vendor_control(&s, 0xc105, 1, 0, 8, b) == 8 &&
           ldl_le_p(b) == 0xa123 && ldl_le_p(b + 4) == 1);
    bcm_download_reset(&s);
    assert(bcm_vendor_control(&s, 0xc105, 1, 0, 8, b) == 8 &&
           ldl_le_p(b) == 0x4334 && ldl_le_p(b + 4) == 2);
    /* The native iOS 6 loader submits unused buffer tail after a valid
     * uncompressed image, including additional complete USB chunks. */
    uint8_t extended[3000];
    memset(extended, 0xa5, sizeof(extended));
    stl_le_p(extended, BCM_TRX_MAGIC);
    stl_le_p(extended + 4, 1700);
    stl_le_p(extended + 12, 0x10020);
    stl_le_p(extended + 16, 1672);
    stl_le_p(extended + 24, 0);
    bcm_download_reset(&s);
    assert(bcm_download_data(&s, extended, 1500));
    assert(bcm_download_data(&s, extended + 1500, 1500));
    assert(s.dl_state == BCM_DL_RUNNABLE && s.download->len == 3000 &&
           s.dl_size == 1700);
    assert(bcm_download_data(&s, extended, 1500) && s.download->len == 4500);
    assert(!bcm_download_data(&s, extended, SIZE_MAX) &&
           s.download->len == 4500);
    assert(bcm_vendor_control(&s, 0xc102, 1, 0, 8, b) == 8 && s.running);
    assert(!bcm_download_data(&s, extended, 1));
    bcm_download_reset(&s);
    /* Extra compressed data is still rejected. */
    stl_le_p(extended + 12, 0);
    assert(bcm_download_data(&s, extended, 1500));
    assert(!bcm_download_data(&s, extended + 1500, 1500));
    bcm_download_reset(&s);
    /* Optional caller-supplied native image; no firmware is redistributed or
     * required by the ordinary test. Exercise real chunk/CRC boundaries. */
    const char *fixture = g_getenv("BCM_TRX_FIXTURE");
    if (fixture) {
        char *bytes = NULL;
        gsize length = 0;
        GError *error = NULL;
        assert(g_file_get_contents(fixture, &bytes, &length, &error));
        assert(length >= 28 && length <= BCM_DL_MAX);
        bcm_vendor_control(&s, 0xc103, 1, 0, 8, b);
        for (gsize at = 0; at < length; at += 1500) {
            assert(bcm_download_data(&s, (const uint8_t *)bytes + at,
                                     MIN(1500, length - at)));
        }
        assert(s.dl_state == BCM_DL_RUNNABLE && s.download->len == length);
        assert(bcm_vendor_control(&s, 0xc102, 1, 0, 8, b) == 8 && s.running);
        printf("Native TRX fixture validated: %zu bytes\n", length);
        g_free(bytes);
    }
    g_byte_array_unref(s.download);
    puts(
        "PASS: BCM USB chip/state packets, bounded TRX download, CRC/GO gating and reset");
}
