/*
 * NAND image store: see include/hw/arm/nand-image.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "block/block.h"
#include "system/runstate.h"
#include "hw/arm/nand-image.h"

/*
 * A failed write, erase or flush latches: the helper must not report the stop as clean, which lets the device's old
 * store be deleted (qemu_ios_ui_storage_failed).
 */
static bool nand_image_failed;

bool nand_image_io_failed(void)
{
    return qatomic_read(&nand_image_failed);
}

struct NANDImage {
    BlockBackend *chip[NAND_IMAGE_MAX_CHIPS];
    NANDImageGeometry geo;
    uint32_t stride;
    uint8_t *blank;    /* what a hole reads as: the stored XOR key */
    uint8_t *buffer;
    VMChangeStateEntry *vm_state;
};

BlockBackend *nand_image_open_file(DeviceState *owner, const char *path,
                                   size_t size, bool writable, Error **errp)
{
    struct stat st;
    QDict *options;
    BlockBackend *blk;
    BlockDriverState *bs;
    uint64_t perm = BLK_PERM_CONSISTENT_READ | (writable ? BLK_PERM_WRITE : 0);
    uint64_t shared = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE_UNCHANGED;
    int fd = open(path, writable ? O_RDWR | O_CREAT : O_RDONLY, 0644);

    if (fd < 0 || fstat(fd, &st) < 0 ||
        (writable && st.st_size == 0 && ftruncate(fd, size) < 0)) {
        error_setg_errno(errp, errno, "cannot open NAND file %s", path);
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    if (st.st_size != size && !(writable && st.st_size == 0)) {
        error_setg(errp, "%s: NAND file size differs from geometry", path);
        close(fd);
        return NULL;
    }
    close(fd);
    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    bs = bdrv_open(path, NULL, options, BDRV_O_NO_SHARE |
                   (runstate_check(RUN_STATE_INMIGRATE) ? BDRV_O_INACTIVE : 0) |
                   (writable ? BDRV_O_RDWR | BDRV_O_UNMAP : 0), errp);
    if (!bs) {
        return NULL;
    }
    /* Attach the guest owner before requesting permissions: incoming
     * migration keeps images inactive until normal block-layer handoff. */
    blk = blk_new(bdrv_get_aio_context(bs), 0, shared);
    if (blk_attach_dev(blk, owner) < 0) {
        error_setg(errp, "%s: NAND backend already has a device owner", path);
        blk_unref(blk);
        bdrv_unref(bs);
        return NULL;
    }
    if (blk_set_perm(blk, perm, shared, errp) < 0 ||
        blk_insert_bs(blk, bs, errp) < 0) {
        blk_detach_dev(blk, owner);
        blk_unref(blk);
        bdrv_unref(bs);
        return NULL;
    }
    bdrv_unref(bs);
    return blk;
}

static QDict *nand_image_geometry(const char *dir, Error **errp)
{
    g_autofree char *path = g_build_filename(dir, "geometry.json", NULL);
    g_autofree char *json = NULL;
    QObject *obj;
    QDict *g;

    if (!g_file_get_contents(path, &json, NULL, NULL)) {
        error_setg(errp, "cannot read %s", path);
        return NULL;
    }
    obj = qobject_from_json(json, NULL);
    g = qobject_to(QDict, obj);
    if (!g) {
        error_setg(errp, "%s: not a JSON object", path);
        qobject_unref(obj);
    }
    return g;
}

bool nand_image_is_store(const char *dir)
{
    QDict *g = nand_image_geometry(dir, NULL);
    const char *format = g ? qdict_get_try_str(g, "storage_format") : NULL;
    bool is = format && !strcmp(format, NAND_IMAGE_FORMAT);

    qobject_unref(g);
    return is;
}

/* The flash's erased record, as the store's "blank" names it. */
static bool nand_image_blank(const char *name, uint8_t *record, uint32_t page_bytes, uint32_t spare_bytes)
{
    if (!g_strcmp0(name, "ff")) {
        memset(record, 0xff, page_bytes + spare_bytes);
        return true;
    }
    /* The iPod touch 2G's generated layout (ipod_touch_fmss.c fmss_blank_page): zero data, spare word 2 0x00FF00FF. */
    if (!g_strcmp0(name, "n72-generated") && spare_bytes >= 12) {
        memset(record, 0, page_bytes + spare_bytes);
        stl_le_p(record + page_bytes + 8, 0x00ff00ff);
        return true;
    }
    return false;
}

static void nand_image_vm_state(void *opaque, bool running, RunState state)
{
    NANDImage *n = opaque;

    if (running) {
        return;
    }
    for (unsigned i = 0; i < n->geo.chips; i++) {
        int result = blk_flush(n->chip[i]);
        if (result < 0) {
            qatomic_set(&nand_image_failed, true);
            error_report("NAND image: flush of chip select %u: %s", i, strerror(-result));
        }
    }
}

static void nand_image_free(NANDImage *n)
{
    for (unsigned i = 0; i < NAND_IMAGE_MAX_CHIPS; i++) {
        if (n->chip[i]) {
            blk_unref(n->chip[i]);
        }
    }
    g_free(n->blank);
    g_free(n->buffer);
    g_free(n);
}

NANDImage *nand_image_open(DeviceState *owner, const char *dir,
                           const NANDImageGeometry *geo, Error **errp)
{
    g_autoptr(QDict) g = nand_image_geometry(dir, errp);
    const char *keys[] = { "chip_selects", "page_bytes", "spare_bytes", "pages_per_block", "blocks_per_ce" };
    const uint32_t want[] = { geo->chips, geo->page_bytes, geo->spare_bytes, geo->pages_per_block,
                              geo->blocks_per_chip };
    NANDImage *n;

    if (!g) {
        return NULL;
    }
    if (g_strcmp0(qdict_get_try_str(g, "storage_format"), NAND_IMAGE_FORMAT)) {
        error_setg(errp, "%s: not a " NAND_IMAGE_FORMAT " store", dir);
        return NULL;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(keys); i++) {
        QNum *v = qobject_to(QNum, qdict_get(g, keys[i]));
        uint64_t got;
        if (!v || !qnum_get_try_uint(v, &got) || got != want[i]) {
            error_setg(errp, "%s: geometry.json \"%s\" is not the model's %u", dir, keys[i], want[i]);
            return NULL;
        }
    }
    if (geo->chips > NAND_IMAGE_MAX_CHIPS) {
        error_setg(errp, "%s: %u chip selects is more than %d", dir, geo->chips, NAND_IMAGE_MAX_CHIPS);
        return NULL;
    }
    n = g_new0(NANDImage, 1);
    n->geo = *geo;
    n->stride = geo->page_bytes + geo->spare_bytes;
    n->blank = g_malloc(n->stride);
    n->buffer = g_malloc(n->stride);
    if (!nand_image_blank(qdict_get_try_str(g, "blank"), n->blank, geo->page_bytes, geo->spare_bytes)) {
        error_setg(errp, "%s: geometry.json names no blank record this model knows", dir);
        nand_image_free(n);
        return NULL;
    }
    for (unsigned i = 0; i < geo->chips; i++) {
        g_autofree char *path = g_strdup_printf("%s/cs%u.pages", dir, i);
        size_t size = (size_t)geo->blocks_per_chip * geo->pages_per_block * n->stride;
        n->chip[i] = nand_image_open_file(owner, path, size, true, errp);
        if (!n->chip[i]) {
            nand_image_free(n);
            return NULL;
        }
    }
    n->vm_state = qemu_add_vm_change_state_handler(nand_image_vm_state, n);
    return n;
}

static bool nand_image_bad(NANDImage *n, unsigned chip, uint32_t page)
{
    return chip >= n->geo.chips || page / n->geo.pages_per_block >= n->geo.blocks_per_chip;
}

static void nand_image_xor(NANDImage *n, uint8_t *out, const uint8_t *in)
{
    for (uint32_t i = 0; i < n->stride; i++) {
        out[i] = in[i] ^ n->blank[i];
    }
}

int nand_image_read(NANDImage *n, unsigned chip, uint32_t page, uint8_t *record)
{
    int result;

    if (nand_image_bad(n, chip, page)) {
        return -EINVAL;
    }
    /* bounded: nand_image_bad checked chip */
    result = blk_pread(n->chip[chip], (int64_t)page * n->stride, n->stride, record, 0);
    if (result < 0) {
        return result;
    }
    nand_image_xor(n, record, record);
    return 0;
}

int nand_image_write(NANDImage *n, unsigned chip, uint32_t page, const uint8_t *record)
{
    if (nand_image_bad(n, chip, page)) {
        return -EINVAL;
    }
    nand_image_xor(n, n->buffer, record);
    /* bounded: nand_image_bad checked chip */
    int result = blk_pwrite(n->chip[chip], (int64_t)page * n->stride, n->stride, n->buffer, 0);

    if (result < 0) {
        qatomic_set(&nand_image_failed, true);
    }
    return result;
}

int nand_image_erase(NANDImage *n, unsigned chip, uint32_t block)
{
    uint64_t first = (uint64_t)block * n->geo.pages_per_block;   /* 32 bits would wrap */

    if (first > UINT32_MAX || nand_image_bad(n, chip, first)) {
        return -EINVAL;
    }
    /* Zeros read as blank whether the host unmaps the range or writes them. */
    /* bounded: nand_image_bad checked chip */
    int result = blk_pwrite_zeroes(n->chip[chip], (int64_t)first * n->stride,
                                   (int64_t)n->geo.pages_per_block * n->stride, BDRV_REQ_MAY_UNMAP);

    if (result < 0) {
        qatomic_set(&nand_image_failed, true);
    }
    return result;
}
