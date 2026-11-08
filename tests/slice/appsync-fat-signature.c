/* ARM64-first universal apps need the ARMv7 code directory on a 32-bit guest.
 * SLICE contrib/appsync/appsync.c fn be32 appsync_arm32_slice
 * CFLAGS -Wall -Wextra -Werror
 */
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void put(unsigned char *p, unsigned v)
{
    for (int i = 0; i < 4; i++)
        p[i] = v >> (24 - 8 * i);
}
#include "slice.h"
int main(void)
{
    unsigned char h[48] = {0};
    put(h, 0xcafebabe);
    put(h + 4, 2);
    put(h + 8, 0x100000c);
    put(h + 16, 16384);
    put(h + 28, 12);
    put(h + 32, 9);
    put(h + 36, 40189952);
    assert(appsync_arm32_slice(h, sizeof h) == 40189952);
    assert(appsync_arm32_slice(h, 47) == -1);
    put(h + 8, 12);
    assert(appsync_arm32_slice(h, sizeof h) == 16384);
    put(h + 8, 0x100000c);
    put(h + 28, 0x100000c);
    assert(appsync_arm32_slice(h, sizeof h) == -1);
    put(h + 4, 0xffffffff);
    assert(appsync_arm32_slice(h, sizeof h) == -1);
    put(h, 0xcefaedfe);
    assert(appsync_arm32_slice(h, sizeof h) == 0);
    assert(appsync_arm32_slice(h, 7) == -1);
    puts(
        "PASS: ARM64-first Pages layout, ARM32-first compatibility and bounded fat header parsing");
}
