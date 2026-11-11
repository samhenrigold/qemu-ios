#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xfer-abi.h"

/* Synthetic ISA/ABI templates, not firmware bytes. */
static void half(unsigned char *p, unsigned v)
{
    p[0] = v;
    p[1] = v >> 8;
}
static void branch(unsigned char *p, unsigned char *target)
{
    uint32_t d = (uint32_t)(target - (p + 4));
    unsigned s = d >> 24 & 1, i1 = d >> 23 & 1, i2 = d >> 22 & 1;
    half(p, 0xf000 | s << 10 | (d >> 12 & 0x3ff));
    half(p + 2, 0xd000 | ((~i1 ^ s) & 1) << 13 | ((~i2 ^ s) & 1) << 11 |
                    (d >> 1 & 0x7ff));
}
static void forwarder(unsigned char *p, unsigned char *target)
{
    half(p, 0xb580);
    half(p + 2, 0x466f);
    half(p + 4, 0xb085);
    for (unsigned off = 6; off < 30; off += 4) {
        half(p + off, off < 18 ? 0xf8d7 : 0xf8cd);
        half(p + off + 2,
             off); /* arbitrary stack-only register/offset fields */
    }
    half(p + 30, 0xf04f);
    half(p + 32, 0x0900);
    half(p + 34, 0xe9cd);
    half(p + 36, 0x9900);
    branch(p + 38, target);
    half(p + 42, 0xb005);
    half(p + 44, 0xbd80);
}
int main(void)
{
    unsigned char bytes[512] = {0};
    unsigned char *a = bytes + 64, *b = bytes + 192, *swap = bytes + 320;
    half(swap, 0x2000);
    half(swap + 2, 0x4770);
    forwarder(a, bytes);
    forwarder(b, bytes); /* negative BL displacements */
    assert(fe_xfer_synchronous_alias(a, b, swap));
    branch(b + 38, bytes + 448);
    assert(!fe_xfer_synchronous_alias(a, b, swap));
    branch(a + 38, bytes + 448); /* positive BL displacements */
    assert(fe_xfer_synchronous_alias(a, b, swap));
    half(swap, 0x2001);
    assert(!fe_xfer_synchronous_alias(a, b, swap));
    half(swap, 0x2000);
    half(b + 4, 0xb089);
    assert(!fe_xfer_synchronous_alias(a, b, swap));
    half(b + 4, 0xb085);
    half(b + 40, fe_xfer_half(b + 40) & ~0x1000); /* BLX is not this ABI */
    assert(!fe_xfer_synchronous_alias(a, b, swap));
    assert(!fe_xfer_synchronous_alias(a, NULL, swap));
    puts(
        "PASS synchronous transfer aliases; legacy/different/invalid wrappers rejected");
}
