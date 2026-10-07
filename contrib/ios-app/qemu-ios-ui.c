/*
 * The bridge between this emulator and a host iOS app: frames out, touches in.
 *
 * Deliberately NOT a `-display ios` backend. A registered display type has to
 * be declared in qapi/ui.json and selected on the command line, and none of
 * that buys anything here -- the app is in the same process and can simply
 * call these functions. So the VM runs with `-display none` and the app
 * attaches afterwards.
 *
 * THREADING is the whole difficulty. Everything below marked "QEMU thread"
 * runs under the BQL on the thread executing qemu_ios_main(); everything
 * marked "app thread" is called from UIKit. The two never touch the same
 * state without going through a bottom half, because QEMU's console and input
 * layers assume the BQL is held and UIKit assumes the main thread.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/seqlock.h"
#include "qemu/aio.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "qapi/error.h"
#include "hw/arm/ipod_touch_buttons.h"
#include "hw/core/boards.h"

void gles_host_set_allowed(bool allowed);
bool ipod_touch_fmss_io_failed(void);
bool ipod_touch_nor_io_failed(void);
bool s5l8930_iop_io_failed(void);
bool ipod_touch_mipi_dsi_panel_off(void);
bool s5l8930_d1815_guest_shutdown_confirmed(void);

#include "qemu-ios-ui.h"
#include "virtual-input.h"

static void ios_sequence_cancel_current(void);
static bool ios_manual_touch;
static bool ios_manual_touch2;
static void ios_sequence_reset(void *opaque)
{
    ios_sequence_cancel_current();
}
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "hw/arm/ipod_touch_lcd.h"

#include <sys/resource.h>

#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-commands-misc.h"
#include "migration/misc.h"
#include "migration/migration.h"      /* migrate_get_current, MigrationState.state */
#include "system/runstate.h"
#include "system/reset.h"

#define IOS_MAX_SLOTS 8

/*
 * Frames rotate through a ring rather than a single buffer so the app can wrap
 * the published one in a CGImage and hand it to the compositor WITHOUT copying
 * it: by the time the emulator writes this buffer again, IOS_FRAME_BUFFERS
 * frames have gone by and the image is long since drawn. One buffer would mean
 * either a copy per frame or the guest repainting pixels that are on screen.
 */
#define IOS_FRAME_BUFFERS 3

/* Set once the console/display are attached, cleared when the main loop returns. */
static int ios_vm_alive;
/* Which machine is running, latched on the QEMU thread when the VM starts. */
static int ios_is_ipad1;
static int ios_dsi_panel;       /* the panel is powered over DSI: the iPad's and the S5L8920 boards' */

static struct {
    DisplayChangeListener dcl;
    QemuConsole *con;
    bool attached;

    /* Written on the QEMU thread, read by the app thread under frame_lock. */
    QemuMutex frame_lock;
    void *buf[IOS_FRAME_BUFFERS];
    size_t buf_size;          /* capacity of each, never shrinks */
    GSList *retired;          /* buffers a CGImage may still be reading */
    int published;            /* index of the newest complete frame, -1 if none */
    int width, height;
    uint64_t serial;          /* bumped on every completed frame */

    qemu_ios_frame_cb cb;
    void *cb_opaque;

    struct touch_slot slots[INPUT_EVENT_SLOTS_MAX];

    /* Set by a partial update, cleared when a whole frame is published. */
    bool pending;
} ios;

/* Frame polling can begin even when startup fails before attach(). Initialize
 * on every entry path through GLib's once primitive, including concurrent first
 * reads. The lock lives for the process; published buffers outlive VM exit. */
static void ios_init_frame_lock(void)
{
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        qemu_mutex_init(&ios.frame_lock);
        ios.published = -1;
        g_once_init_leave(&initialized, 1);
    }
}

/* --- frames out: QEMU thread ------------------------------------------- */

/*
 * Time to the first frame the guest has actually DRAWN.
 *
 * "First frame" on its own is worthless as a boot measurement: the panel
 * publishes a blank surface the moment the console exists, which is a few
 * milliseconds in and says nothing about the guest. The first frame with
 * light in it is the earliest honest sign the guest is alive, and it is the
 * only figure comparable between TCG backends.
 *
 * Sampled sparsely and only until it fires, so it costs nothing thereafter.
 */
