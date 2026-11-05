/* A host failure on an image store (write, erase, or the flush at a stop) latches, so qemu_ios_ui_storage_failed keeps
 * the helper from reporting the stop as clean, and the device's old store isn't deleted. A guest address out of range
 * is the guest's error, and doesn't latch.
 *
 * SLICE include/hw/arm/nand-image.h define NAND_IMAGE_MAX_CHIPS
 * SLICE include/hw/arm/nand-image.h typedef NANDImageGeometry
 * SLICE hw/arm/nand-image.c range static bool nand_image_failed; | BlockBackend *nand_image_open_file
 * SLICE hw/arm/nand-image.c fn nand_image_bad nand_image_xor nand_image_write nand_image_erase nand_image_vm_state
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int BlockBackend, RunState, VMChangeStateEntry;
typedef struct NANDImage NANDImage;
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
#define BDRV_REQ_MAY_UNMAP 1
#define error_report(...) ((void)0)
static int write_result, zero_result, flush_result;
static int blk_pwrite(BlockBackend *b, int64_t o, int64_t n, const void *p, int f) { return write_result; }
static int blk_pwrite_zeroes(BlockBackend *b, int64_t o, int64_t n, int f) { return zero_result; }
static int blk_flush(BlockBackend *b) { return flush_result; }
#include "slice.h"

int main(void)
{
    static uint8_t record[8], key[8], buffer[8];
    BlockBackend chip = 0;
    NANDImage n = { .chip = { &chip }, .blank = key, .buffer = buffer };

    n.geo.chips = 1;
    n.geo.pages_per_block = 4;
    n.geo.blocks_per_chip = 4;
    n.geo.page_bytes = sizeof(record);
    n.stride = sizeof(record);

    assert(!nand_image_write(&n, 0, 0, record) && !nand_image_erase(&n, 0, 0));
    nand_image_vm_state(&n, false, 0);
    assert(!nand_image_io_failed());
    assert(nand_image_write(&n, 9, 0, record) == -EINVAL && nand_image_erase(&n, 9, 0) == -EINVAL);
    assert(!nand_image_io_failed());          /* the guest's address, not the host */

    flush_result = -EIO;
    nand_image_vm_state(&n, true, 0);
    assert(!nand_image_io_failed());          /* running: no flush */
    nand_image_vm_state(&n, false, 0);
    assert(nand_image_io_failed());
    flush_result = 0;
    nand_image_vm_state(&n, false, 0);
    assert(nand_image_io_failed());           /* a later flush doesn't clear it */

    nand_image_failed = false;
    write_result = -ENOSPC;
    assert(nand_image_write(&n, 0, 0, record) == -ENOSPC && nand_image_io_failed());
    write_result = 0;
    nand_image_failed = false;
    zero_result = -EIO;
    assert(nand_image_erase(&n, 0, 0) == -EIO && nand_image_io_failed());
    puts("PASS: an image store's failed write, erase or flush latches; a guest's bad address doesn't");
    return 0;
}
