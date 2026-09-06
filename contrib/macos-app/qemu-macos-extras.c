/*
 * macOS-app additions to contrib/ios-app/qemu-ios-ui.c, which is reused
 * unchanged. Every entry point here follows that file's one rule: the app
 * thread allocates a small struct and a bottom half runs it on the QEMU
 * thread under the BQL.
 */

#include "qemu/osdep.h"
#include "audio/audio.h"
#include "system/runstate.h"
#include <pthread.h>
#include "qemu-ios-ui.h"
#include "qemu/main-loop.h"
#include "block/aio.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/boards.h"
#include "hw/arm/ipod-attitude.h"
#include "qapi/qapi-commands-qom.h"
#include "qobject/qnum.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"

#include "qemu-main.h"

#include <dlfcn.h>
#include <mach-o/loader.h>
#include <Carbon/Carbon.h>          /* kVK_* virtual keycodes */

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
    if (!qemu_ios_ui_ready()) {
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

    if (con) {
        qemu_input_event_send_key_qcode(con, k->qcode, k->down);
    }
    g_free(k);
}

void qemu_ios_ui_key(int qcode, bool down)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    struct key_event *k = g_new0(struct key_event, 1);
    k->qcode = qcode;
    k->down = down;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), key_bh, k);
}

/*
 * macOS virtual keycode (NSEvent.keyCode / kVK_*) to QKeyCode. The same table
 * ui/cocoa.m uses -- kept here verbatim so host-keyboard passthrough matches
 * the built-in Cocoa UI exactly and the app never has to carry QKeyCode
 * integer constants in Swift.
 */
static const int mac_to_qkeycode_map[] = {
    [kVK_ANSI_A] = Q_KEY_CODE_A, [kVK_ANSI_B] = Q_KEY_CODE_B,
    [kVK_ANSI_C] = Q_KEY_CODE_C, [kVK_ANSI_D] = Q_KEY_CODE_D,
    [kVK_ANSI_E] = Q_KEY_CODE_E, [kVK_ANSI_F] = Q_KEY_CODE_F,
    [kVK_ANSI_G] = Q_KEY_CODE_G, [kVK_ANSI_H] = Q_KEY_CODE_H,
    [kVK_ANSI_I] = Q_KEY_CODE_I, [kVK_ANSI_J] = Q_KEY_CODE_J,
    [kVK_ANSI_K] = Q_KEY_CODE_K, [kVK_ANSI_L] = Q_KEY_CODE_L,
    [kVK_ANSI_M] = Q_KEY_CODE_M, [kVK_ANSI_N] = Q_KEY_CODE_N,
    [kVK_ANSI_O] = Q_KEY_CODE_O, [kVK_ANSI_P] = Q_KEY_CODE_P,
    [kVK_ANSI_Q] = Q_KEY_CODE_Q, [kVK_ANSI_R] = Q_KEY_CODE_R,
    [kVK_ANSI_S] = Q_KEY_CODE_S, [kVK_ANSI_T] = Q_KEY_CODE_T,
    [kVK_ANSI_U] = Q_KEY_CODE_U, [kVK_ANSI_V] = Q_KEY_CODE_V,
    [kVK_ANSI_W] = Q_KEY_CODE_W, [kVK_ANSI_X] = Q_KEY_CODE_X,
    [kVK_ANSI_Y] = Q_KEY_CODE_Y, [kVK_ANSI_Z] = Q_KEY_CODE_Z,
    [kVK_ANSI_0] = Q_KEY_CODE_0, [kVK_ANSI_1] = Q_KEY_CODE_1,
    [kVK_ANSI_2] = Q_KEY_CODE_2, [kVK_ANSI_3] = Q_KEY_CODE_3,
    [kVK_ANSI_4] = Q_KEY_CODE_4, [kVK_ANSI_5] = Q_KEY_CODE_5,
    [kVK_ANSI_6] = Q_KEY_CODE_6, [kVK_ANSI_7] = Q_KEY_CODE_7,
    [kVK_ANSI_8] = Q_KEY_CODE_8, [kVK_ANSI_9] = Q_KEY_CODE_9,
    [kVK_ANSI_Grave] = Q_KEY_CODE_GRAVE_ACCENT,
    [kVK_ANSI_Minus] = Q_KEY_CODE_MINUS,
    [kVK_ANSI_Equal] = Q_KEY_CODE_EQUAL,
    [kVK_Delete] = Q_KEY_CODE_BACKSPACE,
    [kVK_CapsLock] = Q_KEY_CODE_CAPS_LOCK,
    [kVK_Tab] = Q_KEY_CODE_TAB,
    [kVK_Return] = Q_KEY_CODE_RET,
    [kVK_ANSI_LeftBracket] = Q_KEY_CODE_BRACKET_LEFT,
    [kVK_ANSI_RightBracket] = Q_KEY_CODE_BRACKET_RIGHT,
    [kVK_ANSI_Backslash] = Q_KEY_CODE_BACKSLASH,
    [kVK_ANSI_Semicolon] = Q_KEY_CODE_SEMICOLON,
    [kVK_ANSI_Quote] = Q_KEY_CODE_APOSTROPHE,
    [kVK_ANSI_Comma] = Q_KEY_CODE_COMMA,
    [kVK_ANSI_Period] = Q_KEY_CODE_DOT,
    [kVK_ANSI_Slash] = Q_KEY_CODE_SLASH,
    [kVK_Space] = Q_KEY_CODE_SPC,
    [kVK_UpArrow] = Q_KEY_CODE_UP,
    [kVK_DownArrow] = Q_KEY_CODE_DOWN,
    [kVK_LeftArrow] = Q_KEY_CODE_LEFT,
    [kVK_RightArrow] = Q_KEY_CODE_RIGHT,
    [kVK_Home] = Q_KEY_CODE_HOME,
    [kVK_PageUp] = Q_KEY_CODE_PGUP,
    [kVK_PageDown] = Q_KEY_CODE_PGDN,
    [kVK_End] = Q_KEY_CODE_END,
    [kVK_ForwardDelete] = Q_KEY_CODE_DELETE,
    [kVK_Escape] = Q_KEY_CODE_ESC,
};

