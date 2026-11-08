/* Boot-time N42 identity must match original iBoot, using the shared DT editor.
 * SLICE include/hw/arm/s5l8930.h typedef A4DTEdit
 * SLICE hw/arm/ipad1.c fn a4_dt_walk a4_dt_edit
 * SLICE:id hw/arm/s5l8950.c range static const uint8_t a6_n42_product_id[20] = | };
 * SLICE hw/arm/s5l8950.c fn a6_dt_identity
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define warn_report(...) ((void)0)
static uint32_t ldl_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}
static void stl_le_p(void *p, uint32_t n)
{
    uint8_t *b = p;
    for (unsigned i = 0; i < 4; i++)
        b[i] = n >> (8 * i);
}
#include "id.h"
}
;
#include "slice.h"
static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    assert(n > 0);
    rewind(f);
    uint8_t *b = malloc(n);
    assert(b);
    assert(fread(b, 1, n, f) == (size_t)n);
    fclose(f);
    *size = n;
    return b;
}
int main(void)
{
    /* A root with one product node, including an unrelated USB product-id. */
    uint8_t image[512] = {0};
    uint8_t *dt = image + 0x100;
    stl_le_p(dt, 1);
    stl_le_p(dt + 4, 1);
    memcpy(dt + 8, "name", 5);
    stl_le_p(dt + 40, 1);
    uint8_t *node = dt + 48;
    stl_le_p(node, 3);
    memcpy(node + 8, "name", 5);
    stl_le_p(node + 40, 8);
    memcpy(node + 44, "product", 8);
    memcpy(node + 52, "product-id", 11);
    stl_le_p(node + 84, 20);
    memcpy(node + 108, "usb-product-id", 15);
    stl_le_p(node + 140, 4);
    stl_le_p(node + 144, 0x12a8);
    stl_le_p(image + 4, 0x80000000);
    stl_le_p(image + 8, 0x80000000);
    stl_le_p(image + 0x30, 0x80000100);
    stl_le_p(image + 0x34, 196);
    a6_dt_identity(image, sizeof(image), 0x80000000, 0x80000000);
    assert(!memcmp(node + 88, a6_n42_product_id, 20));
    assert(ldl_le_p(node + 144) == 0x12a8);
    uint8_t before[512];
    memcpy(before, image, sizeof(image));
    a6_dt_identity(image, sizeof(image), 0x80000000, 0x80000000);
    assert(!memcmp(before, image, sizeof(image)));
    memset(node + 88, 0, 20);
    stl_le_p(image + 0x34, 32);
    a6_dt_identity(image, sizeof(image), 0x80000000, 0x80000000);
    for (unsigned i = 0; i < 20; i++)
        assert(!node[88 + i]);
    const char *path = getenv("IOS_N42_IBOOT_11A465");
    if (path) {
        size_t n;
        uint8_t *boot = read_file(path, &n);
        assert(n > 0x434e4);
        /* Native product-id setter calls this getter; its literal points
         * to the 20 original bytes copied into the reserved DT property. */
        assert(ldl_le_p(boot + 0x28448) == 0x47704800);
        uint32_t ptr = ldl_le_p(boot + 0x2844c);
        assert(ptr >= 0xbff00000 && (uint64_t)ptr - 0xbff00000 + 20 <= n);
        assert(!memcmp(boot + ptr - 0xbff00000, a6_n42_product_id, 20));
        free(boot);
    }
    puts(
        "PASS: N42 boot identity, unrelated properties, idempotence, truncated DT and optional original iBoot reference");
}
