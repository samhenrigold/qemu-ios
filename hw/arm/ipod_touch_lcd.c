#include "hw/arm/ipod_touch_lcd.h"
#include "migration/vmstate.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "exec/cpu-common.h"

int lcd_brightness = 255;

#define LCD_FB_WIDTH  320
#define LCD_FB_HEIGHT 480

/*
 * Rotation the host window should be presenting, in degrees clockwise. Written
 * by it_display_set_orientation() (accelerometer / Command+Left / Command+Right
 * / the accel-orientation QMP property) and picked up by the next refresh,
 * which owns the console resize.
 *
 * iOS keeps its framebuffer in portrait and rotates the UI *inside* it, so to
 * show an upright landscape image the model has to turn the picture back the
 * same way the user "turned" the device.
 */
static int it_display_rotation_req;

static uint64_t ipod_touch_lcd_read(void *opaque, hwaddr addr, unsigned size)
{
    // printf("%s: read from location 0x%08x\n", __func__, addr);

    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;
    switch(addr)
    {
        case 0x0:
            return 2;
        case 0x4:
            return s->lcd_con;
        case 0xC:
            return 0x1; //s->unknown1;
        case 0x20:
            return s->w1_display_depth_info;
        case 0x24:
            return s->w1_framebuffer_base;
        case 0x28:
            return s->w1_hspan;
        case 0x30:
            return s->w1_display_resolution_info;
        case 0x1b10:
            return 2;
	case 0x1b14:
	    return 0x3;
        default:
            /*
             * IT_LCD_READY: report "ready" for every register this model does
             * not implement. 3.1.3's AppleM2TVOut close path sleeps waiting on
             * three independent ready handshakes (SDO_CLKCON & 0x2,
             * enable.reg.clkgating_rdy, mix_ctrl.reg.reg_mixer_ready_clk_down)
             * and logs "TVOUT SHUT DOWN PROBLEM: ..." when they never assert.
             * That wait happens on the SpringBoard thread, so the UI never
             * comes up. Reads seen spinning here: 0x410 (status beside the
             * 0x408/0x40c indirect address/data port, polled thousands of
             * times) and the 0x1b80..0x1bb0 mixer/SDO status block.
             *
             * Returning all-ones satisfies any `status & mask` readiness test.
             * It is a bring-up probe, not a model: once the specific bits are
             * known they should be implemented properly per register.
             */
            if (getenv("IT_LCD_READY")) {
                return 0xffffffff;
            }
            printf("%s: read invalid location 0x%08x.\n", __func__, addr);
            break;
    }
    return 0;
}

static void ipod_touch_lcd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;
    // printf("%s: writing 0x%08x to 0x%08x\n", __func__, val, addr);

    if (getenv("LCD_TRACE")) {
        fprintf(stderr, "[LCD] wr [0x%04x] <- 0x%08x\n", (unsigned)addr, (unsigned)val);
    }

    switch(addr) {
        case 0x4:
            s->lcd_con = val;
            break;
        case 0xC:
            s->render = val;
            qemu_irq_lower(s->irq);
	    // qemu_irq_raise(s->irq);
            break;
        case 0x20:
            s->w1_display_depth_info = val;
            break;
        case 0x24:
            s->w1_framebuffer_base = val;
            break;
        case 0x28:
            s->w1_hspan = val;
            break;
        case 0x30:
            s->w1_display_resolution_info = val;
            break;
    }
}

void lcd_changebrightness(int brightness)
{
    lcd_brightness = brightness & 0xFF;
    if (getenv("LCD_TRACE")) {
        fprintf(stderr, "[LCD] brightness <- %d\n", lcd_brightness);
    }
}

/*
 * IT_LCD_BRIGHT overrides the panel backlight level the guest programmed.
 * draw_line32_32 scales every channel by lcd_brightness/255, so a guest that
 * leaves the backlight at 0 produces a solid-black screendump even when the
 * framebuffer is full of pixels. Useful both as a probe and for guests whose
 * backlight register we do not model.
 */
