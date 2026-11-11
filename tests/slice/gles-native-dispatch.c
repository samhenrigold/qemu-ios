/* Synthetic ARM trampoline templates; no firmware bytes are included.
 * SLICE contrib/gles-public/opengles.c fn fe_native_dispatch
 */
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned fe_u32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}
#include "slice.h"
static void half(unsigned char *p, unsigned n)
{
    p[0] = n;
    p[1] = n >> 8;
}
static void word(unsigned char *p, unsigned n)
{
    half(p, n);
    half(p + 2, n >> 16);
}
static void trampoline(unsigned char *p, int guarded, unsigned key, unsigned gc)
{
    memset(p, 0, 32);
    word(p + 4, 0x0f70ee1d); /* mrc: thread pointer */
    word(p + 8, 0x0003f020); /* clear thread-pointer tag */
    half(p + 12, 0x6803 | key << 6);
    unsigned at = guarded ? 20 : 16;
    if (guarded) {
        half(p + 14, 0x2b00); /* cmp r3, #0 */
        half(p + 16, 0xbf08); /* it eq */
        half(p + 18, 0x4770); /* bx lr */
    } else {
        half(p + 14, 0xb103); /* short null-context guard */
    }
    half(p + at, 0x6818 | (gc / 4) << 6);
    half(p + at + 2, 0xf8d3);
    half(p + at + 4, 0x3000 | (gc + 4 + 104 * 4));
    half(p + at + 6, 0x4718);
}
int main(void)
{
    unsigned char p[32];
    unsigned key = 0, gc = 0;
    for (int guarded = 0; guarded < 2; guarded++) {
        for (unsigned offset = 12; offset <= 16; offset += 4) {
            trampoline(p, guarded, 30, offset);
            assert(fe_native_dispatch(p, 104, &key, &gc));
            assert(key == 30 && gc == offset / 4);
            assert(!fe_native_dispatch(p, 103, &key, &gc));
            assert(!fe_native_dispatch(p, ~0u, &key, &gc));
            p[(guarded ? 20 : 16) + 6] ^= 1;
            assert(!fe_native_dispatch(p, 104, &key, &gc));
        }
    }
    trampoline(p, 1, 0, 16);
    assert(!fe_native_dispatch(p, 104, &key, &gc));
    trampoline(p, 1, 30, 8);
    assert(!fe_native_dispatch(p, 104, &key, &gc));
    trampoline(p, 1, 30, 16);
    p[16] = 0; /* A partial conditional-return sequence is not recognized. */
    assert(!fe_native_dispatch(p, 104, &key, &gc));
    trampoline(p, 1, 30, 16);
    p[4] ^= 1;
    assert(!fe_native_dispatch(p, 104, &key, &gc));
    puts(
        "ok: legacy and conditional-return native GL dispatch; malformed tails rejected");
}
