/*
 * A NAND image store: one raw file per chip select in a directory, written in
 * place. geometry.json names the layout; record `block * pages_per_block +
 * page` sits at `record * (page_bytes + spare_bytes)`, data then spare, and is
 * stored XOR the model's blank record, so a hole (a sparse range, or an erase)
 * reads as the blank page. Host independent: the files go through QEMU's block
 * layer (an erase unmaps where the host can: F_PUNCHHOLE on macOS, fallocate
 * on Linux). Cloning a base into a device's live image is the host app's job.
 *
 *   geometry.json  {"storage_format": "nand-xor-blank-v2", "blank": "ff" | "n72-generated",
 *                   "chip_selects", "page_bytes", "spare_bytes", "pages_per_block", "blocks_per_ce"}
 *   cs<N>.pages    one per chip select, pages_per_ce * stride bytes
 */
#ifndef HW_ARM_NAND_IMAGE_H
#define HW_ARM_NAND_IMAGE_H

#include "system/block-backend.h"

#define NAND_IMAGE_FORMAT "nand-xor-blank-v2"
#define NAND_IMAGE_MAX_CHIPS 8

typedef struct NANDImage NANDImage;

/* The geometry a model expects; nand_image_open refuses a store that differs. */
typedef struct NANDImageGeometry {
    unsigned chips;
    uint32_t page_bytes, spare_bytes, pages_per_block, blocks_per_chip;
} NANDImageGeometry;

/* One raw file as a BlockBackend owned by `owner`, `size` bytes (a writable
 * empty file is sized). Shared with the A4 IOP's stores. */
BlockBackend *nand_image_open_file(DeviceState *owner, const char *path,
                                   size_t size, bool writable, Error **errp);

/* Whether `dir` holds a NAND image store (geometry.json names this format). */
bool nand_image_is_store(const char *dir);

/* The store in `dir`, opened for writing; NULL (errp set) when it is not one
 * of `geo`. Flushed on every VM stop. */
NANDImage *nand_image_open(DeviceState *owner, const char *dir,
                           const NANDImageGeometry *geo, Error **errp);

/* One record (page_bytes + spare_bytes) as the chip holds it. 0 or -errno. */
int nand_image_read(NANDImage *n, unsigned chip, uint32_t page, uint8_t *record);
/* Replaces one record. 0 or -errno. */
int nand_image_write(NANDImage *n, unsigned chip, uint32_t page, const uint8_t *record);
/* Every record of one erase block reads blank again. 0 or -errno. */
int nand_image_erase(NANDImage *n, unsigned chip, uint32_t block);
/* Whether a write, erase or flush of any image store failed; latched for the process. */
bool nand_image_io_failed(void);

#endif
