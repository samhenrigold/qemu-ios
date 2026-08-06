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
#include "block/aio.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "qapi/error.h"
#include "hw/arm/ipod_touch_buttons.h"

void gles_host_set_allowed(bool allowed);
uint64_t ipod_touch_fmss_icon_state_writes(void);

#include "qemu-ios-ui.h"

#include <sys/resource.h>

#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-commands-misc.h"
#include "migration/misc.h"
#include "migration/migration.h"      /* migrate_get_current, MigrationState.state */
#include "system/runstate.h"

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

void qemu_ios_ui_vm_stopped(void)
{
    qatomic_set(&ios_vm_alive, 0);
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
    for (i = 0; i < INPUT_EVENT_SLOTS_MAX; i++) {
        ios.slots[i].tracking_id = -1;
    }
    ios.dcl.ops = &ios_dcl_ops;
    ios.dcl.con = ios.con;
    register_displaychangelistener(&ios.dcl);
    ios.attached = true;
    qatomic_set(&ios_vm_alive, 1);
}

/* --- app thread -------------------------------------------------------- */

void qemu_ios_ui_attach(qemu_ios_frame_cb cb, void *opaque)
{
    static bool inited;

    /*
     * Called before the VM thread exists, so no lock is needed here and the
     * frame lock is ready before anything can take it.
     */
    if (!inited) {
        qemu_mutex_init(&ios.frame_lock);
        inited = true;
    }
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
    qemu_mutex_lock(&ios.frame_lock);
    *width = ios.width;
    *height = ios.height;
    qemu_mutex_unlock(&ios.frame_lock);
}

struct ios_touch {
    double nx, ny;             /* normalised 0..1 over the panel */
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

    if (slot != 0 || !ios.attached) {
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

static void ios_button_bh(void *opaque)
{
    struct ios_button *b = opaque;

    switch (b->button) {
    case QEMU_IOS_BUTTON_HOME:
        ipod_touch_press_button(IPOD_TOUCH_BUTTON_HOME, b->down);
        break;
    case QEMU_IOS_BUTTON_POWER:
        ipod_touch_press_button(IPOD_TOUCH_BUTTON_POWER, b->down);
        break;
    case QEMU_IOS_BUTTON_VOLUME_UP:
        ipod_touch_press_button(IPOD_TOUCH_BUTTON_VOLUP, b->down);
        break;
    case QEMU_IOS_BUTTON_VOLUME_DOWN:
        ipod_touch_press_button(IPOD_TOUCH_BUTTON_VOLDOWN, b->down);
        break;
    default:
        break;
    }
    g_free(b);
}

void qemu_ios_ui_button(int button, bool down)
{
    struct ios_button *b;

    if (!ios.attached) {
        return;
    }
    b = g_new0(struct ios_button, 1);
    b->button = button;
    b->down = down;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_button_bh, b);
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
 */
static void ios_snapshot_bh(void *opaque)
{
    char *path = opaque;
    Error *err = NULL;
    g_autofree char *uri = g_strdup_printf("file:%s", path);

    qmp_stop(&err);
    if (err) {
        fprintf(stderr, "[snapshot] stop failed: %s\n", error_get_pretty(err));
        error_free(err);
        g_free(path);
        return;
    }

    /* Omit global-state so the restore autostarts running, not frozen-paused.
     * See ios_snapshot2_bh for the full reasoning. */
    migrate_get_current()->store_global_state = false;
    qmp_migrate(uri, false, NULL, false, false, false, false, &err);
    if (err) {
        fprintf(stderr, "[snapshot] save failed: %s\n", error_get_pretty(err));
        error_free(err);
    } else {
        fprintf(stderr, "[snapshot] writing %s\n", path);
    }
    g_free(path);
}

void qemu_ios_snapshot_save(const char *path)
{
    if (!ios.attached) {
        return;
    }
    /* Runs on the QEMU thread under the BQL, like every other command here. */
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_snapshot_bh,
                            g_strdup(path));
}

/*
 * Whether the save has finished. The app has only a few seconds of background
 * time, so it needs to know when the file is complete rather than guessing.
 */
bool qemu_ios_snapshot_done(void)
{
    return !migration_is_running();
}

/*
 * Tracked variant. The plain _done() above is true both before the bottom half
 * has run (nothing is migrating yet) and after a failed migrate (it stopped
 * running) -- so the app could not tell "not started" from "done" from
 * "failed". This tracks an explicit status, set RUNNING on the app thread
 * before the BH so the pre-BH window reads RUNNING, then resolved by reading
 * the migration state.
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
    char *path = opaque;
    Error *err = NULL;
    g_autofree char *uri = g_strdup_printf("file:%s", path);

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
    qmp_migrate(uri, false, NULL, false, false, false, false, &err);
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
    if (!ios.attached) {
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
    if (!runstate_is_running()) {
        vm_start();
    }
}

void qemu_ios_snapshot_resume(void)
{
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
    if (!runstate_is_running()) {
        vm_start();
    }
}

/* --- home-screen layout -------------------------------------------------- */

/*
 * A pass-through, not a copy of the state: the counter lives in the NAND device
 * because that is where it is incremented, and the dependency can only point
 * this way -- hw/arm/ipod_touch_fmss.c is linked into every build of the
 * emulator, while this file is linked only into the app dylib. A device calling
 * into here would break plain qemu-system-arm.
 *
 * No lock and no bottom half: it is one atomic load of a value the vCPU thread
 * only ever increments, which is exactly what an app polling on its own tick
 * wants.
 */
uint64_t qemu_ios_ui_icon_state_generation(void)
{
    return ipod_touch_fmss_icon_state_writes();
}

void qemu_ios_set_foreground(bool foreground)
{
    gles_host_set_allowed(foreground);

    if (foreground && ios.attached) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_resume_bh, NULL);
    }
}
