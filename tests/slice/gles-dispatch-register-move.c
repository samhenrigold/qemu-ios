/* Synthetic dispatch trampolines: register-moved targets, early guards and
 * spilled arguments. No stock firmware instruction sequences are embedded.
 * Losing the lr-to-ip target move left native texture filtering unimplemented.
 * SLICE contrib/it-gles/gles_dispatch.c fn gles_trampoline_slot
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "slice.h"
int main(void)
{
    static const uint16_t moved[] = {
        0x6908,         /* ldr r0,[r1,#16] */
        0xf8d1, 0xe4cc, /* ldr.w lr,[r1,#0x4cc] */
        0x46f4,         /* mov ip,lr */
        0x4760          /* bx ip */
    };
    uint32_t aligned[40] = {0};
    memcpy(aligned, moved, sizeof moved);
    assert(gles_trampoline_slot(aligned, 1, 1035) == 302);
    /* Synthetic spill wrapper: a null-context branch skips the call body.
     * lr holds an argument loaded from the caller's stack. After blx r4 and
     * pop {pc}, that argument must not be mistaken for a second dispatch
     * target at the alternate bx lr return. */
    static const uint16_t spilled[] = {
        0xb148,                /* cbz r0, alternate return at +22 */
        0xb510,                /* push {r4,lr} */
        0xf8d7, 0xe014,        /* ldr.w lr,[r7,#20] */
        0xf8d0, 0x44c8,        /* ldr.w r4,[r0,#0x4c8] */
        0x6900,                /* ldr r0,[r0,#16] */
        0x47a0,                /* blx r4 */
        0xbd10,                /* pop {r4,pc} */
        0xbf00, 0xbf00, 0x4770 /* alternate bx lr */
    };
    memset(aligned, 0, sizeof aligned);
    memcpy(aligned, spilled, sizeof spilled);
    assert(gles_trampoline_slot(aligned, 1, 1035) == 301);
    /* Synthetic Thumb null-context guard: cbnz skips an early return and
     * reaches the target load. This is a separate shape from an IT guard. */
    static const uint16_t guarded[] = {
        0xb901, 0xbd80, /* cbnz r1, +0; pop {r7,pc} */
        0x6908,         /* ldr r0,[r1,#16] */
        0xf8d1, 0x94cc, /* ldr.w r9,[r1,#0x4cc] */
        0x4748          /* bx r9 */
    };
    memset(aligned, 0, sizeof aligned);
    memcpy(aligned, guarded, sizeof guarded);
    assert(gles_trampoline_slot(aligned, 1, 1035) == 302);
    /* Legacy direct ARM target, GC at +0xc and dispatch entry at +0x4c8. */
    memset(aligned, 0, sizeof aligned);
    aligned[0] = 0xe590000c;
    aligned[1] = 0xe590c4c8;
    aligned[2] = 0xe12fff1c;
    assert(gles_trampoline_slot(aligned, 0, 1035) == 302);
    /* A synthetic table entry beyond immediate LDR's 4 KiB reach. */
    static const uint16_t indexed[] = {
        0xf241, 0x2510, /* movw r5,#0x1210 */
        0x5945,         /* ldr r5,[r0,r5] */
        0x6900,         /* ldr r0,[r0,#16] */
        0x47a8,         /* blx r5 */
        0xbd00          /* pop {pc} */
    };
    memset(aligned, 0, sizeof aligned);
    memcpy(aligned, indexed, sizeof indexed);
    assert(gles_trampoline_slot(aligned, 1, 1200) == (0x1210 - 0x14) / 4);
    assert(gles_trampoline_slot(aligned, 1, 1000) == -1);
    /* The same indexed entry loaded into a high register needs LDR.W. */
    static const uint16_t wide_indexed[] = {
        0xf241, 0x2310, /* movw r3,#0x1210 */
        0xf850, 0xc003, /* ldr.w ip,[r0,r3] */
        0x6900,         /* ldr r0,[r0,#16] */
        0x4760          /* bx ip */
    };
    memset(aligned, 0, sizeof aligned);
    memcpy(aligned, wide_indexed, sizeof wide_indexed);
    assert(gles_trampoline_slot(aligned, 1, 1200) == (0x1210 - 0x14) / 4);
    puts(
        "GL dispatch: synthetic moved targets, guards, spills and legacy ARM passed");
}