static void ios_report_first_lit_frame(const uint8_t *bgra, int w, int h)
{
    static bool reported;
    static int64_t first_capture_ns;
    size_t pixels, lit = 0, i;

    if (reported) {
        return;
    }
    if (first_capture_ns == 0) {
        first_capture_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }

    pixels = (size_t)w * h;
    for (i = 0; i < pixels; i += 64) {
        const uint8_t *p = bgra + i * 4;
        if (p[0] > 24 || p[1] > 24 || p[2] > 24) {
            lit++;
        }
    }

    /* A handful of stray pixels is noise; a drawn screen is nothing like it. */
    if (lit * 64 > pixels / 100) {
        struct rusage ru;
        double cpu = 0;

        reported = true;

        /*
         * CPU time as well as wall time, because wall time cannot compare TCG
         * backends on this workload: boot is mostly fixed timer and I/O waits,
         * so a backend that burns half the cycles still finishes at about the
         * same moment. CPU seconds for identical guest work is the honest
         * measure of interpreter throughput.
         */
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
                + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        }

        fprintf(stderr, "[boot] first lit frame after %.1f s wall, %.1f s cpu\n",
                (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - first_capture_ns)
                    / 1e9, cpu);
    }
}

/*
 * Copy the surface rather than handing the app QEMU's pixels directly: the
 * surface can be freed or reallocated by dpy_gfx_switch at any moment, and a
 * UIKit frame in flight referencing freed pixman memory is a use-after-free
 * that would only show up under load. This is the ONLY copy -- the app reads
 * the published buffer in place.
 */
static void ios_capture(DisplaySurface *surface)
{
    int w = surface_width(surface);
    int h = surface_height(surface);
    int stride = surface_stride(surface);
    const uint8_t *src = surface_data(surface);
    size_t need = (size_t)w * h * 4;
    int next, y;
    uint8_t *dst;

    ios_init_frame_lock();
    qemu_mutex_lock(&ios.frame_lock);

    if (need > ios.buf_size) {
        /*
         * Growing (in practice only a rotation) retires the old buffers rather
         * than freeing them: the app may still hold a CGImage over one. They
         * are released at the NEXT growth, seconds away at the very least.
         */
        int i;
        for (i = 0; i < IOS_FRAME_BUFFERS; i++) {
            if (ios.buf[i]) {
                ios.retired = g_slist_prepend(ios.retired, ios.buf[i]);
            }
            ios.buf[i] = g_malloc0(need);
        }
        ios.buf_size = need;
        ios.published = -1;
    }

    /*
     * A panel that redraws whole frames every refresh (s5l8930_display, and
     * any panel while the VM is paused) repaints identical pixels. Publishing
     * them would wake the helper's copy and the app's texture upload for
     * nothing, so an unchanged frame keeps the old serial.
     */
    if (ios.published >= 0 && ios.width == w && ios.height == h) {
        const uint8_t *last = ios.buf[ios.published];
        for (y = 0; y < h; y++) {
            if (memcmp(last + (size_t)y * w * 4, src + (size_t)y * stride,
                       (size_t)w * 4)) {
                break;
            }
        }
        if (y == h) {
            qemu_mutex_unlock(&ios.frame_lock);
            return;
        }
    }

    next = (ios.published + 1) % IOS_FRAME_BUFFERS;
    dst = ios.buf[next];
    for (y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * w * 4, src + (size_t)y * stride, (size_t)w * 4);
    }

    ios.width = w;
    ios.height = h;
    ios.published = next;
    ios.serial++;
    qemu_mutex_unlock(&ios.frame_lock);

    ios_report_first_lit_frame(dst, w, h);

    if (ios.cb) {
        ios.cb(ios.cb_opaque);
    }
}

/*
 * Note what changed; do not publish yet.
 *
 * Now that the panel does proper dirty tracking, one guest frame arrives as
 * SEVERAL partial updates. Publishing each one hands the app a frame caught
 * mid-paint, which is visible as the screen wiping top-to-bottom instead of
 * changing at once -- and costs a full-surface copy per fragment rather than
 * per frame. Frames are published at the refresh boundary instead, where the
 * picture is whole.
 */
static void ios_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    ios.pending = true;
}

static void ios_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *surface)
{
    /*
     * A new surface replaces everything, so it is whole by definition -- but
     * do NOT capture it here: on a rotation the console resize happens in the
     * middle of the panel's refresh, BEFORE it has blitted anything into the
     * new surface, so capturing now publishes one all-black frame at the new
     * size and the app visibly blinks. Marking it pending instead lets
     * ios_refresh publish it right after graphic_hw_update returns, by which
     * point the same refresh has painted the surface.
     */
    if (surface) {
        ios.pending = true;
    }
}

