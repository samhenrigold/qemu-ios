/* Native USB setup clears halt on each configured pipe before opening it.
 * SLICE hw/usb/dev-mdm9x15.c fn mdm_control
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define MDM_INTERFACES 13
#define EndpointOutRequest 0x0200
#define USB_REQ_CLEAR_FEATURE 1
#define USB_DIR_IN 0x80
#define USB_TOKEN_IN 0x69
#define USB_TOKEN_OUT 0xe1
#define USB_ENDPOINT_XFER_BULK 2
#define USB_ENDPOINT_XFER_INT 3
#define USB_ENDPOINT_XFER_INVALID 255
#define USB_RET_STALL -3
typedef struct {
    unsigned type;
    bool halted;
} USBEndpoint;
typedef struct {
    USBEndpoint in[16], out[16];
} USBDevice;
typedef struct {
    int actual_length, status;
} USBPacket;
typedef struct {
    bool runtime;
} Download;
typedef struct {
    USBDevice parent;
    Download *download;
} MDM9x15State;
#define MDM9X15(dev) ((MDM9x15State *)(dev))
static unsigned receives, reads, ready;
static USBEndpoint *usb_ep_get(USBDevice *dev, int pid, int nr)
{
    assert(nr > 0 && nr < 16);
    return pid == USB_TOKEN_IN ? &dev->in[nr] : &dev->out[nr];
}
static void usb_ep_set_halted(USBDevice *dev, int pid, int nr, bool halted)
{
    usb_ep_get(dev, pid, nr)->halted = halted;
}
static int usb_desc_handle_control(USBDevice *dev, USBPacket *p, int request,
                                   int value, int index, int length,
                                   uint8_t *data)
{
    return request == 9 ? 0 : -1;
}
static void mdm_log(MDM9x15State *s, const char *name, unsigned a, unsigned b,
                    unsigned c)
{
}
static void mdm_qmi_ready(MDM9x15State *s)
{
    ready++;
}
static void mdm_receive(MDM9x15State *s, unsigned iface, const uint8_t *data,
                        unsigned length)
{
    assert(iface == 3 && length == 4);
    receives++;
}
static int mdm_read(MDM9x15State *s, unsigned iface, uint8_t *data,
                    unsigned length)
{
    assert(iface == 3 && length == 4);
    reads++;
    return 2;
}
#include "slice.h"
static int request(MDM9x15State *s, int req, int value, int index, int length)
{
    USBPacket p = {0};
    uint8_t data[4] = {0};
    mdm_control(&s->parent, &p, req, value, index, length, data);
    return p.status ? p.status : p.actual_length;
}
int main(void)
{
    Download runtime = {.runtime = true};
    MDM9x15State s = {.download = &runtime};
    for (unsigned i = 0; i < 16; i++) {
        s.parent.in[i].type = s.parent.out[i].type = USB_ENDPOINT_XFER_INVALID;
    }
    s.parent.out[1] = (USBEndpoint){USB_ENDPOINT_XFER_BULK, true};
    s.parent.in[1] = (USBEndpoint){USB_ENDPOINT_XFER_BULK, true};
    s.parent.in[2] = (USBEndpoint){USB_ENDPOINT_XFER_INT, true};
    assert(request(&s, 0x0201, 0, 1, 0) == 0 && !s.parent.out[1].halted);
    assert(s.parent.in[1].halted); /* Direction selects an independent pipe. */
    assert(request(&s, 0x0201, 0, 0x81, 0) == 0 && !s.parent.in[1].halted);
    assert(request(&s, 0x0201, 0, 0x82, 0) == 0 && !s.parent.in[2].halted);
    assert(request(&s, 0x0201, 0, 0x82, 0) == 0); /* Already clear is legal. */
    const int invalid[] = {0, 0x80, 2, 15, 0x91, 0x181, -1};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        assert(request(&s, 0x0201, 0, invalid[i], 0) == USB_RET_STALL);
    }
    s.parent.in[1].halted = true;
    assert(request(&s, 0x0201, 1, 0x81, 0) == USB_RET_STALL);
    assert(request(&s, 0x0201, 0, 0x81, 1) == USB_RET_STALL);
    assert(s.parent.in[1].halted);
    assert(request(&s, 0x0203, 0, 0x81, 0) == USB_RET_STALL);
    assert(request(&s, 9, 1, 0, 0) == 0 && ready == 1);
    assert(request(&s, 0x2100, 0, 3, 4) == 4 && receives == 1);
    assert(request(&s, 0xa101, 0, 3, 4) == 2 && reads == 1);
    assert(request(&s, 0x2122, 0, 3, 0) == 0);
    assert(request(&s, 0x2100, 0, 13, 4) == USB_RET_STALL && receives == 1);
    puts(
        "PASS: USB endpoint halt recovery, direction isolation, invalid endpoint/feature rejection and QMI control requests");
}
