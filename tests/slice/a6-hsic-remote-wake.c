/* The HSIC peers' remote-wake lines (N42 DT function-remote_wake, active
 * high) idle low and rise only when a peer queues data for the host, which
 * AppleUSBHSICPort samples on every port suspend: idle high, they resumed both
 * links at once, 15 times a second with no traffic.
 * SLICE hw/arm/s5l8950.c fn a6_wire_remote_wake
 * SLICE hw/usb/dev-bcm-hsic.c fn bcm_fullmac_output bcm_data
 * SLICE hw/usb/dev-mdm9x15.c fn mdm_wake mdm_data
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int *qemu_irq;
static void qemu_set_irq(qemu_irq irq, int level)
{
    if (irq) {
        *irq = level;
    }
}
static void qemu_irq_raise(qemu_irq irq)
{
    qemu_set_irq(irq, 1);
}
static void qemu_irq_lower(qemu_irq irq)
{
    qemu_set_irq(irq, 0);
}

enum { USB_TOKEN_IN = 0x69, USB_TOKEN_OUT = 0xe1 };
enum { USB_RET_SUCCESS = 0, USB_RET_NAK = -2, USB_RET_STALL = -3 };
typedef struct {
    const void *usb_desc;
    qemu_irq remote_wake;
    bool bcm;
} USBDevice;
typedef struct {
    int nr;
} USBEndpoint;
typedef struct {
    size_t size;
} QEMUIOVector;
typedef struct {
    int pid, status;
    USBEndpoint *ep;
    QEMUIOVector iov;
    unsigned actual_length;
} USBPacket;
static unsigned wakeups;
static USBEndpoint endpoint;
static USBEndpoint *usb_ep_get(USBDevice *d, int pid, int ep)
{
    (void)d, (void)pid;
    endpoint.nr = ep;
    return &endpoint;
}
static void usb_wakeup(USBEndpoint *ep, unsigned stream)
{
    (void)ep, (void)stream;
    wakeups++;
}
static void usb_packet_copy(USBPacket *p, void *data, size_t n)
{
    (void)data;
    p->actual_length = n;
}
#define qemu_log_mask(...) ((void)0)

/* The A6 machine: GPIO pins as levels, rest levels, named outputs. */
typedef struct {
    int level[232];
    bool rest_low[232];
} GPIO;
typedef struct USBPort {
    char path[16];
    USBDevice *dev;
    struct USBPort *next;
} USBPort;
typedef struct {
    struct {
        USBPort *first;
    } used;
} USBBus;
typedef struct {
    USBBus *hsic_bus;
    GPIO *gpio;
    USBDevice *baseband_usb;
} S5L8950State;
typedef int Notifier;
static S5L8950State *machine;
#define S5L8950_MACHINE(x) (x)
#define qdev_get_machine() machine
#define QTAILQ_FOREACH(p, h, f) for ((p) = (h)->first; (p); (p) = (p)->f)
#define OBJECT(x) (x)
#define DEVICE(x) (x)
static void *object_dynamic_cast(USBDevice *d, const char *type)
{
    assert(!strcmp(type, "usb-bcm-hsic"));
    return d->bcm ? d : NULL;
}
static void s5l8930_gpio_set_rest_level(GPIO *g, unsigned pin, bool level)
{
    g->rest_low[pin] = !level;
    g->level[pin] = level;
}
static int *qdev_get_gpio_in(GPIO *g, int pin)
{
    return &g->level[pin];
}
static void qdev_connect_gpio_out_named(USBDevice *d, const char *name, int n,
                                        qemu_irq pin)
{
    assert(!strcmp(name, "remote-wake") && !n);
    d->remote_wake = pin;
}

/* usb-bcm-hsic */
#define BCM_FULLMAC_CONTROL 0
#define BCM_DL_READY 1
#define BCM_DL_RUNNABLE 4
#define BCM_DL_IMAGE_TOOBIG 7
#define BCM_DL_MAX (8 * 1024 * 1024)
typedef struct {
    void *nic;
} BCMFullmacState;
typedef struct {
    USBDevice parent_obj;
    unsigned log_count, dl_state, bulk_error_log;
    bool running, control_notified, control_prefixed;
    uint16_t interrupt_seq, bulk_rx_seq;
    GQueue *control_rx, *data_rx;
    BCMFullmacState fullmac;
    qemu_irq remote_wake;
} BCMHSICState;
#define BCM_HSIC(x) ((BCMHSICState *)(x))
static void bcm_make_header(uint8_t *h, unsigned c, uint16_t seq, uint16_t n)
{
    (void)c, (void)seq, (void)n;
    memset(h, 0, 16);
}
static unsigned bcm_notification(BCMHSICState *s, uint8_t *n)
{
    (void)s;
    memset(n, 0, 8);
    return 8;
}
static bool bcm_bulk_read(BCMHSICState *s, USBPacket *p)
{
    GBytes *frame = g_queue_pop_head(s->data_rx);
    if (frame) {
        usb_packet_copy(p, NULL, g_bytes_get_size(frame));
        g_bytes_unref(frame);
    }
    return frame;
}
static bool bcm_download_data(BCMHSICState *s, uint8_t *d, size_t n)
{
    (void)s, (void)d, (void)n;
    return false;
}
static bool bcm_bulk_input(BCMHSICState *s, const uint8_t *d, size_t n)
{
    (void)s, (void)d, (void)n;
    return true;
}
static size_t qemu_iovec_to_buf(QEMUIOVector *v, size_t o, void *b, size_t n)
{
    (void)v, (void)o;
    memset(b, 0, n);
    return n;
}