static void ios_refresh(DisplayChangeListener *dcl)
{
    DisplaySurface *surface;

    graphic_hw_update(dcl->con);

    if (!ios.pending) {
        return;
    }
    surface = qemu_console_surface(dcl->con);
    if (surface) {
        ios_capture(surface);
        ios.pending = false;
    }
}

static const DisplayChangeListenerOps ios_dcl_ops = {
    .dpy_name       = "ios",
    .dpy_refresh    = ios_refresh,
    .dpy_gfx_update = ios_gfx_update,
    .dpy_gfx_switch = ios_gfx_switch,
};

/*
 * Called from qemu_ios_main() once qemu_init() has returned, on the QEMU
 * thread with the BQL held.
 *
 * This cannot be driven from the app side. Before qemu_init() there is no AIO
 * context and no console, so scheduling the registration as a bottom half from
 * the app -- the obvious design -- dereferences a null context and segfaults
 * on the app's main thread.
 */
/*
 * Whether the emulator is up and its AioContext is safe to schedule onto.
 *
 * qemu_get_aio_context() returns NULL until qemu_init() has run, and after the
 * main loop returns the context is no longer serviced -- so a bottom half
 * scheduled outside that window either dereferences NULL on the app's main
 * thread or is silently dropped (leaking its payload). The app starts QEMU on a
 * background thread and shows its window immediately, so key/mouse/tilt events
 * genuinely can arrive on both sides of that window. Every ABI entry point that
 * schedules work checks this first.
 */
bool qemu_ios_ui_ready(void)
{
    return qatomic_read(&ios_vm_alive) != 0;
}

bool qemu_ios_ui_guest_shutdown_confirmed(void)
{
    return qatomic_read(&ios_is_ipad1) ? s5l8930_d1815_guest_shutdown_confirmed()
                                       : pcf50633_guest_shutdown_confirmed();
}

bool qemu_ios_ui_display_sleeping(void)
{
    /* The iPad has no iPod LCD backlight; its panel is powered over DSI. */
    return qatomic_read(&ios_dsi_panel) ? ipod_touch_mipi_dsi_panel_off()
                                       : lcd_backlight_is_off();
}

int qemu_ios_ui_backlight_level(void)
{
    return ios_backlight_level();
}

bool qemu_ios_ui_storage_failed(void)
{
    return ipod_touch_fmss_io_failed() || ipod_touch_nor_io_failed() ||
           s5l8930_iop_io_failed();
}

void qemu_ios_ui_vm_stopped(void)
{
    ios_sequence_cancel_current();
    qatomic_set(&ios_vm_alive, 0);
    /* Also drop `attached`. It used to stay true for the life of the process,
     * so every entry point below that guarded on it kept accepting work after
     * the main loop had gone -- most damagingly snapshot_save2, which then
     * queued a BH nobody would ever run and pinned its status at RUNNING,
     * stalling the app's save poll for its full timeout on every quit that
     * races the VM exiting. */
    ios.attached = false;
}

void qemu_ios_ui_vm_started(void)
{
    int i;

    if (ios.attached) {
        return;
    }
    ios.con = qemu_console_lookup_by_index(0);
    if (!ios.con) {
        return;
    }
    qatomic_set(&ios_is_ipad1, object_dynamic_cast(qdev_get_machine(),
                                                   MACHINE_TYPE_NAME("ipad1")) != NULL);
    qatomic_set(&ios_dsi_panel, qatomic_read(&ios_is_ipad1) ||
                object_dynamic_cast(qdev_get_machine(), "s5l8920-machine") != NULL);
    for (i = 0; i < INPUT_EVENT_SLOTS_MAX; i++) {
        ios.slots[i].tracking_id = -1;
    }
    ios.dcl.ops = &ios_dcl_ops;
    ios.dcl.con = ios.con;
    register_displaychangelistener(&ios.dcl);
    ios.attached = true;
    qemu_register_reset(ios_sequence_reset, NULL);
    qatomic_set(&ios_vm_alive, 1);
}

/* --- app thread -------------------------------------------------------- */

/*
 * Every machine's facts for the app (qemu-ios-ui.h). Panel limits are the
 * machines' panel= setters': the CLCD boards' even width 64..1024 by 64..511
 * rows (the S5L8720 window keeps 9 bits of height), the A4 pipe's width a
 * multiple of 16 with both sides 64..2047, inside iBoot's display region
 * (9 MB at 4 bytes a pixel, 0x4f700000 up to the iPad's DRAM end).
 */
