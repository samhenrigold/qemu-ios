/*
 * it_kbd_agent — host-keyboard text-input agent for the qemu-ios iPod Touch 2G.
 *
 * ============================================================================
 * WARNING: THIS SOURCE HAS NEVER BEEN RUN ON A GUEST. (2026-08-05)
 *
 * It was rewritten to dlopen CoreFoundation/libobjc and resolve every symbol
 * via dlsym, because the original directly #include'd and linked those
 * frameworks and therefore could only be built on a Snow Leopard box that no
 * longer exists. The rewrite COMPILES and LINKS cleanly to a valid armv6
 * Mach-O via contrib/armv6-toolchain -- and that is the entire extent of the
 * evidence. It has not been injected into SpringBoard, so nothing here is
 * known to work at runtime.
 *
 * The committed it_kbd_agent.dylib is therefore still the ONLY trusted
 * artefact and is deliberately still tracked in git. Do not delete it, and do
 * not ship a dylib built from this source, until someone injects the rebuilt
 * one into a guest and confirms typing actually reaches a text field.
 *
 * Blocker for whoever picks this up: the agent is not baked into
 * nand-appsync3 or nand-canonical (no dylib, no DYLD_INSERT_LIBRARIES plist),
 * and imgtools/editimg.py currently refuses to write back to either image
 * because both fail its fsck_hfs gate. Injecting over SSH-on-usbmuxd is the
 * likelier route -- see the qemu-ios-ssh-over-usb notes.
 *
 * Do NOT cite an IT_OSK=1 typing demo as evidence for this agent: that path
 * drives synthetic on-screen-keyboard taps from the host and does not load
 * this dylib at all.
 * ============================================================================
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
 * Build: see build.sh -- contrib/armv6-toolchain. Neither CoreFoundation nor
 * libobjc is linked: `ld -framework CoreFoundation` / `-lobjc` against the
 * 3.1.3 SDK is fatal (same issue as contrib/it-pasteboard and contrib/it-gles),
 * so both are dlopen'd and every symbol resolved with dlsym, matching the
 * pattern in it_pbd.c / sblaunch.c / gles_fw.c. Per point 1 above, the
 * CoreFoundation handle and symbols are resolved lazily, on first use inside
 * it_kbd_agent_setup(), never from the constructor.
 */
#include <stdint.h>
#include <string.h>

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2

typedef void *id_;
typedef void *SEL_;
typedef void *Class_;
typedef void *Method_;
typedef void *IMP_;

static void *(*p_objc_getClass)(const char *);
static void *(*p_sel_registerName)(const char *);
static void *(*p_objc_msgSend)(void *, void *, ...);
static void *(*p_class_getInstanceMethod)(void *, void *);
static void *(*p_method_getImplementation)(void *);
static void  (*p_method_setImplementation)(void *, void *);

typedef unsigned short UniChar_;
typedef double CFTimeInterval_;
typedef double CFAbsoluteTime_;
typedef void (*CFRunLoopTimerCallBack_)(void *timer, void *info);

static void  *(*p_CFRunLoopTimerCreate)(void *, CFAbsoluteTime_, CFTimeInterval_,
                                         unsigned, int, CFRunLoopTimerCallBack_,
                                         void *);
static void   (*p_CFRunLoopAddTimer)(void *, void *, void *);
static void  *(*p_CFRunLoopGetMain)(void);
static CFAbsoluteTime_ (*p_CFAbsoluteTimeGetCurrent)(void);
static void  *(*p_CFStringCreateWithCharacters)(void *, const UniChar_ *, long);
static void   (*p_CFRelease)(void *);
static void  *s_kCFRunLoopCommonModes;

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

static SEL_ s_activeInstance, s_acceptInputString, s_deleteFromInput;
static Class_ s_UIKeyboardImpl;