/* usb-mdm9x15 */
#define MDM_INTERFACES 13
#define MDM_MAX_FRAME 64
typedef struct {
    USBDevice parent_obj;
    GQueue rx[MDM_INTERFACES];
    qemu_irq remote_wake;
} MDM9x15State;
#define MDM9X15(x) ((MDM9x15State *)(x))
#define USB_DEVICE(x) (&(x)->parent_obj)
static bool mdm_is_notification(USBDevice *dev, unsigned ep)
{
    (void)dev;
    return ep == 2;
}
static int mdm_read(MDM9x15State *s, unsigned iface, uint8_t *out, size_t cap)
{
    GBytes *b = g_queue_pop_head(&s->rx[iface]);
    (void)out, (void)cap;
    if (!b) {
        return 0;
    }
    g_bytes_unref(b);
    return 1;
}
static void mdm_receive(MDM9x15State *s, unsigned i, const uint8_t *p, size_t n)
{
    (void)s, (void)i, (void)p, (void)n;
}
static void mdm_log(MDM9x15State *s, const char *k, unsigned a, unsigned b,
                    unsigned c)
{
    (void)s, (void)k, (void)a, (void)b, (void)c;
}

#include "slice.h"

static void poll_in(void (*data)(USBDevice *, USBPacket *), USBDevice *dev,
                    int ep, int expect)
{
    USBEndpoint e = {.nr = ep};
    USBPacket p = {.pid = USB_TOKEN_IN, .ep = &e, .iov.size = 64};
    data(dev, &p);
    assert(p.status == expect);
}

int main(void)
{
    GPIO gpio = {0};
    for (unsigned i = 0; i < 232; i++) { /* the GPIO model's inputs idle high */
        gpio.level[i] = 1;
    }
    BCMHSICState bcm = {.parent_obj.bcm = true,
                        .running = true,
                        .control_rx = g_queue_new(),
                        .data_rx = g_queue_new()};
    MDM9x15State mdm = {0};
    for (unsigned i = 0; i < MDM_INTERFACES; i++) {
        g_queue_init(&mdm.rx[i]);
    }
    USBPort wlan = {.path = "4", .dev = &bcm.parent_obj};
    USBBus bus = {.used.first = &wlan};
    S5L8950State s = {.hsic_bus = &bus, .gpio = &gpio,
                      .baseband_usb = &mdm.parent_obj};
    machine = &s;

    a6_wire_remote_wake(NULL, NULL);
    assert(gpio.rest_low[10] && !gpio.level[10]);
    assert(gpio.rest_low[200] && !gpio.level[200]);
    /* The stub keeps the connection on the USBDevice; the models own it. */
    bcm.remote_wake = bcm.parent_obj.remote_wake;
    mdm.remote_wake = mdm.parent_obj.remote_wake;
    assert(bcm.remote_wake == &gpio.level[200]);
    assert(mdm.remote_wake == &gpio.level[10]);

    /* A firmware event for a suspended host raises GPIO 25.0; the host's
     * next poll, even one the module NAKs, shows the link is back up. */
    bcm_fullmac_output(&bcm, 1, (const uint8_t *)"e", 1);
    assert(gpio.level[200] && wakeups == 1);
    poll_in(bcm_data, &bcm.parent_obj, 1, USB_RET_NAK);
    assert(!gpio.level[200] && g_queue_get_length(bcm.data_rx) == 1);
    poll_in(bcm_data, &bcm.parent_obj, 2, USB_RET_SUCCESS);
    assert(!gpio.level[200] && g_queue_is_empty(bcm.data_rx));

    /* The modem's QMI indication does the same on GPIO 1.2. */
    g_queue_push_tail(&mdm.rx[1], g_bytes_new_static("q", 1));
    mdm_wake(&mdm, 2);
    assert(gpio.level[10] && wakeups == 2);
    poll_in(mdm_data, &mdm.parent_obj, 2, USB_RET_SUCCESS);
    assert(!gpio.level[10]);
    poll_in(mdm_data, &mdm.parent_obj, 3, USB_RET_NAK);
    assert(!gpio.level[10]);

    g_queue_free(bcm.control_rx);
    g_queue_free(bcm.data_rx);
    puts("PASS: HSIC remote-wake lines idle low and rise for queued data");
}