#define CLCD_PANEL 64, 1024, 511, 2, 0
#define A4_PANEL   64, 2047, 2047, 16, 0x900000 / 4
static const QemuIosDeviceInfo ios_devices[] = {
    /* machine          board    w     h    scale orient cell   usbhost compass charger panel */
    { "iPod-Touch",    "n72ap",  320,  480, 1, 0, false, false, false, false, CLCD_PANEL },
    { "iPod-Touch-1G", "n45ap",  320,  480, 1, 0, false, false, false, false, CLCD_PANEL },
    { "iPhone-2G",     "m68ap",  320,  480, 1, 0, true,  false, false, false, CLCD_PANEL },
    { "n18",           "n18ap",  320,  480, 1, 0, false, false, false, false, CLCD_PANEL },
    { "n88",           "n88ap",  320,  480, 1, 0, true,  false, false, false, CLCD_PANEL },
    /* s5l8930_display scans out 1024x768: the K48 panel is mounted landscape */
    { "ipad1",         "k48ap", 1024,  768, 1, 1, false, true,  true,  true,  A4_PANEL },
    { "iPod-Touch-4G", "n81ap",  640,  960, 2, 0, false, true,  false, false, A4_PANEL },
    { "iPhone-4",      "n90ap",  640,  960, 2, 0, true,  true,  true,  false, A4_PANEL },
};

const QemuIosDeviceInfo *qemu_ios_device_info(const char *machine)
{
    for (size_t i = 0; machine && i < ARRAY_SIZE(ios_devices); i++) {
        if (!strcmp(ios_devices[i].machine, machine)) {
            return &ios_devices[i];
        }
    }
    return NULL;
}

const QemuIosDeviceInfo *qemu_ios_device_info_at(int index)
{
    return index >= 0 && index < (int)ARRAY_SIZE(ios_devices) ? &ios_devices[index] : NULL;
}

void qemu_ios_ui_attach(qemu_ios_frame_cb cb, void *opaque)
{
    ios_init_frame_lock();
    ios.cb = cb;
    ios.cb_opaque = opaque;
}

/*
 * Hand back a pointer to the newest frame, without copying it.
 *
 * `serial` is in-out: pass what you last saw and this returns false, touching
 * nothing, if that is still the newest. That check matters more than it looks
 * -- the guest paints at 30-60 Hz while an iPhone display link fires at up to
 * 120, so most wake-ups have no new frame at all and used to spend a 600 KB
 * allocation and copy discovering it.
 *
 * The pointer stays readable for the next few frames (see IOS_FRAME_BUFFERS).
 * Do not free it and do not hold it indefinitely.
 */
bool qemu_ios_ui_frame(const void **pixels, int *width, int *height,
                       uint64_t *serial)
{
    ios_init_frame_lock();
    qemu_mutex_lock(&ios.frame_lock);

    if (ios.published < 0 || ios.serial == *serial) {
        qemu_mutex_unlock(&ios.frame_lock);
        return false;
    }

    *pixels = ios.buf[ios.published];
    *width = ios.width;
    *height = ios.height;
    *serial = ios.serial;

    qemu_mutex_unlock(&ios.frame_lock);
    return true;
}

void qemu_ios_ui_frame_size(int *width, int *height)
{
    ios_init_frame_lock();
    qemu_mutex_lock(&ios.frame_lock);
    *width = ios.width;
    *height = ios.height;
    qemu_mutex_unlock(&ios.frame_lock);
}

struct ios_touch {
    double nx, ny;             /* normalized 0..1 over the panel */
    bool down;
};

/*
 * Deliberately the LEGACY absolute-pointer path, not QEMU's multi-touch
 * events, even though the panel model understands both.
 *
 * console_handle_touch_event() -- the obvious API -- queues the BEGIN commit
 * BEFORE the DATA events carrying the coordinates, and the digitizer ignores a
 * commit for a slot whose position it has never seen. The press is therefore
 * dropped and only the release survives, which reads exactly like "taps do
 * nothing". The abs+button sequence below is the one the existing tooling
 * (qmp-touch.py) has always used.
 *
 * The cost is one finger at a time: pinch and rotate need the multi-touch path
 * with DATA queued before the commit.
 */
