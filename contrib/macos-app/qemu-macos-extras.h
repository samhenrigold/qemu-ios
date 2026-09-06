/*
 * macOS-app additions to the qemu-ios-ui.h ABI (see contrib/ios-app).
 * Same rules: app-thread safe, everything is marshalled onto the QEMU
 * thread through a bottom half.
 */

#ifndef QEMU_MACOS_EXTRAS_H
#define QEMU_MACOS_EXTRAS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Second finger for pinch/rotate, on QEMU's multi-touch path (slot 1,
 * distinct tracking id). Finger 0 stays on the legacy abs path in
 * qemu_ios_ui_touch(); mixing the two is what the panel model expects
 * from the existing tooling. Phases are QEMU_IOS_TOUCH_*.
 */
void qemu_ios_ui_touch2(int phase, double nx, double ny);

/* A host key event, as a QKeyCode value (Q_KEY_CODE_*). */
void qemu_ios_ui_key(int qcode, bool down);

/* Rotate the device (Meta+Left/Right chord); edge-triggered, self-releasing. */
void qemu_ios_ui_rotate(bool clockwise);

/*
 * A host key event by macOS virtual keycode (NSEvent.keyCode / kVK_*). Uses
 * the same mapping as ui/cocoa.m, so the app can forward key events without
 * carrying QKeyCode constants. Unmapped keycodes are ignored.
 */
void qemu_ios_ui_key_mac(int mac_keycode, bool down);

/* Shake gesture (machine property "accel-shake"). */
void qemu_ios_ui_shake(void);

/*
 * Raw accelerometer counts (machine properties "accel-x/y/z"; ±0x40 == ±1 g).
 * Sets the steady-state vector, so it persists until the next call or the
 * next orientation change.
 */
void qemu_ios_ui_accel(int x, int y, int z);

/* Queue UTF-8 text for the guest pasteboard (machine property "pasteboard"). */
void qemu_ios_ui_paste(const char *utf8);

/* Local bounded RPC. Request is id/op header + newline + base64 body.
 * Result is an owned id/status header + newline + base64 body, or NULL.
 * Always free a non-NULL result with qemu_ios_agent_free_result. */
bool qemu_ios_agent_request(const char *request);
/* Stops pending work; an already executed mutation cannot be undone. */
void qemu_ios_agent_cancel(const char *id);
char *qemu_ios_agent_result(void);
void qemu_ios_agent_free_result(char *result);
/* 0 absent/not running, 1 alive, 2 stale. */
int qemu_ios_agent_status(void);

/* Live host GL contexts cannot be included in a snapshot. */
int qemu_ios_gles_contexts(void);

/* Machine controls. */
void qemu_ios_ui_pause(void);
void qemu_ios_ui_resume(void);
void qemu_ios_ui_reset(void);
void qemu_ios_ui_powerdown(void);
void qemu_ios_ui_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_MACOS_EXTRAS_H */
