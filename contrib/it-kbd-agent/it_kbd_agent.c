/*
 * it_kbd_agent — host-keyboard text-input agent for the qemu-ios iPod Touch 2G.
 *
 * Injected into SpringBoard (or any UIKit process) via DYLD_INSERT_LIBRARIES.
 * It polls the emulator's cp15 QEMU_CALL tunnel (QC_POLL_INPUT) for unichars the
 * host typed in the display window, and inserts them into whatever text field
 * has keyboard focus — with NO on-screen keyboard.
 *
 * It types via UIKit's live text-input SPI, -[UIKeyboardImpl acceptInputString:]
 * (and -deleteFromInput for backspace). On iPhone OS 2.x this is the reliable
 * path: UIKit has no hardware-key-event consumer (handleKeyEvent: is 3.2+), and
 * _GSPostSyntheticKeyEvent's synthetic key events are dropped — but acceptInputString:
 * is fully implemented and feeds the first responder directly. Confirmed against
 * the real 2.1.1 device UIKit.
 *
 * The QEMU half (key capture, Command-modifier button remap, the ring, and the
 * QC_POLL_INPUT op) is in hw/arm/ipod_touch_2g.c and hw/arm/guest-services.c.
 *
 * Two things are essential to not wedge SpringBoard's launch (learned the hard
 * way via a full bisect):
 *   1. Do NOTHING in the C constructor except install an ObjC swizzle. Loading
 *      the dylib and doing objc_getClass / method_setImplementation is safe, but
 *      calling into CoreFoundation (e.g. CFRunLoopTimerCreate) from the
 *      constructor is not. So we defer all setup to -[SpringBoard
 *      applicationDidFinishLaunching:], which runs on the main thread after the
 *      run loop exists.
 *   2. The poll timer must not FIRE during the launch window. A repeating timer
 *      whose first fire lands mid-launch stalls SpringBoard at the spinner
 *      regardless of what the tick does (even an empty tick). We set the first
 *      fire date to now + STARTUP_DELAY so ticking only begins once the home
 *      screen is up. (cp15 QC_POLL_INPUT itself is fine post-boot; it was the
 *      timer-during-boot that looked like a cp15 fault in early debugging.)
 *
 * Build: see build.sh (REMOTE=tiger cross-builds armv6 on the Snow Leopard box).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <string.h>

#define QC_POLL_INPUT 0x130

/* Matches qemu_call_t exactly: call_number(4) + args(32) + retval(8) + error(8). */
typedef struct __attribute__((packed)) {
    uint32_t call_number;
    uint8_t  args[32];
    int64_t  retval;
    int64_t  error;
} qemu_call_t;

static int64_t qc_poll_input(void)
{
    qemu_call_t q;
    memset(&q, 0, sizeof(q));
    q.call_number = QC_POLL_INPUT;
    void *p = &q;
    __asm__ volatile("mcr p15, 3, %0, c15, c15, 0" : : "r"(p) : "memory");
    return q.retval;
}

static SEL s_activeInstance, s_acceptInputString, s_deleteFromInput;
static Class s_UIKeyboardImpl;

static void kbd_tick(CFRunLoopTimerRef timer, void *info)
{
    (void)timer; (void)info;

    /*
     * Poll first and touch UIKit only once a key is actually queued. This starts
     * ticking only after STARTUP_DELAY (see the timer setup), so the run loop and
     * keyboard system are fully up by the time we ever call activeInstance.
     */
    int64_t ch = qc_poll_input();
    if (ch <= 0) {
        return;
    }

    /* Something was typed. Resolve the focused keyboard now (nil if no field
     * has focus, in which case we just drain and drop). */
    id impl = s_UIKeyboardImpl
        ? ((id (*)(id, SEL))objc_msgSend)((id)s_UIKeyboardImpl, s_activeInstance)
        : (id)0;

    do {
        if (impl) {
            if (ch == 0x08) {             /* backspace */
                ((void (*)(id, SEL))objc_msgSend)(impl, s_deleteFromInput);
            } else {
                UniChar u = (UniChar)ch;  /* newline/space/printables insert */
                CFStringRef s = CFStringCreateWithCharacters(NULL, &u, 1);
                if (s) {
                    ((void (*)(id, SEL, id))objc_msgSend)(impl, s_acceptInputString, (id)s);
                    CFRelease(s);
                }
            }
        }
        ch = qc_poll_input();
    } while (ch > 0);
}

/*
 * Seconds to wait after applicationDidFinishLaunching: before the poll timer
 * first fires. Must clear the launch window (spinner) so ticking never runs
 * mid-launch. ~12s is comfortably past a cold boot to the home screen.
 */
#define STARTUP_DELAY 12.0

/* Runs on the main thread, after the run loop and keyboard system exist. */
static void it_kbd_agent_setup(void)
{
    s_UIKeyboardImpl    = objc_getClass("UIKeyboardImpl");
    s_activeInstance    = sel_registerName("activeInstance");
    s_acceptInputString = sel_registerName("acceptInputString:");
    s_deleteFromInput   = sel_registerName("deleteFromInput");

    /* Poll on the main run loop so all UIKit work stays on the main thread.
     * First fire is delayed so the timer never ticks during SpringBoard launch. */
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(
        NULL, CFAbsoluteTimeGetCurrent() + STARTUP_DELAY,
        1.0 / 60.0, 0, 0, kbd_tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
}

/* Our swizzled -[SpringBoard applicationDidFinishLaunching:]: run the original,
 * then install the poll timer from this safe, post-run-loop context. */
static IMP s_orig_adfl;
static void it_kbd_adfl(id self, SEL _cmd, id application)
{
    ((void (*)(id, SEL, id))s_orig_adfl)(self, _cmd, application);
    it_kbd_agent_setup();
}

__attribute__((constructor))
static void it_kbd_agent_init(void)
{
    /*
     * Constructor does the minimum: swizzle SpringBoard's launch-finished hook.
     * objc_getClass / method_setImplementation are safe here; CoreFoundation
     * calls are not (see file header). If we're injected into a non-SpringBoard
     * process, the class won't exist and we simply do nothing.
     */
    Class sb = objc_getClass("SpringBoard");
    if (!sb) {
        return;
    }
    SEL sel = sel_registerName("applicationDidFinishLaunching:");
    Method m = class_getInstanceMethod(sb, sel);
    if (!m) {
        return;
    }
    s_orig_adfl = method_getImplementation(m);
    method_setImplementation(m, (IMP)it_kbd_adfl);
}
