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

/*
 * Panel frame interrupt period. 60 Hz is what the guest's display driver is
 * programmed for; the constant used to be spelled inline as
 * NANOSECONDS_PER_SECOND / 60 next to a dead reference to
 * LCD_REFRESH_RATE_FREQUENCY (10 Hz), which is not the rate anything runs at.
 */
#define LCD_VSYNC_PERIOD_NS (NANOSECONDS_PER_SECOND / 60)

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
    /*
     * What the surface currently on screen was drawn from. The blit only
     * redraws the lines the guest wrote since the last frame, so anything that
     * changes the meaning of "unchanged" without dirtying guest memory has to
     * be noticed here and turned into a full repaint: a flip to a different
     * scanout buffer, a new host surface, or a new backlight level (which
     * rescales every pixel with no guest write behind it).
     */
    uint32_t fbsection_base;
    void *last_surface;
    int last_bright;
    /*
     * Host time of the last frame pushed by the panel's frame interrupt, in
     * QEMU_CLOCK_REALTIME ns. QEMU's own display poll asks for a second
     * conversion of a frame already on screen; that is dropped while this shows
     * the vsync push is running. See lcd_refresh().
     */
    int64_t last_present_ns;
    qemu_irq irq;
    uint32_t lcd_con;
    uint32_t plane_regs[0x300 / 4];
    uint32_t plane_scanout[0x300 / 4];

    uint32_t w1_display_resolution_info;
    uint32_t w1_framebuffer_base;
    /*
     * The base actually being scanned out. The panel latches the register at
     * vblank, so a blit driven from anywhere other than the frame interrupt
     * (QEMU's 30 ms display poll, a screendump) still sees a coherent frame
     * rather than whichever buffer the guest had installed at that instant.
     */
    uint32_t scanout_base;
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

    /*
     * Latched multi-touch contact positions, in console coordinates (0..2^15).
     * QEMU's mtt protocol delivers a slot's X and Y as separate DATA events and
     * only commits them with a following BEGIN/UPDATE/END, so each slot's last
     * reported position has to be held here until the commit arrives.
     * mtt_seen distinguishes "no position yet" from "position (0,0)", which
     * would otherwise be reported to the guest as a tap in the corner.
     */
    int mtt_x[MT_MAX_FINGERS];
    int mtt_y[MT_MAX_FINGERS];
    bool mtt_seen[MT_MAX_FINGERS];

    QEMUTimer *refresh_timer;

    /*
     * Absolute deadline of the next frame interrupt, in QEMU_CLOCK_VIRTUAL ns.
     * The tick advances this by exactly one period instead of re-arming from
     * "now", so the callback's own dispatch latency is not folded into the
     * frame rate. See refresh_timer_tick().
     */
    int64_t next_vsync;
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