static int lcd_bright_effective(void)
{
    static int ovr = -2;
    if (ovr == -2) {
        const char *v = getenv("IT_LCD_BRIGHT");
        ovr = v ? atoi(v) : -1;
    }
    return ovr >= 0 ? ovr : lcd_brightness;
}

void it_display_set_orientation(uint32_t orientation)
{
    int rot;

    switch (orientation) {
    case 2:  rot = 180; break;   /* portrait upside down */
    case 3:  rot = 270; break;   /* landscape left  - device turned ccw */
    case 4:  rot = 90;  break;   /* landscape right - device turned cw  */
    default: rot = 0;   break;   /* portrait, face up/down, unset */
    }
    if (rot != it_display_rotation_req) {
        it_display_rotation_req = rot;
        if (getenv("LCD_TRACE")) {
            fprintf(stderr, "[LCD] orientation %u -> window rotation %d\n",
                    orientation, rot);
        }
    }
}

static void lcd_invalidate(void *opaque)
{
    IPodTouchLCDState *s = opaque;
    s->invalidate = 1;
}

static void draw_line32_32(void *opaque, uint8_t *d, const uint8_t *s, int width, int deststep)
{
    // IPodTouchLCDState *lcd = (IPodTouchLCDState *) opaque;
    uint8_t r, g, b;

    do {
        //v = lduw_le_p((void *) s);
        //printf("V: %d\n", *s);
        b = s[0];
        g = s[1];
        r = s[2];
        // printf("R: %d, G: %d, B: %d, A: %d\n", r, g, b, lcd_brightness);
        int bri = lcd_bright_effective();
        ((uint32_t *) d)[0] = rgb_to_pixel32(round((float)r * ((float)bri / 255.0f)), round((float)g * ((float)bri / 255.0f)), round((float)b * ((float)bri / 255.0f)));
        s += 4;
        d += 4;
    } while (-- width != 0);
}

/*
 * Blit the guest's portrait framebuffer into a rotated host surface.
 *
 * framebuffer_update_display() can only walk source and destination with fixed
 * strides, which cannot express a transpose, so the rotated case gets its own
 * blit. The whole frame is redrawn every time (the portrait path already forces
 * invalidate on every refresh, so this costs no more than the status quo).
 */
static void lcd_refresh_rotated(IPodTouchLCDState *lcd, DisplaySurface *surface,
                                int rot)
{
    const int sw = LCD_FB_WIDTH, sh = LCD_FB_HEIGHT;
    int dw = (rot == 180) ? sw : sh;
    int dh = (rot == 180) ? sh : sw;
    uint8_t *dst = surface_data(surface);
    int dstride = surface_stride(surface);
    int bri = lcd_bright_effective();
    int sx, sy;

    if (surface_width(surface) != dw || surface_height(surface) != dh) {
        return; /* resize has not landed yet; skip this frame */
    }
    if (!lcd->rotbuf) {
        lcd->rotbuf = g_malloc(sw * sh * 4);
    }
    cpu_physical_memory_read(lcd->w1_framebuffer_base, lcd->rotbuf, sw * sh * 4);

    for (sy = 0; sy < sh; sy++) {
        const uint8_t *s = lcd->rotbuf + (size_t)sy * sw * 4;
        for (sx = 0; sx < sw; sx++, s += 4) {
            int dx, dy;
            uint8_t b = s[0], g = s[1], r = s[2];

            switch (rot) {
            case 90:  dx = sh - 1 - sy; dy = sx;          break;
            case 270: dx = sy;          dy = sw - 1 - sx; break;
            default:  dx = sw - 1 - sx; dy = sh - 1 - sy; break;
            }
            ((uint32_t *)(dst + (size_t)dy * dstride))[dx] =
                rgb_to_pixel32((r * bri + 127) / 255,
                               (g * bri + 127) / 255,
                               (b * bri + 127) / 255);
        }
    }
    dpy_gfx_update(lcd->con, 0, 0, dw, dh);
}

