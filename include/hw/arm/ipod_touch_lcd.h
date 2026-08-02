#ifndef IPOD_TOUCH_LCD_H
#define IPOD_TOUCH_LCD_H

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/arm/ipod_touch_multitouch.h"

#define TYPE_IPOD_TOUCH_LCD                "ipodtouch.lcd"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchLCDState, IPOD_TOUCH_LCD)

#define LCD_REFRESH_RATE_FREQUENCY 10

typedef struct IPodTouchLCDState
{
    SysBusDevice parent_obj;
    MemoryRegion *sysmem;
    MemoryRegion iomem;
    QemuConsole *con;
    IPodTouchMultitouchState *mt;
    int invalidate;
    uint8_t brightness;
    MemoryRegionSection fbsection;
    qemu_irq irq;
    uint32_t lcd_con;

    uint32_t w1_display_resolution_info;
    uint32_t w1_framebuffer_base;
    uint32_t w1_hspan;
    uint32_t w1_display_depth_info;

    uint32_t render;

    /*
     * Display rotation currently applied to the host window, in degrees
     * clockwise (0 / 90 / 180 / 270). The guest always renders into a portrait
     * 320x480 framebuffer, so a non-zero value means lcd_refresh transposes it
     * into a rotated surface -- and the pointer events coming back in console
     * coordinates have to be un-rotated before they reach the multitouch model.
     */
    int rotation;
    uint8_t *rotbuf;

    QEMUTimer *refresh_timer;
} IPodTouchLCDState;

void lcd_changebrightness(int brightness);

/*
 * Follow the accelerometer: turn the host window the same way the user "turned"
 * the device, so a landscape orientation gives a landscape (480x320) window.
 * Takes a UIDeviceOrientation value (1 portrait, 2 upside down, 3 landscape
 * left, 4 landscape right); anything else leaves the window in portrait.
 */
void it_display_set_orientation(uint32_t orientation);

#endif
