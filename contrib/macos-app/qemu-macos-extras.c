/*
 * macOS-app additions to contrib/ios-app/qemu-ios-ui.c, which is reused
 * unchanged. Every entry point here follows that file's one rule: the app
 * thread allocates a small struct and a bottom half runs it on the QEMU
 * thread under the BQL.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/audio.h"
#include "qemu/audio-capture.h"
#include "system/runstate.h"
#include <pthread.h>
#include "qemu-ios-ui.h"
#include "qemu/main-loop.h"
#include "qemu/aio.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/core/boards.h"
#include "hw/arm/ipod-attitude.h"
#include "qapi/qapi-commands-qom.h"
#include "qobject/qnum.h"
#include "qemu/thread.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"

#include "qemu-main.h"

#include <dlfcn.h>
#include <mach-o/loader.h>

#include "qemu-macos-extras.h"

/*
 * system/main.c (replaced by qemu-ios-entry.c) defined this; ui/cocoa.m still
 * references it. It stays NULL under -display none, and qemu-ios-entry.c owns
 * the main loop regardless.
 */
int (*qemu_main)(void);

static QemuConsole *con0(void)
{
    return qemu_console_lookup_by_index(0);
}

/* --- second finger (multi-touch path) ----------------------------------- */

struct mtt_touch {
    int phase;
    double nx, ny;
};

static void mtt_bh(void *opaque)
{
    struct mtt_touch *t = opaque;
    QemuConsole *con = con0();
    static bool tracked;

    if (!runstate_is_running() && !runstate_check(RUN_STATE_SUSPENDED)) {
        if (t->phase == QEMU_IOS_TOUCH_END && tracked && con) {
            InputMultiTouchEvent mtt = {
                .type = INPUT_MULTI_TOUCH_TYPE_END, .slot = 1, .tracking_id = 1,
            };
            InputEvent event = {.type = INPUT_EVENT_KIND_MTT, .u.mtt.data = &mtt};
            qemu_ios_ui_manual_touch2(false);
            qemu_input_event_send_impl(con, &event);
            qemu_input_event_sync_impl();
            tracked = false;
        }
        g_free(t);
        return;
    }
    qemu_ios_ui_manual_touch2(t->phase != QEMU_IOS_TOUCH_END);
    if (con) {
        InputMultiTouchType type;
        if (t->phase == QEMU_IOS_TOUCH_END) {
            type = INPUT_MULTI_TOUCH_TYPE_END;
            tracked = false;
        } else if (!tracked) {
            type = INPUT_MULTI_TOUCH_TYPE_BEGIN;
            tracked = true;
        } else {
            type = INPUT_MULTI_TOUCH_TYPE_UPDATE;
        }
        /* DATA before the commit, or the digitizer drops the press. */
        qemu_input_queue_mtt_abs(con, INPUT_AXIS_X,
                                 (int)(t->nx * INPUT_EVENT_ABS_MAX),
                                 0, INPUT_EVENT_ABS_MAX, 1, 1);
        qemu_input_queue_mtt_abs(con, INPUT_AXIS_Y,
                                 (int)(t->ny * INPUT_EVENT_ABS_MAX),
                                 0, INPUT_EVENT_ABS_MAX, 1, 1);
        qemu_input_queue_mtt(con, type, 1, 1);
        qemu_input_event_sync();
    }
    g_free(t);
}

void qemu_ios_ui_touch2(int phase, double nx, double ny)
{
    if (phase < QEMU_IOS_TOUCH_BEGIN || phase > QEMU_IOS_TOUCH_END ||
        !isfinite(nx) || !isfinite(ny) || nx < 0 || nx > 1 || ny < 0 || ny > 1 ||
        !qemu_ios_ui_ready()) {
        return;
    }
    struct mtt_touch *t = g_new0(struct mtt_touch, 1);
    t->phase = phase;
    t->nx = nx;
    t->ny = ny;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), mtt_bh, t);
}

/* --- keyboard ------------------------------------------------------------ */

struct key_event {
    int qcode;
    bool down;
};

static void key_bh(void *opaque)
{
    struct key_event *k = opaque;
    QemuConsole *con = con0();
    qemu_ios_ui_cancel_input();

    if (con) {
        qemu_input_event_send_key_qcode(con, k->qcode, k->down);
    }
    g_free(k);
}

