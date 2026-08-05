/*
 * Entry point for embedding this emulator in an iOS app.
 *
 * On macOS, main() runs qemu_init() on the main thread, hands the main loop to
 * a second thread and gives the main thread to CFRunLoopRun(), because the
 * Cocoa UI needs it. An iOS app already owns its main thread and its runloop,
 * so instead the app calls qemu_ios_main() on a background pthread and the
 * whole emulator -- init, main loop, cleanup -- lives there.
 *
 * QEMU is not designed to be started twice in one process: it never fully
 * releases what it allocates. One VM per app launch, as in UTM.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "system/replay.h"
#include "system/system.h"

#include "qemu-ios-ui.h"

int qemu_ios_main(int argc, char **argv);

int qemu_ios_main(int argc, char **argv)
{
    int status;

    qemu_init(argc, argv);

    /*
     * Resume automatically when restoring a snapshot.
     *
     * -incoming clears `autostart` (system/vl.c), so a restored machine sits
     * in `paused` forever waiting for a `cont` that, in this app, no monitor is
     * ever going to send. Setting it back here means the VM starts itself the
     * moment the incoming stream is consumed -- which is what makes restore
     * look instant rather than look broken.
     */
    autostart = 1;

    /* Console and AIO context exist only now; the display attaches here. */
    qemu_ios_ui_vm_started();

    /*
     * qemu_init() returns holding the BQL and the replay mutex, and the main
     * loop expects to take them itself. Handing them straight back keeps this
     * identical to the sequence in system/main.c.
     */
    bql_unlock();
    replay_mutex_unlock();

    replay_mutex_lock();
    bql_lock();
    status = qemu_main_loop();
    qemu_cleanup(status);
    bql_unlock();
    replay_mutex_unlock();

    return status;
}