static void kbd_tick(void *timer, void *info)
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
    id_ impl = s_UIKeyboardImpl
        ? p_objc_msgSend(s_UIKeyboardImpl, s_activeInstance)
        : (id_)0;

    do {
        if (impl) {
            if (ch == 0x08) {             /* backspace */
                p_objc_msgSend(impl, s_deleteFromInput);
            } else {
                UniChar_ u = (UniChar_)ch;  /* newline/space/printables insert */
                void *s = p_CFStringCreateWithCharacters(0, &u, 1);
                if (s) {
                    p_objc_msgSend(impl, s_acceptInputString, s);
                    p_CFRelease(s);
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

/* Runs on the main thread, after the run loop and keyboard system exist. This
 * is also where CoreFoundation is dlopen'd for the first time -- safe here,
 * unsafe from the constructor (see file header). */
static void it_kbd_agent_setup(void)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/"
                       "CoreFoundation", RTLD_NOW);
    if (!cf) {
        return;
    }
    p_CFRunLoopTimerCreate         = dlsym(cf, "CFRunLoopTimerCreate");
    p_CFRunLoopAddTimer            = dlsym(cf, "CFRunLoopAddTimer");
    p_CFRunLoopGetMain             = dlsym(cf, "CFRunLoopGetMain");
    p_CFAbsoluteTimeGetCurrent     = dlsym(cf, "CFAbsoluteTimeGetCurrent");
    p_CFStringCreateWithCharacters = dlsym(cf, "CFStringCreateWithCharacters");
    p_CFRelease                    = dlsym(cf, "CFRelease");
    /* kCFRunLoopCommonModes is a CFStringRef *variable*, not a function --
     * dlsym gives the address of the variable, so it needs a dereference. */
    void *modes_var = dlsym(cf, "kCFRunLoopCommonModes");
    if (!p_CFRunLoopTimerCreate || !p_CFRunLoopAddTimer || !p_CFRunLoopGetMain ||
        !p_CFAbsoluteTimeGetCurrent || !p_CFStringCreateWithCharacters ||
        !p_CFRelease || !modes_var) {
        return;
    }
    s_kCFRunLoopCommonModes = *(void **)modes_var;

    s_UIKeyboardImpl    = p_objc_getClass("UIKeyboardImpl");
    s_activeInstance    = p_sel_registerName("activeInstance");
    s_acceptInputString = p_sel_registerName("acceptInputString:");
    s_deleteFromInput   = p_sel_registerName("deleteFromInput");

    /* Poll on the main run loop so all UIKit work stays on the main thread.
     * First fire is delayed so the timer never ticks during SpringBoard launch. */
    void *t = p_CFRunLoopTimerCreate(
        0, p_CFAbsoluteTimeGetCurrent() + STARTUP_DELAY,
        1.0 / 60.0, 0, 0, kbd_tick, 0);
    p_CFRunLoopAddTimer(p_CFRunLoopGetMain(), t, s_kCFRunLoopCommonModes);
}

/* Our swizzled -[SpringBoard applicationDidFinishLaunching:]: run the original,
 * then install the poll timer from this safe, post-run-loop context. */
static IMP_ s_orig_adfl;
static void it_kbd_adfl(id_ self, SEL_ _cmd, id_ application)
{
    ((void (*)(id_, SEL_, id_))s_orig_adfl)(self, _cmd, application);
    it_kbd_agent_setup();
}

__attribute__((constructor))
static void it_kbd_agent_init(void)
{
    /*
     * Constructor does the minimum: resolve the ObjC runtime (already loaded
     * into every UIKit process, so dlopen here just bumps a refcount -- no
     * different from the objc_getClass / method_setImplementation calls this
     * did when directly linked) and swizzle SpringBoard's launch-finished
     * hook. CoreFoundation calls are not safe here (see file header). If
     * we're injected into a non-SpringBoard process, the class won't exist
     * and we simply do nothing.
     */
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    if (!objc) {
        return;
    }
    p_objc_getClass            = dlsym(objc, "objc_getClass");
    p_sel_registerName         = dlsym(objc, "sel_registerName");
    p_objc_msgSend              = dlsym(objc, "objc_msgSend");
    p_class_getInstanceMethod  = dlsym(objc, "class_getInstanceMethod");
    p_method_getImplementation = dlsym(objc, "method_getImplementation");
    p_method_setImplementation = dlsym(objc, "method_setImplementation");
    if (!p_objc_getClass || !p_sel_registerName || !p_objc_msgSend ||
        !p_class_getInstanceMethod || !p_method_getImplementation ||
        !p_method_setImplementation) {
        return;
    }

    Class_ sb = p_objc_getClass("SpringBoard");
    if (!sb) {
        return;
    }
    SEL_ sel = p_sel_registerName("applicationDidFinishLaunching:");
    Method_ m = p_class_getInstanceMethod(sb, sel);
    if (!m) {
        return;
    }
    s_orig_adfl = p_method_getImplementation(m);
    p_method_setImplementation(m, (void *)it_kbd_adfl);
}
