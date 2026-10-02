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

/*
 * What an app needs to know about a machine BEFORE it boots it: which -M name
 * to pass, how big a window to open, which way up. Static per machine, so it
 * is valid before qemu_ios_main() and needs no lock. Unknown name -> NULL.
 */
typedef struct {
    const char *machine;        /* the -M name */
    int screen_width;           /* framebuffer pixels at default_orientation */
    int screen_height;
    int screen_scale;           /* points per pixel */
    int default_orientation;    /* 0 portrait, 1 landscape */
    bool has_cellular;
} QemuIosDeviceInfo;

const QemuIosDeviceInfo *qemu_ios_device_info(const char *machine);

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
 * True only while the emulator is initialised and its main loop is running.
 * Entry points that schedule bottom halves must check this: before qemu_init()
 * the AioContext is NULL, and after the main loop returns nothing services it.
 */
bool qemu_ios_ui_ready(void);
/* Session-latched host NAND I/O failure; thread-safe to poll from the UI. */
bool qemu_ios_ui_storage_failed(void);

/* True only after the guest writes the final PMU power-off command. */
bool qemu_ios_ui_guest_shutdown_confirmed(void);
/* Guest backlight state, independent of black framebuffer contents. */
bool qemu_ios_ui_display_sleeping(void);
void qemu_ios_ui_vm_stopped(void);

/*
 * The newest frame as tightly packed BGRA, WITHOUT a copy. `serial` is in-out:
 * pass the last one you saw and this returns false if nothing is newer, which
 * is the common case (the guest paints slower than the display refreshes).
 * The pixels stay valid for a few more frames; never free them.
 */
bool qemu_ios_ui_frame(const void **pixels, int *width, int *height,
                       uint64_t *serial);

/* Stable copy into caller-owned memory; also works when no frame advanced.
 * False leaves outputs untouched (no frame or insufficient capacity). */
bool qemu_ios_ui_copy_frame(void *pixels, size_t capacity, int *width, int *height);

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

/* Generic button/single-touch sequences in virtual milliseconds, not wall
 * time. Arrays have count elements; transitions must be balanced and ordered.
 * A new submission cancels/releases the prior sequence. Manual input cancels
 * it too; held manual input causes admission refusal. The immediate return
 * means queued, not executed; poll the most recent ID for actual admission.
 * Explicit cancellation releases only owned signals even while paused.
 * This is host automation state and is not part of a guest snapshot. */
#define QEMU_IOS_INPUT_UNKNOWN 0
#define QEMU_IOS_INPUT_RUNNING 1
#define QEMU_IOS_INPUT_DONE 2
#define QEMU_IOS_INPUT_CANCELLED 3
#define QEMU_IOS_INPUT_REJECTED 4
bool qemu_ios_ui_input_sequence(uint64_t id, size_t count,
    const int64_t *at_ms, const int32_t *kind, const int32_t *value,
    const int32_t *phase, const double *x, const double *y);
int qemu_ios_ui_input_sequence_status(uint64_t id);
void qemu_ios_ui_input_sequence_cancel(uint64_t id);
/* Internal: QEMU thread under BQL, before reset/quit/semantic powerdown. */
void qemu_ios_ui_cancel_input(void);
void qemu_ios_ui_manual_touch2(bool down);

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

/*
 * Tracked snapshot save. Unlike qemu_ios_snapshot_save/_done, this reports
 * real progress: _status() distinguishes "not started", "in flight", "done"
 * and "failed", and fills errbuf with the reason on failure. _save2 sets the
 * status to RUNNING on the calling thread before scheduling the work, so a
 * status() call that races ahead of the bottom half never reads a stale DONE.
 */
typedef enum {
    QEMU_IOS_SNAPSHOT_IDLE = 0,
    QEMU_IOS_SNAPSHOT_RUNNING = 1,
    QEMU_IOS_SNAPSHOT_DONE = 2,
    QEMU_IOS_SNAPSHOT_FAILED = 3,
} QemuIosSnapshotStatus;

void qemu_ios_snapshot_save2(const char *path);
QemuIosSnapshotStatus qemu_ios_snapshot_status(char *errbuf, unsigned long errlen);

/* Resume the vCPU after a completed save, without touching foreground state
 * (qemu_ios_set_foreground(true) is the only other thing that restarts it). */
void qemu_ios_snapshot_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_IOS_UI_H */