static void qemu_ios_ui_key(int qcode, bool down)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    struct key_event *k = g_new0(struct key_event, 1);
    k->qcode = qcode;
    k->down = down;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), key_bh, k);
}

void qemu_ios_ui_key_mac(int mac_keycode, bool down)
{
    int qcode;

    if (mac_keycode < 0 ||
        (unsigned)mac_keycode >= qemu_input_map_osx_to_qcode_len) {
        return;
    }
    qcode = qemu_input_map_osx_to_qcode[mac_keycode];
    switch (qcode) {
    case Q_KEY_CODE_UNMAPPED:
    /* Command and Control belong to the menu bar. */
    case Q_KEY_CODE_META_L:
    case Q_KEY_CODE_META_R:
    case Q_KEY_CODE_CTRL:
    case Q_KEY_CODE_CTRL_R:
        return;
    }
    qemu_ios_ui_key(qcode, down);
}

/*
 * Rotate is keyboard-only in the guest (Meta+Left / Meta+Right), edge-triggered
 * on the press, so the chord is sent and released in one shot -- no hardware
 * GPIO for it, unlike Home/Lock/Volume.
 */
static void rotate_bh(void *opaque)
{
    bool clockwise = (bool)(intptr_t)opaque;
    QemuConsole *con = con0();
    int arrow = clockwise ? Q_KEY_CODE_RIGHT : Q_KEY_CODE_LEFT;
    Object *machine = OBJECT(qdev_get_machine());

    /*
     * The iPad has no rotate chord (and host keys may belong to its USB
     * keyboard): step the accelerometer's UIDeviceOrientation instead.
     * Turning the device clockwise from portrait puts Home on the left
     * (LandscapeRight, 4), then upside down (2), then Home right (3).
     */
    if (object_dynamic_cast(machine, MACHINE_TYPE_NAME("ipad1"))) {
        static const int cw[] = { [1] = 4, [4] = 2, [2] = 3, [3] = 1 };
        static const int ccw[] = { [1] = 3, [3] = 2, [2] = 4, [4] = 1 };
        int64_t o = object_property_get_int(machine, "accel-orientation", NULL);

        if (o < 1 || o > 4) {
            o = 1;
        }
        object_property_set_int(machine, "accel-orientation",
                                clockwise ? cw[o] : ccw[o], NULL);
        return;
    }
    if (con) {
        qemu_input_event_send_key_qcode(con, Q_KEY_CODE_META_L, true);
        qemu_input_event_send_key_qcode(con, arrow, true);
        qemu_input_event_send_key_qcode(con, arrow, false);
        qemu_input_event_send_key_qcode(con, Q_KEY_CODE_META_L, false);
    }
}

void qemu_ios_ui_rotate(bool clockwise)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), rotate_bh,
                            (void *)(intptr_t)clockwise);
}

/* --- machine properties -------------------------------------------------- */

static void shake_bh(void *opaque)
{
    Error *err = NULL;

    object_property_set_bool(OBJECT(qdev_get_machine()), "accel-shake", true,
                             &err);
    if (err) {
        fprintf(stderr, "[shake] %s\n", error_get_pretty(err));
        error_free(err);
    }
}

void qemu_ios_ui_shake(void)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), shake_bh, NULL);
}

struct attitude_input { double pitch, roll; int pose; };

static void attitude_bh(void *opaque)
{
    struct attitude_input *input = opaque;
    Error *err = NULL;
    Object *machine = OBJECT(qdev_get_machine());
    object_property_set_str(machine, "accel-pose", input->pose ? "flat" : "upright", &err);
    const char *names[] = { "accel-pitch", "accel-roll" };
    double angles[] = { input->pitch, input->roll };
    for (unsigned i = 0; i < 2 && !err; i++) {
        QNum *value = qnum_from_double(angles[i]);
        qmp_qom_set("/machine", names[i], QOBJECT(value), &err);
        qobject_unref(value);
    }
    if (err) {
        fprintf(stderr, "[attitude] %s\n", error_get_pretty(err));
        error_free(err);
    }
    g_free(input);
}

