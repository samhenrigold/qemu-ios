#ifndef FE_XFER_ABI_H
#define FE_XFER_ABI_H
#include <stdint.h>

static unsigned fe_xfer_half(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

/* A Thumb BL's destination; reject BLX and other branch encodings. */
static uintptr_t fe_xfer_callee(const unsigned char *p)
{
    unsigned a = fe_xfer_half(p), b = fe_xfer_half(p + 2);
    if ((a & 0xf800) != 0xf000 || (b & 0xd000) != 0xd000)
        return 0;
    unsigned s = a >> 10 & 1, i1 = (~(b >> 13) ^ s) & 1,
             i2 = (~(b >> 11) ^ s) & 1;
    uint32_t bits =
        s << 24 | i1 << 23 | i2 << 22 | (a & 0x3ff) << 12 | (b & 0x7ff) << 1;
    int32_t displacement = (int32_t)(bits << 7) >> 7;
    return (uintptr_t)(p + 4) + displacement;
}

/* Newer frameworks retain the conditional name as a synchronous seven-argument
 * alias. Establish equivalence from two matching stack-only forwarders to the
 * same callee, plus the obsolete WithSwap's return-success stub. No OS version,
 * symbol address, or Apple binary fixture is embedded in this check. */
static int fe_xfer_synchronous_alias(const unsigned char *plain,
                                     const unsigned char *conditional,
                                     const unsigned char *swap)
{
    if (!plain || !conditional || !swap || fe_xfer_half(swap) != 0x2000 ||
        fe_xfer_half(swap + 2) != 0x4770 || fe_xfer_half(plain) != 0xb580 ||
        fe_xfer_half(plain + 2) != 0x466f || fe_xfer_half(plain + 4) != 0xb085)
        return 0;
    /* Three stack loads and three stack stores: no PC-relative operands. */
    for (unsigned off = 6; off < 30; off += 4) {
        if (fe_xfer_half(plain + off) != (off < 18 ? 0xf8d7 : 0xf8cd) ||
            fe_xfer_half(plain + off + 2) >> 12 == 15)
            return 0;
    }
    /* Zero the two additional internal arguments, then restore the stack. */
    if (fe_xfer_half(plain + 30) != 0xf04f ||
        fe_xfer_half(plain + 32) != 0x0900 ||
        fe_xfer_half(plain + 34) != 0xe9cd ||
        fe_xfer_half(plain + 36) != 0x9900 ||
        fe_xfer_half(plain + 42) != 0xb005 ||
        fe_xfer_half(plain + 44) != 0xbd80)
        return 0;
    for (unsigned i = 0; i < 46; i++) {
        if (i >= 38 && i < 42)
            continue; /* relocated BL */
        if (plain[i] != conditional[i])
            return 0;
    }
    uintptr_t target = fe_xfer_callee(plain + 38);
    return target && target == fe_xfer_callee(conditional + 38);
}
#endif
