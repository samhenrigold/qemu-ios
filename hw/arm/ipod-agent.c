#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/cutils.h"
#include "system/rtc.h"
#include "hw/arm/ipod-agent.h"

typedef struct AgentItem {
    char *id;
    GByteArray *request;
    GByteArray *response;
    int32_t status;
    bool dispatched;
    uint32_t ui_pid;
    uint64_t ui_cookie;
    int64_t ui_deadline;
} AgentItem;

struct IPodAgent {
    int references;
    QemuMutex lock;
    GQueue pending, done;
    uint64_t token;
    uint64_t ui_sequence;
    int64_t last_poll;
    int64_t last_ui_poll;
    int64_t clock_offset;
    bool claimed;
    uint8_t report[IT_AGENT_REPORT_MAX];
    uint32_t report_len;
    uint64_t report_serial;
};

/* A report is its session's: whatever ends the session drops it, so no reader takes it for the guest's state now. */
static void agent_set_report(IPodAgent *a, const uint8_t *state, uint32_t len)
{
    if (len == a->report_len && (!len || !memcmp(a->report, state, len))) {
        return;
    }
    if (len) {
        memcpy(a->report, state, len);
    }
    a->report_len = len;
    a->report_serial++;
}

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
    a->references = 1;
    qemu_mutex_init(&a->lock);
    return a;
}

int64_t ipod_agent_rtc_now(void)
{
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    return mktimegm(&tm);
}

void ipod_agent_set_clock_offset(IPodAgent *a, int64_t offset)
{
    a->clock_offset = offset;
}

void ipod_agent_reset(IPodAgent *a)
{
    qemu_mutex_lock(&a->lock);
    g_queue_clear_full(&a->pending, item_free);
    g_queue_clear_full(&a->done, item_free);
    a->token = 0;
    a->claimed = false;
    agent_set_report(a, NULL, 0);
    qemu_mutex_unlock(&a->lock);
}

void ipod_agent_free(IPodAgent *a)
{
    if (!a || !g_atomic_int_dec_and_test(&a->references)) {
        return;
    }
    ipod_agent_reset(a);
    qemu_mutex_destroy(&a->lock);
    g_free(a);
}

/* Frontend calls may overlap machine cleanup. A published reference and a
 * per-call reference keep the mutex alive across that boundary. */
static GMutex published_lock;
static IPodAgent *published_agent;

void ipod_agent_publish(IPodAgent *a)
{
    g_mutex_lock(&published_lock);
    IPodAgent *old = published_agent;
    if (a) {
        g_atomic_int_inc(&a->references);
    }
    published_agent = a;
    g_mutex_unlock(&published_lock);
    ipod_agent_free(old);
}

IPodAgent *ipod_agent_acquire(void)
{
    g_mutex_lock(&published_lock);
    IPodAgent *a = published_agent;
    if (a) {
        g_atomic_int_inc(&a->references);
    }
    g_mutex_unlock(&published_lock);
    return a;
}

int ipod_agent_submit(IPodAgent *a, const char *request)
{
    const char *nl, *space;
    size_t len = strnlen(request, 2 * IT_AGENT_REQUEST_MAX + 1);
    gsize body_len;
    g_autofree uint8_t *body = NULL;
    AgentItem *item;
    int ret = -EBUSY;

    if (len > 2 * IT_AGENT_REQUEST_MAX) {
        return -EFBIG;
    }
    if (!(nl = strchr(request, '\n')) ||
        !(space = memchr(request, ' ', nl - request)) ||
        space == request || space - request > 64 || nl - space < 2 ||
        nl - request > 4096) {
        return -EINVAL;
    }
    for (const char *p = request; p < nl; p++) {
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126) {
            return -EINVAL;
        }
    }
    body = g_base64_decode(nl + 1, &body_len);
    /* GLib's decoder ignores invalid characters; require canonical encoding. */
    g_autofree char *encoded = g_base64_encode(body, body_len);
    if (strcmp(encoded, nl + 1)) {
        return -EINVAL;
    }
    if (nl - request + 1 + body_len > IT_AGENT_REQUEST_MAX) {
        return -EFBIG;
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
            ret = 0;
        }
    }
    qemu_mutex_unlock(&a->lock);
    if (ret) {
        item_free(item);
    }
    return ret;
}