void qemu_ios_ui_attitude(double pitch_deg, double roll_deg, int pose)
{
    int8_t vector[3];
    if (!qemu_ios_ui_ready() || (pose != 0 && pose != 1) ||
        !ipod_attitude_vector(pitch_deg, roll_deg, pose, vector)) return;
    struct attitude_input *input = g_new(struct attitude_input, 1);
    *input = (struct attitude_input){ pitch_deg, roll_deg, pose };
    aio_bh_schedule_oneshot(qemu_get_aio_context(), attitude_bh, input);
}

struct battery_input { int level, charging; double drain; };

static void battery_bh(void *opaque)
{
    struct battery_input *input = opaque;
    Object *machine = OBJECT(qdev_get_machine());
    Error *err = NULL;
    const char *modes[] = { "auto", "on", "off" };
    object_property_set_int(machine, "battery-level", input->level, &err);
    /* The iPad has none: its charging is the port's (usb-charger). */
    if (!err && object_property_find(machine, "battery-charging"))
        object_property_set_str(machine, "battery-charging", modes[input->charging], &err);
    if (!err) {
        QNum *value = qnum_from_double(input->drain);
        qmp_qom_set("/machine", "battery-drain", QOBJECT(value), &err);
        qobject_unref(value);
    }
    if (err) {
        fprintf(stderr, "[battery] %s\n", error_get_pretty(err));
        error_free(err);
    }
    g_free(input);
}

bool qemu_ios_ui_battery_config(int level, int charging, double drain)
{
    if (!qemu_ios_ui_ready() || level < 0 || level > 100 || charging < 0 || charging > 2 ||
        !isfinite(drain) || drain < 0 || drain > 100) return false;
    struct battery_input *input = g_new(struct battery_input, 1);
    *input = (struct battery_input){ level, charging, drain };
    aio_bh_schedule_oneshot(qemu_get_aio_context(), battery_bh, input);
    return true;
}

bool qemu_ios_ui_battery(int level, int charging)
{
    return qemu_ios_ui_battery_config(level, charging, 0);
}

static void usb_connection_bh(void *opaque)
{
    bool *attached = opaque;
    Error *err = NULL;
    object_property_set_bool(OBJECT(qdev_get_machine()), "usb-attached", *attached, &err);
    if (err) {
        fprintf(stderr, "[usb] %s\n", error_get_pretty(err));
        error_free(err);
    }
    g_free(attached);
}

bool qemu_ios_ui_usb_connection(bool attached)
{
    if (!qemu_ios_ui_ready()) return false;
    bool *value = g_new(bool, 1);
    *value = attached;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), usb_connection_bh, value);
    return true;
}

/* One machine property from its string form, set on the QEMU thread; false
 * if the machine has no such property (e.g. the iPod has no compass). */
struct machine_prop { const char *name; char *value; };

static void machine_prop_bh(void *opaque)
{
    struct machine_prop *m = opaque;
    Error *err = NULL;

    object_property_parse(OBJECT(qdev_get_machine()), m->name, m->value, &err);
    if (err) {
        fprintf(stderr, "[%s] %s\n", m->name, error_get_pretty(err));
        error_free(err);
    }
    g_free(m->value);
    g_free(m);
}

static bool set_machine_prop(const char *name, char *value)
{
    if (!qemu_ios_ui_ready() ||
        !object_property_find(OBJECT(qdev_get_machine()), name)) {
        g_free(value);
        return false;
    }
    struct machine_prop *m = g_new(struct machine_prop, 1);
    *m = (struct machine_prop){ name, value };
    aio_bh_schedule_oneshot(qemu_get_aio_context(), machine_prop_bh, m);
    return true;
}

bool qemu_ios_ui_compass(int heading_deg)
{
    return set_machine_prop("compass-heading",
                            g_strdup_printf("%d", ((heading_deg % 360) + 360) % 360));
}

bool qemu_ios_ui_orientation(int value)
{
    return set_machine_prop("accel-orientation", g_strdup_printf("%d", value));
}

bool qemu_ios_ui_usb_charger(bool high_power)
{
    return set_machine_prop("usb-charger", g_strdup(high_power ? "on" : "off"));
}

