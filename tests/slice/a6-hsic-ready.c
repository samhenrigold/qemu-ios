/* Native N42 readiness wiring must never acknowledge a missing peer.
 * SLICE hw/arm/s5l8950.c fn a6_hsic_ready a6_wlan_reset
 */
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef struct {
    bool attached, bcm;
    int reset;
} USBDevice;
#define OBJECT(x) (x)
#define DEVICE(x) (x)
static void *object_dynamic_cast(USBDevice *d, const char *t)
{
    (void)t;
    return d->bcm ? d : NULL;
}
static int *qdev_get_gpio_in_named(USBDevice *d, const char *n, int i)
{
    (void)n;
    (void)i;
    return &d->reset;
}
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
    int levels[232];
} GPIO;
typedef struct {
    USBBus *hsic_bus;
    GPIO *gpio;
    USBDevice *baseband_usb;
} S5L8950State;
#define QTAILQ_FOREACH(p, h, f) for ((p) = (h)->first; (p); (p) = (p)->f)
static int *qdev_get_gpio_in(GPIO *g, int pin)
{
    assert(pin < 232);
    return &g->levels[pin];
}
static void qemu_set_irq(int *irq, int level)
{
    *irq = level;
}
#include "slice.h"
int main(void)
{
    GPIO gpio = {0};
    S5L8950State s = {.gpio = &gpio};
    USBBus bus = {0};
    a6_hsic_ready(&s, 0, 1);
    assert(!gpio.levels[211]);
    s.hsic_bus = &bus;
    a6_hsic_ready(&s, 1, 1);
    assert(!gpio.levels[57]);
    USBDevice modem = {.attached = true}, wifi = {.attached = false};
    USBPort p4 = {.path = "4", .dev = &wifi},
            p2 = {.path = "2", .dev = &modem, .next = &p4};
    bus.used.first = &p2;
    a6_hsic_ready(&s, 0, 1);
    assert(gpio.levels[211]);
    a6_hsic_ready(&s, 1, 1);
    assert(!gpio.levels[57] && gpio.levels[211]);
    wifi.attached = true;
    a6_hsic_ready(&s, 1, 1);
    assert(gpio.levels[57]);
    a6_hsic_ready(&s, 0, 0);
    assert(!gpio.levels[211] && gpio.levels[57]);
    modem.attached = false;
    a6_hsic_ready(&s, 0, 1);
    assert(!gpio.levels[211]);
    p4.dev = NULL;
    a6_hsic_ready(&s, 1, 1);
    assert(!gpio.levels[57]);
    p4.dev = &wifi;
    wifi.bcm = true;
    a6_wlan_reset(&s, 0, 0);
    assert(wifi.reset == 1 && !gpio.levels[57]);
    a6_wlan_reset(&s, 0, 1);
    assert(wifi.reset == 0 && gpio.levels[57]);
    wifi.attached = false;
    a6_wlan_reset(&s, 0, 1);
    assert(!gpio.levels[57]);
    wifi.bcm = false;
    wifi.attached = true;
    a6_wlan_reset(&s, 0, 1);
    assert(!gpio.levels[57]);
    puts(
        "PASS: HSIC host-ready acknowledgement, absent/disconnected peers and independent modem/WLAN GPIOs");
}
