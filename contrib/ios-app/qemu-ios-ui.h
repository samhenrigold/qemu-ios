/*
 * The ABI an iOS app uses to drive this emulator. Kept free of QEMU headers so
 * the app can include it directly, and small enough that dlsym'ing each entry
 * point by hand stays reasonable.
 */

#ifndef QEMU_IOS_UI_H
#define QEMU_IOS_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QEMU_IOS_TOUCH_BEGIN  0
#define QEMU_IOS_TOUCH_UPDATE 1
#define QEMU_IOS_TOUCH_END    2

#define QEMU_IOS_BUTTON_HOME        0
#define QEMU_IOS_BUTTON_POWER       1
#define QEMU_IOS_BUTTON_VOLUME_UP   2
#define QEMU_IOS_BUTTON_VOLUME_DOWN 3

/*
 * Runs qemu_init(), the main loop and cleanup on the calling thread. Call it
 * on a background pthread; it does not return until the VM stops. QEMU cannot
 * be started twice in one process, so call it once per app launch.
 */
int qemu_ios_main(int argc, char **argv);

/* Called on the QEMU thread whenever a new frame is ready. Do not block. */
typedef void (*qemu_ios_frame_cb)(void *opaque);

/*
 * Attach the app's display. Safe to call before the VM has a console; the
 * listener is registered as soon as one exists.
 */
void qemu_ios_ui_attach(qemu_ios_frame_cb cb, void *opaque);

/* Called by qemu_ios_main() itself once the VM exists; not for app use. */
void qemu_ios_ui_vm_started(void);

/*
 * The newest frame as tightly packed BGRA, WITHOUT a copy. `serial` is in-out:
 * pass the last one you saw and this returns false if nothing is newer, which
 * is the common case (the guest paints slower than the display refreshes).
 * The pixels stay valid for a few more frames; never free them.
 */
bool qemu_ios_ui_frame(const void **pixels, int *width, int *height,
                       uint64_t *serial);

void qemu_ios_ui_frame_size(int *width, int *height);

/*
 * Touch position is normalised 0..1 over the guest screen, y downwards.
 * One finger only for now -- see the note in qemu-ios-ui.c.
 */
void qemu_ios_ui_touch(int slot, int phase, double nx, double ny);

/*
 * A hardware button, pressed and released as separate calls. Home and power
 * are the two that matter: an emulated device that has gone to sleep cannot be
 * woken any other way.
 */
void qemu_ios_ui_button(int button, bool down);

/*
 * Save the machine to `path`, so the next launch can restore instead of
 * booting. Asynchronous: poll qemu_ios_snapshot_done(). The guest is stopped
 * as a side effect and does not resume.
 *
 * Restore by passing `-incoming file:<path>` at startup.
 */
/*
 * Whether the app is foreground. iOS kills a process that issues GL commands
 * while it is not, so this must be cleared BEFORE the app is backgrounded --
 * on willResignActive, not on didEnterBackground.
 */
void qemu_ios_set_foreground(bool foreground);

void qemu_ios_snapshot_save(const char *path);
bool qemu_ios_snapshot_done(void);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_IOS_UI_H */