/* The modem: writes go through a BH; reads come from a snapshot a BH refreshes. */
static QemuMutex modem_lock;
static char *modem_snapshot;                     /* JSON, or NULL before the first refresh */
static char *modem_error;                        /* the last write's refusal, until the next write */
static bool modem_refresh_queued;

static Object *modem_object(void)
{
    return object_resolve_path_component(OBJECT(qdev_get_machine()), "baseband-modem");
}

static void __attribute__((constructor)) modem_lock_init(void)
{
    qemu_mutex_init(&modem_lock);
}

struct modem_write { char *name, *value; };

static void modem_write_bh(void *opaque)
{
    struct modem_write *w = opaque;
    Object *modem = modem_object();
    Error *err = NULL;

    if (modem) {
        object_property_parse(modem, w->name, w->value, &err);
    }
    qemu_mutex_lock(&modem_lock);
    g_free(modem_error);
    modem_error = err ? g_strdup(error_get_pretty(err)) : NULL;
    qemu_mutex_unlock(&modem_lock);
    error_free(err);
    g_free(w->name);
    g_free(w->value);
    g_free(w);
}

bool qemu_ios_ui_modem_set(const char *property, const char *value)
{
    Object *modem;

    if (!property || !value || !qemu_ios_ui_ready() || !(modem = modem_object()) ||
        !object_property_find(modem, property)) {
        return false;
    }
    struct modem_write *w = g_new(struct modem_write, 1);
    *w = (struct modem_write){ g_strdup(property), g_strdup(value) };
    aio_bh_schedule_oneshot(qemu_get_aio_context(), modem_write_bh, w);
    return true;
}

static void modem_refresh_bh(void *opaque)
{
    static const char *const strs[] = {
        "carrier", "mcc-mnc", "call-state", "last-dialed", "last-mo-sms",
    };
    static const char *const bools[] = { "registered", "sim-present", "emergency-call" };
    static const char *const ints[] = { "signal-dbm", "mo-sms-count" };
    Object *modem = modem_object();
    QDict *d = qdict_new();
    GString *json;

    for (int i = 0; modem && i < ARRAY_SIZE(strs); i++) {
        g_autofree char *v = object_property_get_str(modem, strs[i], NULL);
        qdict_put_str(d, strs[i], v ? v : "");
    }
    for (int i = 0; modem && i < ARRAY_SIZE(bools); i++) {
        qdict_put_bool(d, bools[i], object_property_get_bool(modem, bools[i], NULL));
    }
    for (int i = 0; modem && i < ARRAY_SIZE(ints); i++) {
        qdict_put_int(d, ints[i], object_property_get_int(modem, ints[i], NULL));
    }
    qemu_mutex_lock(&modem_lock);
    if (modem_error) {
        qdict_put_str(d, "error", modem_error);
    }
    json = qobject_to_json(QOBJECT(d));
    g_free(modem_snapshot);
    modem_snapshot = modem ? g_string_free(json, false) : (g_string_free(json, true), NULL);
    modem_refresh_queued = false;
    qemu_mutex_unlock(&modem_lock);
    qobject_unref(d);
}

char *qemu_ios_ui_modem_status(void)
{
    char *copy;

    if (!qemu_ios_ui_ready() || !modem_object()) {
        return NULL;
    }
    qemu_mutex_lock(&modem_lock);
    copy = g_strdup(modem_snapshot);
    if (!modem_refresh_queued) {
        modem_refresh_queued = true;
        aio_bh_schedule_oneshot(qemu_get_aio_context(), modem_refresh_bh, NULL);
    }
    qemu_mutex_unlock(&modem_lock);
    return copy;
}

void qemu_ios_ui_modem_free(char *status)
{
    g_free(status);
}

static void paste_bh(void *opaque)
{
    char *text = opaque;
    Error *err = NULL;

    object_property_set_str(OBJECT(qdev_get_machine()), "pasteboard", text,
                            &err);
    if (err) {
        fprintf(stderr, "[paste] %s\n", error_get_pretty(err));
        error_free(err);
    }
    g_free(text);
}

void qemu_ios_ui_paste(const char *utf8)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), paste_bh, g_strdup(utf8));
}

/* --- machine controls ---------------------------------------------------- */

typedef void (*qmp_void_fn)(Error **errp);