static void ios_touch_bh(void *opaque)
{
    struct ios_touch *t = opaque;

    ios_sequence_cancel_current();
    if (!runstate_is_running() && !runstate_check(RUN_STATE_SUSPENDED)) {
        /* Release an already delivered contact without advancing paused input. */
        if (!t->down && ios_manual_touch && ios.con) {
            InputBtnEvent button = {.button = INPUT_BUTTON_LEFT, .down = false};
            InputEvent event = {.type = INPUT_EVENT_KIND_BTN,
                                .u.btn.data = &button};
            qemu_input_event_send_impl(ios.con, &event);
            qemu_input_event_sync_impl();
        }
        if (!t->down) {
            ios_manual_touch = false;
        }
        g_free(t);
        return;
    }
    ios_manual_touch = t->down;
    if (ios.con) {
        qemu_input_queue_abs(ios.con, INPUT_AXIS_X,
                             (int)(t->nx * INPUT_EVENT_ABS_MAX), 0,
                             INPUT_EVENT_ABS_MAX);
        qemu_input_queue_abs(ios.con, INPUT_AXIS_Y,
                             (int)(t->ny * INPUT_EVENT_ABS_MAX), 0,
                             INPUT_EVENT_ABS_MAX);
        qemu_input_queue_btn(ios.con, INPUT_BUTTON_LEFT, t->down);
        qemu_input_event_sync();
    }
    g_free(t);
}

void qemu_ios_ui_touch(int slot, int phase, double nx, double ny)
{
    struct ios_touch *t;

    if (slot != 0 || phase < QEMU_IOS_TOUCH_BEGIN || phase > QEMU_IOS_TOUCH_END ||
        !isfinite(nx) || !isfinite(ny) || nx < 0 || nx > 1 || ny < 0 || ny > 1 ||
        !qemu_ios_ui_ready()) {
        return;                 /* one finger; no AIO context before attach */
    }

    t = g_new0(struct ios_touch, 1);
    t->nx = nx;
    t->ny = ny;
    t->down = (phase != QEMU_IOS_TOUCH_END);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_touch_bh, t);
}

/* --- hardware buttons -------------------------------------------------- */

struct ios_button {
    int button;
    bool down;
};

static struct ios_button_hold {
    QEMUTimer *release;
    int64_t pressed_at;
    IPodTouchButton button;
    bool down;
} ios_button_holds[4];

static void ios_press(IPodTouchButton button, bool down)
{
    /* Each is a no-op unless its machine is the one running. */
    ipod_touch_press_button(button, down);
    ipad1_press_button(button, down);
    ipod_touch_1g_press_button(button, down);
    s5l8920_press_button(button, down);
}

static void ios_button_release(void *opaque)
{
    struct ios_button_hold *hold = opaque;
    ios_press(hold->button, false);
    hold->down = false;
}

static void ios_button_bh(void *opaque)
{
    struct ios_button *b = opaque;
    IPodTouchButton button;
    switch (b->button) {
    case QEMU_IOS_BUTTON_HOME:
        button = IPOD_TOUCH_BUTTON_HOME;
        break;
    case QEMU_IOS_BUTTON_POWER:
        button = IPOD_TOUCH_BUTTON_POWER;
        break;
    case QEMU_IOS_BUTTON_VOLUME_UP:
        button = IPOD_TOUCH_BUTTON_VOLUP;
        break;
    case QEMU_IOS_BUTTON_VOLUME_DOWN:
        button = IPOD_TOUCH_BUTTON_VOLDOWN;
        break;
    default:
        g_free(b);
        return;
    }
    ios_sequence_cancel_current();
    struct ios_button_hold *hold = &ios_button_holds[b->button];
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (!hold->release) {
        hold->button = button;
        hold->release = timer_new_ms(QEMU_CLOCK_VIRTUAL, ios_button_release, hold);
    }
    if (b->down) {
        timer_del(hold->release);
        hold->pressed_at = now;
        hold->down = true;
        ios_press(button, true);
    } else {
        /* Both host events can arrive in one BH batch under load. Give the
         * guest's debounce handler time to observe the pressed pin. */
        timer_mod(hold->release, MAX(now, hold->pressed_at + 100));
    }
    g_free(b);
}

void qemu_ios_ui_button(int button, bool down)
{
    struct ios_button *b;

    if (button < QEMU_IOS_BUTTON_HOME || button > QEMU_IOS_BUTTON_VOLUME_DOWN ||
        !qemu_ios_ui_ready()) {
        return;
    }
    b = g_new0(struct ios_button, 1);
    b->button = button;
    b->down = down;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_button_bh, b);
}

/* --- generic host input sequence ---------------------------------------- */
/* All event application and ownership are under the BQL. Publication is atomic;
 * the host only observes the most recently submitted sequence ID/status. */
