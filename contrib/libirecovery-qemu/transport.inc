/* Emulator-only transport for upstream libirecovery. No physical USB fallback. */
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <stdatomic.h>

static int qio(int fd, void *buffer, size_t count, int writing)
{
    char *p = buffer;
    while (count) {
        ssize_t n = writing ? send(fd, p, count, 0) : recv(fd, p, count, 0);
        if (n <= 0) return -1;
        p += n;
        count -= n;
    }
    return 0;
}

/* RPC integers are little-endian; this adapter targets arm64/x86_64 macOS. */
static int qrpc(uint32_t op, const void *payload, uint32_t length,
                void *reply, uint32_t capacity)
{
    const char *path = getenv("IRECV_QEMU_SOCKET");
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    uint32_t request[2] = {op, length}, response[2];
    int fd, result = -1;
    if (!path || strlen(path) >= sizeof(address.sun_path)) return -1;
    strcpy(address.sun_path, path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval limit = { .tv_sec = 30 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        qio(fd, request, sizeof(request), 1) ||
        qio(fd, (void *)payload, length, 1) ||
        qio(fd, response, sizeof(response), 0)) goto done;
    if (response[1] > capacity) goto done;
    if (qio(fd, reply, response[1], 0)) goto done;
    result = (int32_t)response[0];
done:
    close(fd);
    return result;
}

static int qcontrol(uint8_t kind, uint8_t req, uint16_t val, uint16_t index,
                    unsigned char *data, uint16_t length, unsigned timeout)
{
    unsigned char *request = calloc(1, 12u + length);
    int result;
    if (!request) return -1;
    request[0] = kind; request[1] = req;
    memcpy(request + 2, &val, 2); memcpy(request + 4, &index, 2);
    memcpy(request + 6, &length, 2); memcpy(request + 8, &timeout, 4);
    if (!(kind & 128) && length) memcpy(request + 12, data, length);
    result = qrpc(2, request, 12 + ((kind & 128) ? 0 : length),
                  (kind & 128) ? data : NULL, (kind & 128) ? length : 0);
    free(request);
    return result;
}

static int qbulk(unsigned char ep, unsigned char *data, int length,
                 int *transferred, unsigned timeout)
{
    uint32_t header[3] = {ep, length, timeout};
    unsigned char *request;
    int result;
    if (!transferred || length < 0 || length > 1024*1024 - 12) return -1;
    *transferred = 0;
    request = malloc(12u + length);
    if (!request) return -1;
    memcpy(request, header, 12);
    if (!(ep & 128) && length) memcpy(request + 12, data, length);
    result = qrpc(3, request, 12 + ((ep & 128) ? 0 : length),
                  (ep & 128) ? data : NULL, (ep & 128) ? length : 0);
    free(request);
    if (result < 0) return result;
    *transferred = result;
    return 0;
}

static irecv_error_t qopen(irecv_client_t *out, uint64_t ecid)
{
    char data[2048];
    uint32_t mode;
    int size;
    irecv_client_t client;
    if (!out) return IRECV_E_INVALID_INPUT;
    *out = NULL;
    size = qrpc(1, NULL, 0, data, sizeof(data) - 1);
    if (size < 5 || size >= sizeof(data)) return IRECV_E_NO_DEVICE;
    data[size] = 0;
    memcpy(&mode, data, 4);
    client = calloc(1, sizeof(*client));
    if (!client) return IRECV_E_OUT_OF_MEMORY;
    client->mode = mode;
    irecv_load_device_info_from_iboot_string(client, data + 4);
    if (ecid && ecid != client->device_info.ecid) {
        irecv_close(client);
        return IRECV_E_NO_DEVICE;
    }
    client->handle = (void *)(uintptr_t)1;
    client->usb_config = 1;
    *out = client;
    return IRECV_E_SUCCESS;
}

struct qevents {
    pthread_t thread;
    atomic_int stop;
    irecv_device_event_cb_t callback;
    void *user_data;
};

static void *qevents_poll(void *opaque)
{
    struct qevents *events = opaque;
    irecv_client_t previous = NULL;
    while (!atomic_load(&events->stop)) {
        irecv_client_t current = NULL;
        qopen(&current, 0);
        if (previous && (!current || current->mode != previous->mode ||
                         current->device_info.ecid != previous->device_info.ecid)) {
            irecv_device_event_t event = {IRECV_DEVICE_REMOVE, previous->mode,
                                         &previous->device_info};
            events->callback(&event, events->user_data);
            irecv_close(previous); previous = NULL;
        }
        if (current && !previous) {
            irecv_device_event_t event = {IRECV_DEVICE_ADD, current->mode,
                                         &current->device_info};
            events->callback(&event, events->user_data);
            previous = current; current = NULL;
        }
        irecv_close(current);
        usleep(100000);
    }
    irecv_close(previous);
    return NULL;
}

static irecv_error_t qsubscribe(irecv_device_event_context_t *out,
                                irecv_device_event_cb_t callback, void *data)
{
    struct qevents *events;
    if (!out || !callback) return IRECV_E_INVALID_INPUT;
    events = calloc(1, sizeof(*events));
    if (!events) return IRECV_E_OUT_OF_MEMORY;
    events->callback = callback; events->user_data = data;
    if (pthread_create(&events->thread, NULL, qevents_poll, events)) {
        free(events); return IRECV_E_UNKNOWN_ERROR;
    }
    *out = (irecv_device_event_context_t)events;
    return IRECV_E_SUCCESS;
}

static irecv_error_t qunsubscribe(irecv_device_event_context_t context)
{
    struct qevents *events = (struct qevents *)context;
    if (!events) return IRECV_E_INVALID_INPUT;
    atomic_store(&events->stop, 1);
    pthread_join(events->thread, NULL);
    free(events);
    return IRECV_E_SUCCESS;
}
