/* An APPLE_sync fence reaches the host only where it asks for one: the A6's host reports GLES_CAP_FENCE_FINISH and
 * gets a glFinish before each fence; every other board's reports no bits and its fences are answered in the shim, as
 * those boards were qualified with (6.x and 7.x CoreAnimation fence every frame).
 *
 * SLICE:fe contrib/gles-public/opengles.c fn fe_fenceSync
 */
#include <assert.h>
#include <stdio.h>
#include <unistd.h>   /* execv: fe_fenceSync reads the capability once, so each host is its own process */
#define GLES_OP_CAPABILITIES 0x100a
#define GLES_CAP_FENCE_FINISH 8u   /* include/hw/arm/guest-services/gles.h */
#define GLES_ID_glFinish 7
#define A(...) (const unsigned[]){ __VA_ARGS__ }
static unsigned caps, asked, finished;
static long long qc(unsigned slot, void *gc, unsigned argc, const unsigned *args)
{
    (void)gc; (void)argc; (void)args;
    if (slot == GLES_OP_CAPABILITIES) { asked++; return caps; }
    if (slot == GLES_ID_glFinish) finished++;
    return 0;
}
#include "fe.h"

int main(int argc, char **argv)
{
    int gc;
    caps = argc > 1 ? GLES_CAP_FENCE_FINISH : 0;
    assert(fe_fenceSync(0, 0, 0) == 1 && !asked && !finished);   /* no context: nothing to ask or finish */
    unsigned a = fe_fenceSync(&gc, 0, 0), b = fe_fenceSync(&gc, 0, 0);
    assert(a && b && a != b && asked == 1);
    assert(finished == (caps ? 2 : 0));
    if (argc == 1) {
        char *again[] = { argv[0], "a6", 0 };
        fflush(stdout);
        return execv(argv[0], again);
    }
    puts("PASS: a fence finishes on the host only where it reports GLES_CAP_FENCE_FINISH");
    return 0;
}
