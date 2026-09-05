#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "hw/arm/ipod-agent.h"

typedef struct AgentItem {
    char *id;
    GByteArray *request;
    GByteArray *response;
    int32_t status;
    bool dispatched;
} AgentItem;

struct IPodAgent {
    QemuMutex lock;
    GQueue pending, done;
    uint64_t token;
    int64_t last_poll;
    bool claimed;
};

static void item_free(void *p)
{
    AgentItem *item = p;
    g_free(item->id);
    g_byte_array_unref(item->request);
    g_byte_array_unref(item->response);
    g_free(item);
}

IPodAgent *ipod_agent_new(void)
{
    IPodAgent *a = g_new0(IPodAgent, 1);
    qemu_mutex_init(&a->lock);
    return a;
}

void ipod_agent_reset(IPodAgent *a)
{
    qemu_mutex_lock(&a->lock);
    g_queue_clear_full(&a->pending, item_free);
    g_queue_clear_full(&a->done, item_free);
    a->token = 0;
    a->claimed = false;
    qemu_mutex_unlock(&a->lock);
}

void ipod_agent_free(IPodAgent *a)
{
    ipod_agent_reset(a);
    qemu_mutex_destroy(&a->lock);
    g_free(a);
}

bool ipod_agent_submit(IPodAgent *a, const char *request)
{
    const char *nl, *space;
    size_t len = strnlen(request, 2 * IT_AGENT_REQUEST_MAX + 1);
    gsize body_len;
    g_autofree uint8_t *body = NULL;
    AgentItem *item;
    bool ok = false;

    if (len > 2 * IT_AGENT_REQUEST_MAX || !(nl = strchr(request, '\n')) ||
        !(space = memchr(request, ' ', nl - request)) ||
        space == request || space - request > 64 || nl - space < 2 ||
        nl - request > 4096) {
        return false;
    }
    for (const char *p = request; p < nl; p++) {
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126) {
            return false;
        }
    }
    body = g_base64_decode(nl + 1, &body_len);
    /* GLib's decoder ignores invalid characters; require canonical encoding. */
    g_autofree char *encoded = g_base64_encode(body, body_len);
    if (strcmp(encoded, nl + 1) || nl - request + 1 + body_len > IT_AGENT_REQUEST_MAX) {
        return false;
    }
    item = g_new0(AgentItem, 1);
    item->id = g_strndup(request, space - request);
    item->request = g_byte_array_new();
    item->response = g_byte_array_new();
    g_byte_array_append(item->request, (const uint8_t *)request, nl - request + 1);
    g_byte_array_append(item->request, body, body_len);
    qemu_mutex_lock(&a->lock);
    if (a->pending.length + a->done.length < IT_AGENT_QUEUE_MAX) {
        bool duplicate = false;
        GQueue *queues[] = { &a->pending, &a->done };
        for (unsigned i = 0; i < G_N_ELEMENTS(queues); i++) {
            for (GList *p = queues[i]->head; p; p = p->next) {
                duplicate |= !strcmp(((AgentItem *)p->data)->id, item->id);
            }
        }
        if (!duplicate) {
            g_queue_push_tail(&a->pending, item);
            ok = true;
        }
    }
    qemu_mutex_unlock(&a->lock);
    if (!ok) {
        item_free(item);
    }
    return ok;
}

char *ipod_agent_take_result(IPodAgent *a)
{
    char *result;
    qemu_mutex_lock(&a->lock);
    AgentItem *item = g_queue_pop_head(&a->done);
    qemu_mutex_unlock(&a->lock);
    if (!item) {
        return g_strdup("");
    }
    g_autofree char *body = g_base64_encode(item->response->data, item->response->len);
    result = g_strdup_printf("%s %d\n%s", item->id, item->status, body);
    item_free(item);
    return result;
}

const char *ipod_agent_status(IPodAgent *a, int64_t now_ms)
{
    const char *status;
    qemu_mutex_lock(&a->lock);
    status = !a->claimed ? "absent" :
             now_ms - a->last_poll > IT_AGENT_LEASE_MS ? "stale" : "alive";
    qemu_mutex_unlock(&a->lock);
    return status;
}

int64_t ipod_agent_call(IPodAgent *a, unsigned op, uint64_t token,
                       uint32_t address, uint32_t offset, uint32_t length,
                       int64_t now_ms, uint64_t candidate_token,
                       IPodAgentCopy copy, void *opaque)
{
    int64_t result = -1;
    uint8_t chunk[IT_AGENT_CHUNK_MAX];
    qemu_mutex_lock(&a->lock);
    AgentItem *item = g_queue_peek_head(&a->pending);
    if (op == 0x160) {
        result = 0;
        if (!a->claimed || now_ms - a->last_poll > IT_AGENT_LEASE_MS) {
            /* A lost daemon may have executed a command. Never replay it. */
            if (a->claimed && item && item->dispatched) {
                item->status = -ECONNRESET;
                g_byte_array_set_size(item->response, 0);
                g_queue_push_tail(&a->done, g_queue_pop_head(&a->pending));
            }
            a->token = candidate_token & INT64_MAX;
            if (!a->token) {
                a->token = 1;
            }
            a->claimed = true;
            a->last_poll = now_ms;
            result = a->token;
        }
        goto out;
    }
    if (!a->claimed || token != a->token) {
        goto out;
    }
    if (op == 0x161) {
        a->last_poll = now_ms;
        if (item) {
            item->dispatched = true;
        }
        result = item ? item->request->len : 0;
    } else if (op == 0x165) {
        result = time(NULL);
    } else if (item && op == 0x162 && length <= sizeof(chunk) &&
               offset <= item->request->len) {
        length = MIN(length, item->request->len - offset);
        if (!copy(opaque, address, item->request->data + offset, length, true)) {
            result = length;
        }
    } else if (item && op == 0x163 && length <= sizeof(chunk) &&
               offset <= IT_AGENT_RESPONSE_MAX - length &&
               (offset == 0 || offset == item->response->len)) {
        /* Failed guest reads leave the previous staging buffer intact. */
        if (!copy(opaque, address, chunk, length, false)) {
            if (!offset) {
                g_byte_array_set_size(item->response, 0);
            }
            g_byte_array_append(item->response, chunk, length);
            result = length;
        }
    } else if (item && op == 0x164) {
        item->status = (int32_t)offset;
        g_queue_push_tail(&a->done, g_queue_pop_head(&a->pending));
        result = 0;
    }
out:
    qemu_mutex_unlock(&a->lock);
    return result;
}