static void qmp_bh(void *opaque)
{
    qmp_void_fn fn = opaque;
    Error *err = NULL;

    if (qemu_ios_ui_storage_failed() && fn != qmp_quit) {
        fprintf(stderr, "[machine] NAND storage failed; relaunch after fixing storage\n");
        return;
    }
    if (fn == qmp_system_reset || fn == qmp_quit || fn == qmp_system_powerdown) {
        qemu_ios_ui_cancel_input();
    }
    fn(&err);
    if (err) {
        fprintf(stderr, "[machine] %s\n", error_get_pretty(err));
        error_free(err);
    }
}

static void schedule_qmp(qmp_void_fn fn)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), qmp_bh, (void *)fn);
}

void qemu_ios_ui_pause(void)     { schedule_qmp(qmp_stop); }
void qemu_ios_ui_resume(void)    { schedule_qmp(qmp_cont); }
void qemu_ios_ui_reset(void)     { schedule_qmp(qmp_system_reset); }
void qemu_ios_ui_powerdown(void) { schedule_qmp(qmp_system_powerdown); }
void qemu_ios_ui_quit(void)      { schedule_qmp(qmp_quit); }

/* --- hardware keyboard ------------------------------------------------------ */
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"

static BusState *keyboard_bus(void)
{
    return BUS(object_resolve_path_type("usb-bus.0", TYPE_USB_BUS, NULL));
}

static void hardware_keyboard_bh(void *opaque)
{
    bool attach = (uintptr_t)opaque;
    Object *kbd = object_resolve_path_type("", "usb-kbd", NULL);
    Error *err = NULL;

    if (!attach && kbd) {
        qdev_unplug(DEVICE(kbd), &err);
    } else if (attach && !kbd) {
        BusState *bus = keyboard_bus();
        DeviceState *dev = bus ? qdev_new("usb-kbd") : NULL;
        if (dev) {
            /* As the app's -device line: 4.x's dock port grants a 50 mA budget, so 100 mA is refused. */
            qdev_prop_set_uint32(dev, "max-power", 20);
            qdev_realize_and_unref(dev, bus, &err);
        }
    }
    if (err) {
        fprintf(stderr, "[keyboard] %s\n", error_get_pretty(err));
        error_free(err);
    }
}

bool qemu_ios_ui_hardware_keyboard(bool attached)
{
    if (!qemu_ios_ui_ready() || !keyboard_bus()) {
        return false;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), hardware_keyboard_bh, (void *)(uintptr_t)attached);
    return true;
}

/* --- network restrict flip ----------------------------------------------- */
#ifdef CONFIG_SLIRP
#include "net/slirp.h"

struct net_restrict_req { char *id; bool restricted; };

static void net_restrict_bh(void *opaque)
{
    struct net_restrict_req *r = opaque;
    if (net_slirp_set_restrict(r->id, r->restricted) < 0) {
        fprintf(stderr, "[net] no user netdev '%s' to set restrict\n",
                r->id ? r->id : "(default)");
    }
    g_free(r->id);
    g_free(r);
}

/* Flip a running user netdev's slirp restrict flag from the app, in place: no
 * link event, so the guest keeps its Wi-Fi association and DHCP lease. Used to
 * open networking after the Setup Assistant finishes (restrict=on through
 * Setup avoids 5.x's live-internet Apple-ID stall). */
void qemu_ios_ui_net_restrict(const char *id, bool restricted)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    struct net_restrict_req *r = g_new(struct net_restrict_req, 1);
    r->id = (id && *id) ? g_strdup(id) : NULL;
    r->restricted = restricted;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), net_restrict_bh, r);
}

static void net_lan_bh(void *opaque)
{
    struct net_restrict_req *r = opaque;
    if (net_slirp_set_lan(r->id, r->restricted) < 0) {
        fprintf(stderr, "[net] no user netdev '%s' to set lan\n",
                r->id ? r->id : "(default)");
    }
    g_free(r->id);
    g_free(r);
}

/* The app's "Attach to local network" toggle, applied in place. */
void qemu_ios_ui_net_lan(const char *id, bool allowed)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    struct net_restrict_req *r = g_new(struct net_restrict_req, 1);
    r->id = (id && *id) ? g_strdup(id) : NULL;
    r->restricted = allowed;   /* here: allowed */
    aio_bh_schedule_oneshot(qemu_get_aio_context(), net_lan_bh, r);
}
#else
void qemu_ios_ui_net_restrict(const char *id, bool restricted)
{
    (void)id; (void)restricted;
}