void qemu_ios_ui_key_mac(int mac_keycode, bool down)
{
    int qcode;

    if (mac_keycode < 0 ||
        (size_t)mac_keycode >= ARRAY_SIZE(mac_to_qkeycode_map)) {
        return;
    }
    qcode = mac_to_qkeycode_map[mac_keycode];
    if (qcode == 0) {                 /* unmapped (Q_KEY_CODE_UNMAPPED) */
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

static void accel_bh(void *opaque)
{
    int *v = opaque;
    Object *machine = OBJECT(qdev_get_machine());
    static const char *props[] = { "accel-x", "accel-y", "accel-z" };
    Error *err = NULL;

    for (int i = 0; i < 3 && !err; i++) {
        object_property_set_int(machine, props[i], v[i], &err);
    }
    if (err) {
        fprintf(stderr, "[accel] %s\n", error_get_pretty(err));
        error_free(err);
    }
    g_free(v);
}

void qemu_ios_ui_accel(int x, int y, int z)
{
    if (!qemu_ios_ui_ready()) {
        return;
    }
    int *v = g_new(int, 3);
    v[0] = x; v[1] = y; v[2] = z;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), accel_bh, v);
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
    if (!err) object_property_set_str(machine, "battery-charging", modes[input->charging], &err);
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

/* Agent operations use a separate mutex and an acquired lifetime reference;
 * they never touch CPU/device state or hold the BQL. */
#include "hw/arm/ipod-agent.h"

bool qemu_ios_agent_request(const char *request)
{
    if (!request || !qemu_ios_ui_ready()) {
        return false;
    }
    IPodAgent *a = ipod_agent_acquire();
    bool accepted = a && ipod_agent_submit(a, request);
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

/* Atomic renderer count; safe while the main thread presents the device. */
extern int gles_host_context_count(void);
int qemu_ios_gles_contexts(void)
{
    return qemu_ios_ui_ready() ? gles_host_context_count() : 0;
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
    if (recording_audio_voice) AUD_del_capture(recording_audio_voice, recording_audio_context);
    Error *err = NULL;
    AudioState *audio = audio_get_default_audio_state(&err);
    struct audsettings settings = { .freq = 44100, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .endianness = 0 };
    struct audio_capture_ops ops = { recording_audio_notify, recording_audio_samples, recording_audio_destroy };
    recording_audio_context = (void *)(uintptr_t)generation;
    if (audio) recording_audio_voice = AUD_add_capture(audio, &settings, &ops, recording_audio_context);
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
    if (current && recording_audio_voice) AUD_del_capture(recording_audio_voice, recording_audio_context);
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
