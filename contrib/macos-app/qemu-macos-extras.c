/*
 * macOS-app additions to contrib/ios-app/qemu-ios-ui.c, which is reused
 * unchanged. Every entry point here follows that file's one rule: the app
 * thread allocates a small struct and a bottom half runs it on the QEMU
 * thread under the BQL.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "block/aio.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/boards.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"

#include "qemu-main.h"

#include <Carbon/Carbon.h>          /* kVK_* virtual keycodes */

#include "qemu-macos-extras.h"

/*
 * system/main.c (replaced by qemu-ios-entry.c) defined this; ui/cocoa.m still
 * references it. It stays NULL under -display none, and qemu-ios-entry.c owns
 * the main loop regardless.
 */
int (*qemu_main)(void);

/* Phase values shared with qemu-ios-ui.h (not included: it has no guards
 * against double-declaring the ABI and this file only needs the constants). */
#define TOUCH_END 2

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
        if (t->phase == TOUCH_END) {
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
    aio_bh_schedule_oneshot(qemu_get_aio_context(), shake_bh, NULL);
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
    aio_bh_schedule_oneshot(qemu_get_aio_context(), paste_bh, g_strdup(utf8));
}

/* --- machine controls ---------------------------------------------------- */

typedef void (*qmp_void_fn)(Error **errp);

static void qmp_bh(void *opaque)
{
    qmp_void_fn fn = opaque;
    Error *err = NULL;

    fn(&err);
    if (err) {
        fprintf(stderr, "[machine] %s\n", error_get_pretty(err));
        error_free(err);
    }
}

static void schedule_qmp(qmp_void_fn fn)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), qmp_bh, (void *)fn);
}

void qemu_ios_ui_pause(void)     { schedule_qmp(qmp_stop); }
void qemu_ios_ui_resume(void)    { schedule_qmp(qmp_cont); }
void qemu_ios_ui_reset(void)     { schedule_qmp(qmp_system_reset); }
void qemu_ios_ui_powerdown(void) { schedule_qmp(qmp_system_powerdown); }
void qemu_ios_ui_quit(void)      { schedule_qmp(qmp_quit); }