void qemu_ios_ui_net_lan(const char *id, bool allowed)
{
    (void)id; (void)allowed;
}
#endif

/* Agent operations use a separate mutex and an acquired lifetime reference;
 * they never touch CPU/device state or hold the BQL. */
#include "hw/arm/ipod-agent.h"

bool qemu_ios_agent_request(const char *request)
{
    if (!request || !qemu_ios_ui_ready()) {
        return false;
    }
    IPodAgent *a = ipod_agent_acquire();
    bool accepted = a && !ipod_agent_submit(a, request);
    ipod_agent_free(a);
    return accepted;
}

char *qemu_ios_agent_result(void)
{
    if (!qemu_ios_ui_ready()) {
        return NULL;
    }
    IPodAgent *a = ipod_agent_acquire();
    char *result = a ? ipod_agent_take_result(a) : NULL;
    ipod_agent_free(a);
    if (result && !*result) {
        g_free(result);
        return NULL;
    }
    return result;
}

void qemu_ios_agent_free_result(char *result)
{
    g_free(result);
}

int qemu_ios_agent_status(void)
{
    if (!qemu_ios_ui_ready()) {
        return 0;
    }
    IPodAgent *a = ipod_agent_acquire();
    const char *status = a ? ipod_agent_status(a, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)) : "absent";
    ipod_agent_free(a);
    return !strcmp(status, "alive") ? 1 : !strcmp(status, "stale") ? 2 : 0;
}

void qemu_ios_agent_cancel(const char *id)
{
    if (id && qemu_ios_ui_ready()) {
        IPodAgent *a = ipod_agent_acquire();
        if (a) {
            ipod_agent_cancel(a, id);
        }
        ipod_agent_free(a);
    }
}

/* A clean shutdown: the agent's halt (reboot2(RB_HALT), where SpringBoard's power-off slider ends), else
 * (1.x has no agent) the board's power-off gesture. */
int qemu_ios_ui_shutdown(void)
{
    if (!qemu_ios_ui_ready() || qemu_ios_ui_storage_failed()) {
        return 0;
    }
    if (qemu_ios_agent_status() == 1 && qemu_ios_agent_request("qemu-ios-shutdown halt\n")) {
        return 1;
    }
    qemu_ios_ui_powerdown();
    return 2;
}

/* Atomic renderer count; safe while the main thread presents the device. */
extern int gles_host_context_count(void);
int qemu_ios_gles_contexts(void)
{
    return qemu_ios_ui_ready() ? gles_host_context_count() : 0;
}

/* hw/arm/guest-package.c; its own lock, so no BQL or ready check needed. */
extern bool guest_pkg_last_report(int64_t *serial, int32_t *result);
extern int32_t guest_pkg_gles_protocol(int64_t *serial);
bool qemu_ios_guest_package_report(int64_t *serial, int32_t *result)
{
    int64_t s;
    int32_t r;
    bool have = guest_pkg_last_report(&s, &r);
    if (serial) {
        *serial = s;
    }
    if (result) {
        *result = r;
    }
    return have;
}

int32_t qemu_ios_gles_protocol(int64_t *serial)
{
    return guest_pkg_gles_protocol(serial);
}

uint32_t qemu_ios_api_version(void)
{
    return QEMU_IOS_API_VERSION;
}