static void lcd_refresh(void *opaque)
{
    // printf("%s: refreshing LCD screen\n", __func__);

    IPodTouchLCDState *lcd = (IPodTouchLCDState *) opaque;
    DisplaySurface *surface = qemu_console_surface(lcd->con);
    drawfn draw_line;
    int src_width, dest_width;
    int height, first, last;
    int width, linesize;

    if (!lcd || !lcd->con || !surface_bits_per_pixel(surface))
        return;

    /*
     * Pick up a pending orientation change. The console resize has to happen
     * from the graphics update, not from the I2C/QMP thread that moved the
     * accelerometer.
     */
    if (lcd->rotation != it_display_rotation_req) {
        int rot = it_display_rotation_req;
        bool land = (rot == 90 || rot == 270);

        lcd->rotation = rot;
        qemu_console_resize(lcd->con,
                            land ? LCD_FB_HEIGHT : LCD_FB_WIDTH,
                            land ? LCD_FB_WIDTH  : LCD_FB_HEIGHT);
        surface = qemu_console_surface(lcd->con);
        lcd->invalidate = 1;
        if (!surface_bits_per_pixel(surface)) {
            return;
        }
    }

    if (lcd->rotation != 0) {
        lcd_refresh_rotated(lcd, surface, lcd->rotation);
        lcd->invalidate = 0;
        return;
    }

    dest_width = 4;
    draw_line = draw_line32_32;

    /* Resolution */
    first = last = 0;
    width = 320;
    height = 480;
    lcd->invalidate = 1;

    src_width =  4 * width;
    linesize = surface_stride(surface);

    if(lcd->invalidate) {
        framebuffer_update_memory_section(&lcd->fbsection, lcd->sysmem, lcd->w1_framebuffer_base, height, 4 * width);
    }

    framebuffer_update_display(surface, &lcd->fbsection,
                               width, height,
                               src_width,       /* Length of source line, in bytes.  */
                               linesize,        /* Bytes between adjacent horizontal output pixels.  */
                               dest_width,      /* Bytes between adjacent vertical output pixels.  */
                               lcd->invalidate,
                               draw_line, NULL,
                               &first, &last);
    if (first >= 0) {
        dpy_gfx_update(lcd->con, 0, first, width, last - first + 1);
    }
    lcd->invalidate = 0;
}

