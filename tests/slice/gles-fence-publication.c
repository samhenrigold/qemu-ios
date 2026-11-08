/* A completed bridge fence must drain queued rendering and publish its pixels
 * before a CPU image-cache consumer reads the surface.
 * SLICE contrib/gles-public/opengles.c fn batch_flush gles_batch fe_fenceSync fe_clientWaitSync
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define GLES_OP_BATCH 0x1009
#define GLES_ID_glFinish 1
#define DRAW 7
#define BATCH_WORDS 32
#define FE_GL_ALREADY_SIGNALED 0x911a
#define A(...)                                                                 \
    (const unsigned[])                                                         \
    {                                                                          \
        __VA_ARGS__                                                            \
    }
typedef struct {
    unsigned host, *batch, batch_len, rendered, published;
} GuestGC;
static const unsigned char gles_batchable[8] = {[DRAW] = 1};
static long long qc(unsigned, void *, unsigned, const unsigned *);
#include "slice.h"
static long long qc(unsigned slot, void *opaque, unsigned argc,
                    const unsigned *args)
{
    GuestGC *gc = opaque;
    if (gles_batch(slot, gc, argc, args))
        return 0;
    if (slot == GLES_OP_BATCH) {
        assert(argc == 2);
        for (unsigned i = 0; i < args[1]; i += 2) {
            assert(gc->batch[i] == (DRAW | 1u << 16));
            gc->rendered = gc->batch[i + 1];
        }
    } else {
        assert(slot == GLES_ID_glFinish && argc == 0);
        assert(gc->batch_len == 0);
        gc->published = gc->rendered;
    }
    return 0;
}
int main(void)
{
    GuestGC a = {.host = 1}, b = {.host = 2};
    qc(DRAW, &a, 1, A(0xff336699));
    qc(DRAW, &b, 1, A(0xff112233));
    assert(a.published == 0 && b.published == 0);
    unsigned first = fe_fenceSync(&a, 0x9117, 0);
    assert(first && !a.batch_len && a.published == 0xff336699);
    assert(b.batch_len && b.published == 0); /* no cross-context publication */
    assert(fe_clientWaitSync(&a, first, 0, 0, 0) == FE_GL_ALREADY_SIGNALED);
    unsigned second = fe_fenceSync(&b, 0x9117, 0);
    assert(second && second != first && b.published == 0xff112233);
    qc(DRAW, &a, 1, A(0xffabcdef));
    assert(a.published == 0xff336699);
    assert(fe_fenceSync(&a, 0x9117, 0));
    assert(a.published == 0xffabcdef);
    assert(fe_fenceSync(NULL, 0x9117, 0));
    free(a.batch);
    free(b.batch);
    puts(
        "PASS: queued drawing published before fence completion, contexts isolated");
}
