/* Physical module reset must detach and discard firmware/queues, whereas
 * release reattaches once. No general USB link-reset behavior is changed.
 * SLICE hw/usb/dev-bcm-hsic.c fn bcm_power_reset
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef struct {
    bool attached;
} USBDevice;
typedef struct {
    USBDevice dev;
    bool reset_asserted, running, control_notified, control_prefixed;
    unsigned interrupt_seq, bulk_rx_seq, bulk_tx_seq, bulk_rx_offset;
    int fullmac;
    GQueue *control_rx, *data_rx;
} BCMHSICState;
#define BCM_HSIC(x) ((BCMHSICState *)(x))
#define USB_DEVICE(x) (&((BCMHSICState *)(x))->dev)
static unsigned detached, attached, cleared, firmware_reset;
static int error_abort;
static void usb_device_detach(USBDevice *d)
{
    assert(d->attached);
    d->attached = false;
    detached++;
}
static void usb_device_attach(USBDevice *d, int *e)
{
    (void)e;
    assert(!d->attached);
    d->attached = true;
    attached++;
}
static void bcm_download_reset(BCMHSICState *s)
{
    s->running = false;
    firmware_reset++;
}
static void bcm_fullmac_reset(int *f)
{
    *f = 0;
    cleared++;
}
#include "slice.h"
int main(void)
{
    BCMHSICState s = {.dev.attached = true,
                      .running = true,
                      .control_notified = true,
                      .control_prefixed = true,
                      .interrupt_seq = 3,
                      .bulk_rx_seq = 4,
                      .bulk_tx_seq = 5,
                      .bulk_rx_offset = 7,
                      .fullmac = 1,
                      .control_rx = g_queue_new(),
                      .data_rx = g_queue_new()};
    g_queue_push_tail(s.control_rx, g_bytes_new_static("a", 1));
    g_queue_push_tail(s.data_rx, g_bytes_new_static("b", 1));
    bcm_power_reset(&s, 0, 1);
    assert(!s.dev.attached && !s.running && detached == 1 && cleared == 1 &&
           firmware_reset == 1);
    assert(g_queue_is_empty(s.control_rx) && g_queue_is_empty(s.data_rx));
    assert(!s.control_notified && !s.control_prefixed && !s.interrupt_seq &&
           !s.bulk_rx_seq && !s.bulk_tx_seq && !s.bulk_rx_offset);
    bcm_power_reset(&s, 0, 1);
    assert(detached == 1 && firmware_reset == 1);
    bcm_power_reset(&s, 0, 0);
    assert(s.dev.attached && attached == 1);
    bcm_power_reset(&s, 0, 0);
    assert(attached == 1);
    g_queue_free(s.control_rx);
    g_queue_free(s.data_rx);
    puts(
        "PASS: physical WLAN reset clears state, detaches and reattaches once");
}