static const MemoryRegionOps lcd_ops = {
    .read = ipod_touch_lcd_read,
    .write = ipod_touch_lcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const GraphicHwOps gfx_ops = {
    .invalidate  = lcd_invalidate,
    .gfx_update  = lcd_refresh,
};

/*
 * Undo the rotation lcd_refresh_rotated() applied, so the multitouch model
 * always sees coordinates in the guest's portrait framebuffer space (x left to
 * right, y measured from the bottom). Without this, touch lands in the wrong
 * place -- or a transposed place -- as soon as the window is turned.
 *
 * x/y come in as 0..2^15 fractions of the *window*, y measured downwards, which
 * is what both the legacy mouse handler and the multi-touch handler below get.
 */
static void ipod_touch_lcd_map_touch(IPodTouchLCDState *lcd, int x, int y,
                                     float *fx, float *fy)
{
    float cx = x / (float)pow(2, 15);
    float cy = y / (float)pow(2, 15);

    switch (lcd->rotation) {
    case 90:  *fx = cy;        *fy = cx;        break;
    case 270: *fx = 1.0f - cy; *fy = 1.0f - cx; break;
    case 180: *fx = 1.0f - cx; *fy = cy;        break;
    default:  *fx = cx;        *fy = 1.0f - cy; break;
    }
}

static void ipod_touch_lcd_mouse_event(void *opaque, int x, int y, int z, int buttons_state)
{
    // printf("x %d y %d z %d state %d\n", x, y, z, buttons_state);

    IPodTouchLCDState *lcd = (IPodTouchLCDState *) opaque;
    float fx, fy;

    ipod_touch_lcd_map_touch(lcd, x, y, &fx, &fy);

    /*
     * Only the position moves here. prev_touch_* is the reference the reported
     * velocity is measured from, and get_frame() advances it when a frame is
     * actually emitted -- moving it on every host event instead silently
     * dropped the distance covered by any motion the report-rate coalescer
     * skipped, and left a finger held still reporting the speed of its last
     * step forever.
     */
    lcd->mt->touch_x = fx;
    lcd->mt->touch_y = fy;

    if(buttons_state && !lcd->mt->touch_down) {
        ipod_touch_multitouch_on_touch(lcd->mt);
    }
    else if(!buttons_state && lcd->mt->touch_down) {
        ipod_touch_multitouch_on_release(lcd->mt);
    }
    else if(buttons_state) {
        /* Drag: report the motion now rather than waiting for the next tick. */
        ipod_touch_multitouch_on_motion(lcd->mt);
    }
}

/*
 * Multi-touch input.
 *
 * The legacy mouse handler above can only ever describe one contact, so pinch
 * and rotate were simply unreachable however good the digitizer model got.
 * QEMU's own multi-touch event kind (INPUT_EVENT_KIND_MTT, "mtt") carries a
 * slot and a tracking id per contact and is what a touchscreen frontend or QMP
 * `input-send-event` sends, so the panel accepts that directly and hands each
 * slot to the digitizer.
 *
 * The wire protocol is the usual two-phase one: DATA events carry the X and Y
 * for a slot, then BEGIN/UPDATE/END commits them. Positions are therefore
 * latched per slot until a commit arrives -- an END for a slot we never saw is
 * ignored rather than reported at (0,0), which would be a phantom tap in the
 * corner.
 *
 * Slots beyond MT_MAX_FINGERS are dropped by the digitizer, not here, so the
 * limit lives in one place.
 */
static void ipod_touch_lcd_mtt_event(DeviceState *dev, QemuConsole *src,
                                     InputEvent *evt)
{
    IPodTouchLCDState *lcd = IPOD_TOUCH_LCD(dev);
    InputMultiTouchEvent *mtt = evt->u.mtt.data;
    int slot = mtt->slot;
    float fx, fy;

    if (!lcd->mt || slot < 0 || slot >= MT_MAX_FINGERS) {
        return;
    }

    switch (mtt->type) {
    case INPUT_MULTI_TOUCH_TYPE_DATA:
        if (mtt->axis == INPUT_AXIS_X) {
            lcd->mtt_x[slot] = mtt->value;
        } else {
            lcd->mtt_y[slot] = mtt->value;
        }
        lcd->mtt_seen[slot] = true;
        return;

    case INPUT_MULTI_TOUCH_TYPE_BEGIN:
    case INPUT_MULTI_TOUCH_TYPE_UPDATE:
        if (!lcd->mtt_seen[slot]) {
            return;
        }
        ipod_touch_lcd_map_touch(lcd, lcd->mtt_x[slot], lcd->mtt_y[slot],
                                 &fx, &fy);
        ipod_touch_multitouch_set_finger(lcd->mt, slot, fx, fy, true);
        return;

    case INPUT_MULTI_TOUCH_TYPE_END:
    case INPUT_MULTI_TOUCH_TYPE_CANCEL:
        if (!lcd->mtt_seen[slot]) {
            return;
        }
        ipod_touch_lcd_map_touch(lcd, lcd->mtt_x[slot], lcd->mtt_y[slot],
                                 &fx, &fy);
        ipod_touch_multitouch_set_finger(lcd->mt, slot, fx, fy, false);
        lcd->mtt_seen[slot] = false;
        return;

    default:
        return;
    }
}

static const QemuInputHandler ipod_touch_lcd_mtt_handler = {
    .name  = "iPod Touch Multitouch",
    .mask  = INPUT_EVENT_MASK_MTT,
    .event = ipod_touch_lcd_mtt_event,
};

static void refresh_timer_tick(void *opaque)
{
    IPodTouchLCDState *s = (IPodTouchLCDState *)opaque;

    if (s->render == 0x1)
	qemu_irq_raise(s->irq);
    else if (s->render == 0xFF)
	qemu_irq_lower(s->irq);

    timer_mod(s->refresh_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);//LCD_REFRESH_RATE_FREQUENCY);
}

/*
 * Clear the display controller's registers on a warm reset.
 *
 * These are all programmed fresh by iBoot and the kernel on every boot, but
 * until they are the model would otherwise answer reads with the previous
 * boot's values and keep scanning out of the old framebuffer base. invalidate
 * is set so the first frame after the reset is drawn in full rather than
 * diffed against whatever was on screen before.
 */
static void ipod_touch_lcd_reset(DeviceState *dev)
{
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);

    s->lcd_con = 0;
    s->render = 0;
    s->w1_display_resolution_info = 0;
    s->w1_framebuffer_base = 0;
    s->w1_hspan = 0;
    s->w1_display_depth_info = 0;
    s->invalidate = 1;
    memset(&s->fbsection, 0, sizeof(s->fbsection));
    /*
     * The device comes up portrait. it_display_rotation_req is file-scope and
     * survived reset, so a reboot taken in landscape inherited the previous
     * boot's rotation. s->rotation is deliberately NOT cleared here: the
     * refresh path resizes the console only when rotation != request, so
     * leaving the applied value is what makes it rotate back. rotbuf is a
     * fixed-size scratch buffer refilled from guest memory every frame, not
     * state.
     */
    it_display_rotation_req = 0;
    if (getenv("LCD_TRACE")) {
        fprintf(stderr, "[LCD] ==== reset ====\n");
    }
}