static IosInputSequence ios_sequence;
static QEMUTimer *ios_sequence_timer;
static uint64_t ios_sequence_id;
static int ios_sequence_status;
static QemuSeqLock ios_sequence_publication;

/* Single BQL writer; host readers observe one coherent ID/status pair. */
static void ios_sequence_publish(uint64_t id, int status)
{
    seqlock_write_begin(&ios_sequence_publication);
    qatomic_set(&ios_sequence_id, id);
    qatomic_set(&ios_sequence_status, status);
    seqlock_write_end(&ios_sequence_publication);
}

static void ios_sequence_emit(void *opaque, const IosInputEvent *e)
{
    if (e->kind == 0) {
        ios_press((IPodTouchButton)e->value, !!e->phase);

    } else if (ios.con) {
        qemu_input_queue_abs(ios.con, INPUT_AXIS_X,
            (int)(e->x * INPUT_EVENT_ABS_MAX), 0, INPUT_EVENT_ABS_MAX);
        qemu_input_queue_abs(ios.con, INPUT_AXIS_Y,
            (int)(e->y * INPUT_EVENT_ABS_MAX), 0, INPUT_EVENT_ABS_MAX);
        qemu_input_queue_btn(ios.con, INPUT_BUTTON_LEFT, e->phase != 2);
        qemu_input_event_sync();
    }
}

/* qemu_input_event_send() deliberately drops ordinary events while paused.
 * Cancellation is ownership cleanup, so its release must reach the handler
 * under BQL even when virtual time is stopped; it must not leave a finger
 * held forever after resume. No coordinate/button-down is synthesized here. */
static void ios_sequence_release_emit(void *opaque, const IosInputEvent *e)
{
    if (e->kind == 1 && ios.con) {
        InputBtnEvent button = {.button = INPUT_BUTTON_LEFT, .down = false};
        InputEvent event = {.type = INPUT_EVENT_KIND_BTN, .u.btn.data = &button};
        qemu_input_event_send_impl(ios.con, &event);
        qemu_input_event_sync_impl();
    } else {
        ios_sequence_emit(opaque, e);
    }
}

static void ios_sequence_cancel_current(void)
{
    if (ios_sequence_timer) {
        timer_del(ios_sequence_timer);
    }
    ios_input_cancel(&ios_sequence, ios_sequence_release_emit, NULL);
    if (qatomic_read(&ios_sequence_status) == QEMU_IOS_INPUT_RUNNING) {
        ios_sequence_publish(qatomic_read(&ios_sequence_id), QEMU_IOS_INPUT_CANCELLED);
    }
}

void qemu_ios_ui_manual_touch2(bool down)
{
    ios_sequence_cancel_current();
    ios_manual_touch2 = down;
}

void qemu_ios_ui_cancel_input(void)
{
    ios_sequence_cancel_current();
}

static void ios_sequence_tick(void *opaque)
{
    if (ios_input_step(&ios_sequence, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                       ios_sequence_emit, NULL)) {
        ios_sequence_publish(qatomic_read(&ios_sequence_id), QEMU_IOS_INPUT_DONE);
    } else {
        timer_mod(ios_sequence_timer, ios_sequence.origin +
                  ios_sequence.events[ios_sequence.next].at_ms);
    }
}

typedef struct {
    uint64_t id;
    size_t count;
    IosInputEvent events[IOS_INPUT_MAX_EVENTS];
} IosSequenceRequest;

static void ios_sequence_submit_bh(void *opaque)
{
    IosSequenceRequest *r = opaque;
    ios_sequence_cancel_current();
    ios_sequence_publish(0, QEMU_IOS_INPUT_REJECTED);
    bool manual = ios_manual_touch || ios_manual_touch2;
    for (int i = 0; i < 4; ++i) {
        manual |= ios_button_holds[i].down;
    }
    if (!manual && ios.con && qemu_ios_ui_ready()) {
        memcpy(ios_sequence.events, r->events, r->count * sizeof(r->events[0]));
        ios_sequence.count = r->count;
        ios_sequence.next = 0;
        ios_sequence.origin = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        if (!ios_sequence_timer) {
            ios_sequence_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                               ios_sequence_tick, NULL);
        }
        ios_sequence_publish(0, QEMU_IOS_INPUT_RUNNING);
        /* Even offset zero is scheduled on guest time: a paused guest does
         * not acquire a button until its virtual clock resumes. */
        timer_mod(ios_sequence_timer, ios_sequence.origin + r->events[0].at_ms);
    }
    ios_sequence_publish(r->id, qatomic_read(&ios_sequence_status));
    g_free(r);
}

