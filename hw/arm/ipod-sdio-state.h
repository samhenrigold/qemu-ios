/* Bounded serialization of sparse firmware memory and partially read frames.
 * Dropping these loses control replies and the dongle's interrupt masks. */
#define SDIO_SNAPSHOT_PAGES 4096
#define SDIO_SNAPSHOT_FRAMES 256
#define SDIO_SNAPSHOT_FRAME_BYTES 65536

static int sdio_put_backplane(QEMUFile *f, void *pv, size_t size,
                              const VMStateField *field, JSONWriter *vmdesc)
{
    GHashTable *pages = *(GHashTable **)pv;
    if (g_hash_table_size(pages) > SDIO_SNAPSHOT_PAGES) return -E2BIG;
    qemu_put_be32(f, g_hash_table_size(pages));
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, pages);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        qemu_put_be32(f, GPOINTER_TO_UINT(key));
        qemu_put_buffer(f, value, BACKPLANE_PAGE_SIZE);
    }
    return qemu_file_get_error(f);
}

static int sdio_get_backplane(QEMUFile *f, void *pv, size_t size,
                              const VMStateField *field)
{
    GHashTable *pages = *(GHashTable **)pv;
    uint32_t count = qemu_get_be32(f);
    if (count > SDIO_SNAPSHOT_PAGES) return -EINVAL;
    g_hash_table_remove_all(pages);
    for (unsigned i = 0; i < count; i++) {
        uint32_t key = qemu_get_be32(f);
        if (key > (UINT32_MAX >> BACKPLANE_PAGE_BITS) ||
            g_hash_table_contains(pages, GUINT_TO_POINTER(key))) return -EINVAL;
        uint8_t *page = g_malloc(BACKPLANE_PAGE_SIZE);
        if (qemu_get_buffer(f, page, BACKPLANE_PAGE_SIZE) != BACKPLANE_PAGE_SIZE) {
            g_free(page);
            return -EIO;
        }
        g_hash_table_insert(pages, GUINT_TO_POINTER(key), page);
    }
    return qemu_file_get_error(f);
}

static void sdio_free_frame(gpointer opaque)
{
    SDPCMFrame *frame = opaque;
    g_free(frame->data);
    g_free(frame);
}

static int sdio_put_frames(QEMUFile *f, void *pv, size_t size,
                           const VMStateField *field, JSONWriter *vmdesc)
{
    GQueue *queue = *(GQueue **)pv;
    if (queue->length > SDIO_SNAPSHOT_FRAMES) return -E2BIG;
    qemu_put_be32(f, queue->length);
    for (GList *item = queue->head; item; item = item->next) {
        SDPCMFrame *frame = item->data;
        if (!frame->len || frame->len > SDIO_SNAPSHOT_FRAME_BYTES ||
            frame->read_off >= frame->len) return -EINVAL;
        qemu_put_be32(f, frame->len);
        qemu_put_be32(f, frame->read_off);
        qemu_put_buffer(f, frame->data, frame->len);
    }
    return qemu_file_get_error(f);
}

static int sdio_get_frames(QEMUFile *f, void *pv, size_t size,
                           const VMStateField *field)
{
    GQueue *queue = *(GQueue **)pv;
    uint32_t count = qemu_get_be32(f);
    if (count > SDIO_SNAPSHOT_FRAMES) return -EINVAL;
    g_queue_clear_full(queue, sdio_free_frame);
    for (unsigned i = 0; i < count; i++) {
        uint32_t len = qemu_get_be32(f), offset = qemu_get_be32(f);
        if (!len || len > SDIO_SNAPSHOT_FRAME_BYTES || offset >= len) return -EINVAL;
        SDPCMFrame *frame = g_new0(SDPCMFrame, 1);
        frame->len = len;
        frame->read_off = offset;
        frame->data = g_malloc(len);
        if (qemu_get_buffer(f, frame->data, len) != len) {
            sdio_free_frame(frame);
            return -EIO;
        }
        g_queue_push_tail(queue, frame);
    }
    return qemu_file_get_error(f);
}