static void ipod_touch_lcd_realize(DeviceState *dev, Error **errp)
{
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);
    s->con = graphic_console_init(dev, 0, &gfx_ops, s);
    qemu_console_resize(s->con, 320, 480);

    // add mouse handler
    qemu_add_mouse_event_handler(ipod_touch_lcd_mouse_event, s, 1, "iPod Touch Touchscreen");

    /* ... and the multi-touch one, which routes by event mask and so coexists
     * with the mouse rather than replacing it. */
    qemu_input_handler_register(dev, &ipod_touch_lcd_mtt_handler);

    // initialize the refresh timer
    s->refresh_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, refresh_timer_tick, s);
    timer_mod(s->refresh_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / LCD_REFRESH_RATE_FREQUENCY);
}

static void ipod_touch_lcd_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchLCDState *s = IPOD_TOUCH_LCD(dev);

    memory_region_init_io(&s->iomem, obj, &lcd_ops, s, "lcd", 0x10000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

/*
 * con, sysmem, mt, irq and refresh_timer are all machine wiring, rebuilt by
 * realize on the destination before the snapshot is read. rotbuf is a scratch
 * buffer refilled from guest memory every frame. fbsection is a cached
 * MemoryRegionSection - a host pointer - and must never be migrated; it is
 * rebuilt from w1_framebuffer_base. post_load forces a full repaint, otherwise
 * the destination's blank surface is diffed against a framebuffer it never
 * drew and most of the screen stays black.
 */
static int ipod_touch_lcd_post_load(void *opaque, int version_id)
{
    IPodTouchLCDState *s = opaque;

    memset(&s->fbsection, 0, sizeof(s->fbsection));
    s->invalidate = 1;
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_lcd = {
    .name = "ipod_touch_lcd",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ipod_touch_lcd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(brightness, IPodTouchLCDState),
        VMSTATE_UINT32(lcd_con, IPodTouchLCDState),
        VMSTATE_UINT32(w1_display_resolution_info, IPodTouchLCDState),
        VMSTATE_UINT32(w1_framebuffer_base, IPodTouchLCDState),
        VMSTATE_UINT32(w1_hspan, IPodTouchLCDState),
        VMSTATE_UINT32(w1_display_depth_info, IPodTouchLCDState),
        VMSTATE_UINT32(render, IPodTouchLCDState),
        VMSTATE_INT32(rotation, IPodTouchLCDState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_lcd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_lcd_realize;
    dc->reset = ipod_touch_lcd_reset;
    dc->vmsd = &vmstate_ipod_touch_lcd;
}

static const TypeInfo ipod_touch_lcd_info = {
    .name          = TYPE_IPOD_TOUCH_LCD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchLCDState),
    .instance_init = ipod_touch_lcd_init,
    .class_init    = ipod_touch_lcd_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_lcd_info);
}

type_init(ipod_touch_machine_types)