bool qemu_ios_ui_input_sequence(uint64_t id, size_t count,
    const int64_t *at_ms, const int32_t *kind, const int32_t *value,
    const int32_t *phase, const double *x, const double *y)
{
    if (!id || !qemu_ios_ui_ready() || !count || count > IOS_INPUT_MAX_EVENTS ||
        !at_ms || !kind || !value || !phase || !x || !y) {
        return false;
    }
    IosSequenceRequest *r = g_new0(IosSequenceRequest, 1);
    r->id = id;
    r->count = count;
    for (size_t i = 0; i < count; ++i) {
        if (kind[i] != 0 && kind[i] != 1) { g_free(r); return false; }
        r->events[i] = (IosInputEvent){at_ms[i], kind[i], value[i], phase[i],
                                     x[i], y[i]};
    }
    if (!ios_input_valid(r->events, count)) {
        g_free(r);
        return false;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_sequence_submit_bh, r);
    return true;
}

int qemu_ios_ui_input_sequence_status(uint64_t id)
{
    unsigned sequence;
    uint64_t published;
    int status;
    do {
        sequence = seqlock_read_begin(&ios_sequence_publication);
        published = qatomic_read(&ios_sequence_id);
        status = qatomic_read(&ios_sequence_status);
    } while (seqlock_read_retry(&ios_sequence_publication, sequence));
    return id && published == id ? status : QEMU_IOS_INPUT_UNKNOWN;
}

static void ios_sequence_cancel_bh(void *opaque)
{
    uint64_t *id = opaque;
    if (*id == qatomic_read(&ios_sequence_id)) {
        ios_sequence_cancel_current();
    }
    g_free(id);
}

void qemu_ios_ui_input_sequence_cancel(uint64_t id)
{
    if (id && qemu_ios_ui_ready()) {
        uint64_t *copy = g_new(uint64_t, 1);
        *copy = id;
        aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_sequence_cancel_bh, copy);
    }
}

/* --- snapshots ---------------------------------------------------------- */

/*
 * Save the whole machine to a file, so the next launch can resume instead of
 * spending half a minute booting.
 *
 * This machine has no block device, so the classic savevm/loadvm path is
 * unavailable -- the migration stream to a `file:` URI is the equivalent, and
 * restoring is just `-incoming file:...` on the next run (plus the autostart
 * fix in qemu-ios-entry.c, or the machine comes back paused).
 *
 * The guest is stopped first: a snapshot taken while the vCPU is running would
 * capture RAM that disagrees with itself.
 *
 * The status distinguishes "not started", "in flight", "done" and "failed": it
 * is set RUNNING on the app thread before the BH so the pre-BH window reads
 * RUNNING, then resolved by reading the migration state.
 */
static int ios_snap_status = QEMU_IOS_SNAPSHOT_IDLE;
static char ios_snap_err[256];
/*
 * Whether THIS save's bottom half has run and its qmp_migrate was accepted.
 *
 * Without it, status() resolved RUNNING by reading migrate_get_current()->state
 * -- a global only reset to SETUP inside qmp_migrate, i.e. inside the BH. After
 * one successful save it stays COMPLETED forever, so the SECOND save in a
 * process read COMPLETED before its BH had run and reported DONE instantly. The
 * app then ran its atomic promote: deleted the good snapshot and renamed a .tmp
 * that did not exist yet -- silent total loss of the saved state, reported as
 * success. Gate the migration-state read on the BH having actually started.
 */
static int ios_snap_bh_ran;