/* Identify the loaded image, not an on-disk dylib a developer may replace. */
const char *qemu_ios_build_id(void)
{
    static char identity[33];
    static gsize ready;
    if (g_once_init_enter(&ready)) {
        Dl_info info;
        if (dladdr((void *)qemu_ios_build_id, &info) && info.dli_fbase) {
            const struct mach_header_64 *header = info.dli_fbase;
            if (header->magic == MH_MAGIC_64) {
                const uint8_t *cursor = (const uint8_t *)(header + 1);
                size_t remaining = header->sizeofcmds;
                for (unsigned i = 0; i < header->ncmds && remaining >= sizeof(struct load_command); i++) {
                    const struct load_command *command = (const void *)cursor;
                    if (command->cmdsize < sizeof(*command) || command->cmdsize > remaining) break;
                    if (command->cmd == LC_UUID && command->cmdsize >= sizeof(struct uuid_command)) {
                        const struct uuid_command *uuid = (const void *)command;
                        for (unsigned j = 0; j < 16; j++) snprintf(identity + j * 2, 3, "%02x", uuid->uuid[j]);
                        break;
                    }
                    cursor += command->cmdsize;
                    remaining -= command->cmdsize;
                }
            }
        }
        g_once_init_leave(&ready, 1);
    }
    return identity[0] ? identity : NULL;
}

/* Guest output capture uses QEMU's mixer; it does not capture the host microphone. */
#define RECORDING_AUDIO_BYTES 16384
#define RECORDING_AUDIO_PACKETS 128
static pthread_mutex_t recording_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t generation;
    bool active, failed, anchored;
    int64_t origin_us;
    double next_seconds;
    unsigned head, count;
    struct { int size; double seconds; uint8_t bytes[RECORDING_AUDIO_BYTES]; }
        packets[RECORDING_AUDIO_PACKETS];
} recording_audio;
/* Accessed only on the emulator thread, including audio cleanup. */
static CaptureVoiceOut *recording_audio_voice;
static AudioBackend *recording_audio_be;     /* the backend recording_audio_voice is on */
static void *recording_audio_context;
static VMChangeStateEntry *recording_audio_vm_change;

static void recording_audio_notify(void *opaque, audcnotification_e event)
{
    uint64_t generation = (uintptr_t)opaque;
    pthread_mutex_lock(&recording_audio_lock);
    if (generation == recording_audio.generation) recording_audio.anchored = false;
    pthread_mutex_unlock(&recording_audio_lock);
}

static void recording_audio_vm_changed(void *opaque, bool running, RunState state)
{
    recording_audio_notify(opaque, AUD_CNOTIFY_DISABLE);
}

static void recording_audio_samples(void *opaque, const void *buffer, int size)
{
    uint64_t generation = (uintptr_t)opaque;
    const uint8_t *bytes = buffer;
    pthread_mutex_lock(&recording_audio_lock);
    if (generation != recording_audio.generation || !recording_audio.active || recording_audio.failed) goto done;
    if (size < 0 || size % 4) { recording_audio.failed = true; goto done; }
    if (!size) goto done;
    if (!recording_audio.anchored) {
        /* The mixer callback has no device presentation timestamp. Anchor each
         * active interval to its arrival, then preserve exact sample timing. */
        double now = (g_get_monotonic_time() - recording_audio.origin_us) / 1000000.0;
        recording_audio.next_seconds = MAX(recording_audio.next_seconds, MAX(0.0, now - size / 176400.0));
        recording_audio.anchored = true;
    }
    while (size) {
        if (recording_audio.count == RECORDING_AUDIO_PACKETS) {
            recording_audio.failed = true; /* Stop, rather than silently losing audio. */
            break;
        }
        unsigned slot = (recording_audio.head + recording_audio.count) % RECORDING_AUDIO_PACKETS;
        int length = MIN(size, RECORDING_AUDIO_BYTES);
        recording_audio.packets[slot].size = length;
        recording_audio.packets[slot].seconds = recording_audio.next_seconds;
        memcpy(recording_audio.packets[slot].bytes, bytes, length);
        recording_audio.next_seconds += length / 176400.0;
        recording_audio.count++;
        bytes += length;
        size -= length;
    }
done:
    pthread_mutex_unlock(&recording_audio_lock);
}

static void recording_audio_destroy(void *opaque)
{
    uint64_t generation = (uintptr_t)opaque;
    if (recording_audio_context != opaque) return;
    recording_audio_context = NULL;
    recording_audio_voice = NULL;
    if (recording_audio_vm_change) qemu_del_vm_change_state_handler(recording_audio_vm_change);
    recording_audio_vm_change = NULL;
    pthread_mutex_lock(&recording_audio_lock);
    if (generation == recording_audio.generation && recording_audio.active) recording_audio.failed = true;
    pthread_mutex_unlock(&recording_audio_lock);
}