const char *ipod_agent_submit_error(int error)
{
    return error == -EFBIG ? "Agent request over 256 KiB: send large files as putpart chunks (it_agent v3)" :
           error == -EBUSY ? "Duplicate agent request id or full agent request queue" :
                             "Invalid agent request";
}

bool ipod_agent_cancel(IPodAgent *a, const char *id)
{
    bool found = false;
    qemu_mutex_lock(&a->lock);
    for (GList *p = a->pending.head; p; p = p->next) {
        AgentItem *item = p->data;
        if (!strcmp(item->id, id)) {
            if (item->dispatched) {
                /* The old daemon sees a failed POLL and kills its child group.
                 * Late WRITE/DONE calls cannot complete another request. */
                a->claimed = false;
                a->token = 0;
                agent_set_report(a, NULL, 0);
            }
            g_queue_delete_link(&a->pending, p);
            item_free(item);
            found = true;
            break;
        }
    }
    if (!found) {
        for (GList *p = a->done.head; p; p = p->next) {
            AgentItem *item = p->data;
            if (!strcmp(item->id, id)) {
                g_queue_delete_link(&a->done, p);
                item_free(item);
                found = true;
                break;
            }
        }
    }
    qemu_mutex_unlock(&a->lock);
    return found;
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

char *ipod_agent_report(IPodAgent *a, int64_t now_ms, uint64_t *serial)
{
    char *state = NULL;
    qemu_mutex_lock(&a->lock);
    if (now_ms - a->last_poll > IT_AGENT_LEASE_MS) {
        agent_set_report(a, NULL, 0);
    }
    if (a->report_serial != *serial) {
        *serial = a->report_serial;
        state = strndup((const char *)a->report, a->report_len);   /* the app frees it with free() */
    }
    qemu_mutex_unlock(&a->lock);
    return state;
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
            agent_set_report(a, NULL, 0);
            result = a->token;
        }
        goto out;
    }
    /* UI clients run in the target process, independently of the root daemon.
     * Cookies reject late replies; like root tokens, they are not credentials. */
    if (op >= 0x166 && op <= 0x169) {
        if (op == 0x166) {
            a->last_ui_poll = now_ms;
        }
        if (!item || !item->ui_cookie || now_ms >= item->ui_deadline) {
            result = op == 0x166 ? 0 : -1;
            goto out;
        }
        if (op == 0x166) {
            result = offset == item->ui_pid ? item->ui_cookie : 0;
            goto out;
        }
        if (token != item->ui_cookie) {
            goto out;
        }
        /* Reuse the bounded window operations for this exclusively routed item. */
        op -= 5;
    } else {
        if (!a->claimed || token != a->token) {
            goto out;
        }
        if (item && item->ui_cookie && op != 0x161 && op != 0x165) {
            goto out;
        }
        if (op == 0x16a && item && offset != 1) {
            /* No it_typein has polled lately: this guest's apps carry none (the armv7 packages), or SpringBoard is
             * not up. Refused here, the agent answers its host at once (-EIO), where the request would otherwise
             * wait out its deadline for a poller that isn't coming. */
            if (now_ms - a->last_ui_poll > IT_AGENT_UI_ALIVE_MS) {
                goto out;
            }
            item->ui_pid = offset;
            item->ui_cookie = (candidate_token ^ ++a->ui_sequence) & INT64_MAX;
            if (!item->ui_cookie) {
                item->ui_cookie = ++a->ui_sequence;
            }
            item->ui_deadline = now_ms + 5000;
            result = 0;
            goto out;
        }
    }
    if (op == 0x161) {
        a->last_poll = now_ms;
        /* The poll's window is the agent's state report (it_agent v6); an older agent's is empty. */
        if (length && length <= sizeof(chunk) && !copy(opaque, address, chunk, length, false)) {
            agent_set_report(a, chunk, length);
        }
        if (item && item->ui_cookie && now_ms >= item->ui_deadline) {
            item->status = -ETIMEDOUT;
            g_byte_array_set_size(item->response, 0);
            g_queue_push_tail(&a->done, g_queue_pop_head(&a->pending));
            item = g_queue_peek_head(&a->pending);
        }
        if (item) {
            item->dispatched = true;
        }
        result = item && !item->ui_cookie ? item->request->len : 0;
    } else if (op == 0x165) {
        result = ipod_agent_rtc_now() + a->clock_offset;
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