static void ios_snapshot2_bh(void *opaque)
{
    ios_sequence_cancel_current();
    char *path = opaque;
    Error *err = NULL;
    g_autofree char *uri = g_strdup_printf("file:%s", path);

    if (qemu_ios_ui_storage_failed()) {
        snprintf(ios_snap_err, sizeof(ios_snap_err), "NAND storage write failed");
        qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_FAILED);
        g_free(path);
        return;
    }

    qmp_stop(&err);
    if (err) {
        snprintf(ios_snap_err, sizeof(ios_snap_err), "stop: %s", error_get_pretty(err));
        error_free(err);
        qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_FAILED);
        g_free(path);
        return;
    }
    /*
     * Do NOT migrate the global-state section. It would record the runstate we
     * just stopped into ("paused"), and on restore process_incoming_migration_bh
     * sees a non-live target runstate and parks the machine paused forever --
     * autostart (qemu-ios-entry.c) is bypassed and the guest comes back frozen:
     * no vCPU, no input, no orientation. With the section omitted the
     * destination assumes RUNNING (migration_get_target_runstate) and autostart
     * resumes the vCPU the instant the incoming stream is consumed. We never use
     * suspend, so nothing else in global-state is needed.
     */
    migrate_get_current()->store_global_state = false;
    qmp_migrate(uri, false, NULL, false, false, &err);
    if (err) {
        snprintf(ios_snap_err, sizeof(ios_snap_err), "save: %s", error_get_pretty(err));
        error_free(err);
        qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_FAILED);
    } else {
        /* Migration accepted: only NOW may status() trust the migration state. */
        qatomic_set(&ios_snap_bh_ran, 1);
        fprintf(stderr, "[snapshot] writing %s\n", path);
    }
    g_free(path);
}

void qemu_ios_snapshot_save2(const char *path)
{
    if (!qemu_ios_ui_ready()) {
        snprintf(ios_snap_err, sizeof(ios_snap_err), "device not attached");
        qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_FAILED);
        return;
    }
    ios_snap_err[0] = '\0';
    qatomic_set(&ios_snap_bh_ran, 0);
    /* Set RUNNING here, on the app thread, so a status() before the BH runs
     * does not read a leftover DONE/IDLE. */
    qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_RUNNING);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_snapshot2_bh, g_strdup(path));
}

QemuIosSnapshotStatus qemu_ios_snapshot_status(char *errbuf, unsigned long errlen)
{
    int s = qatomic_read(&ios_snap_status);
    if (s == QEMU_IOS_SNAPSHOT_RUNNING && qatomic_read(&ios_snap_bh_ran)) {
        /* Resolve against the migration state -- but only once this save's BH
         * has actually started a migration, or we would read the PREVIOUS
         * save's leftover COMPLETED. Reading the enum field is a plain int
         * load; good enough for a status poll. */
        MigrationState *ms = migrate_get_current();
        int st = ms ? (int)ms->state : 0;
        if (st == MIGRATION_STATUS_COMPLETED) {
            s = QEMU_IOS_SNAPSHOT_DONE;
            qatomic_set(&ios_snap_status, s);
        } else if (st == MIGRATION_STATUS_FAILED || st == MIGRATION_STATUS_CANCELLED) {
            snprintf(ios_snap_err, sizeof(ios_snap_err), "migration did not complete (state %d)", st);
            s = QEMU_IOS_SNAPSHOT_FAILED;
            qatomic_set(&ios_snap_status, s);
        }
    }
    if (errbuf && errlen) {
        strncpy(errbuf, ios_snap_err, errlen - 1);
        errbuf[errlen - 1] = '\0';
    }
    return s;
}

static void ios_snapshot_resume_bh(void *opaque)
{
    if (!qemu_ios_ui_storage_failed() && !runstate_is_running()) {
        vm_start();
    }
}

void qemu_ios_snapshot_resume(void)
{
    if (!qemu_ios_ui_ready()) {
        return;   /* NULL AioContext before qemu_init(); the only entry point
                   * here that was missing this check. */
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_snapshot_resume_bh, NULL);
}

/* --- foreground/background ---------------------------------------------- */

/*
 * iOS terminates an app that touches GL while it is not foreground, and the
 * emulated CPU has no notion of app lifecycle -- it keeps executing guest code,
 * including whatever GL the guest is in the middle of. The app tells us when
 * that is no longer safe.
 *
 * Deliberately NOT scheduled through a bottom half: by the time a BH runs, the
 * app may already have been backgrounded and the first illegal GL call made.
 * The flag is a plain bool read on the vCPU thread; a stale read costs one
 * rejected call, whereas being late costs the process.
 */
static void ios_resume_bh(void *opaque)
{
    /*
     * Saving a snapshot stops the machine and leaves it stopped -- migration
     * refuses to run from a live VM, and refuses a SECOND save from one it
     * already paused ("Can't migrate the vm that was paused due to previous
     * migration"). So coming back to the foreground has to restart it, or the
     * guest is frozen from the first time the app was backgrounded onwards.
     */
    if (!qemu_ios_ui_storage_failed() && !runstate_is_running()) {
        vm_start();
    }
}

void qemu_ios_set_foreground(bool foreground)
{
    gles_host_set_allowed(foreground);

    if (foreground && qemu_ios_ui_ready()) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_resume_bh, NULL);
    }
}