static void recording_audio_start_bh(void *opaque)
{
    uint64_t generation = *(uint64_t *)opaque;
    g_free(opaque);
    pthread_mutex_lock(&recording_audio_lock);
    bool current = recording_audio.active && generation == recording_audio.generation;
    pthread_mutex_unlock(&recording_audio_lock);
    if (!current) return;
    if (recording_audio_voice) audio_be_del_capture(recording_audio_be, recording_audio_voice, recording_audio_context);
    Error *err = NULL;
    AudioBackend *audio = audio_get_default_audio_be(&err);
    struct audsettings settings = { .freq = 44100, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .big_endian = false };
    struct audio_capture_ops ops = { recording_audio_notify, recording_audio_samples, recording_audio_destroy };
    recording_audio_context = (void *)(uintptr_t)generation;
    if (audio) {
        recording_audio_be = audio;
        recording_audio_voice = audio_be_add_capture(audio, &settings, &ops, recording_audio_context);
    }
    if (recording_audio_voice) {
        recording_audio_vm_change = qemu_add_vm_change_state_handler(recording_audio_vm_changed, recording_audio_context);
    } else {
        if (err) { error_report_err(err); }
        recording_audio_destroy(recording_audio_context);
    }
}

static void recording_audio_stop_bh(void *opaque)
{
    uint64_t generation = *(uint64_t *)opaque;
    g_free(opaque);
    pthread_mutex_lock(&recording_audio_lock);
    bool current = generation == recording_audio.generation && !recording_audio.active;
    pthread_mutex_unlock(&recording_audio_lock);
    if (current && recording_audio_voice) audio_be_del_capture(recording_audio_be, recording_audio_voice, recording_audio_context);
}

uint64_t qemu_ios_audio_capture_start(void)
{
    if (!qemu_ios_ui_ready()) return 0;
    pthread_mutex_lock(&recording_audio_lock);
    uint64_t generation = ++recording_audio.generation;
    recording_audio.active = true;
    recording_audio.failed = recording_audio.anchored = false;
    recording_audio.head = recording_audio.count = 0;
    recording_audio.next_seconds = 0;
    recording_audio.origin_us = g_get_monotonic_time();
    pthread_mutex_unlock(&recording_audio_lock);
    uint64_t *request = g_new(uint64_t, 1);
    *request = generation;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), recording_audio_start_bh, request);
    return generation;
}

int qemu_ios_audio_capture_read(uint64_t generation, void *buffer, int capacity, double *seconds)
{
    if (!buffer || capacity < RECORDING_AUDIO_BYTES || !seconds) return -1;
    pthread_mutex_lock(&recording_audio_lock);
    int size = 0;
    *seconds = -1;
    if (generation != recording_audio.generation || recording_audio.failed) size = -1;
    else if (recording_audio.count) {
        unsigned slot = recording_audio.head;
        size = recording_audio.packets[slot].size;
        memcpy(buffer, recording_audio.packets[slot].bytes, size);
        *seconds = recording_audio.packets[slot].seconds;
        recording_audio.head = (slot + 1) % RECORDING_AUDIO_PACKETS;
        recording_audio.count--;
    } else if (recording_audio.active && !recording_audio.anchored) {
        *seconds = (g_get_monotonic_time() - recording_audio.origin_us) / 1000000.0;
    }
    pthread_mutex_unlock(&recording_audio_lock);
    return size;
}

double qemu_ios_audio_capture_time(uint64_t generation)
{
    pthread_mutex_lock(&recording_audio_lock);
    double seconds = generation == recording_audio.generation && recording_audio.active
        ? (g_get_monotonic_time() - recording_audio.origin_us) / 1000000.0 : -1;
    pthread_mutex_unlock(&recording_audio_lock);
    return seconds;
}

void qemu_ios_audio_capture_stop(uint64_t generation)
{
    pthread_mutex_lock(&recording_audio_lock);
    bool current = generation == recording_audio.generation && recording_audio.active;
    if (current) recording_audio.active = false;
    pthread_mutex_unlock(&recording_audio_lock);
    if (!current || !qemu_ios_ui_ready()) return;
    uint64_t *request = g_new(uint64_t, 1);
    *request = generation;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), recording_audio_stop_bh, request);
}
