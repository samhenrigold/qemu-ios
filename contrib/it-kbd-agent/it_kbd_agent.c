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
    if (!s_UIKeyboardImpl) {
        return;
    }
    /* The active keyboard instance (nil if nothing has focus -> input is a no-op). */
    id impl = ((id (*)(id, SEL))objc_msgSend)((id)s_UIKeyboardImpl, s_activeInstance);
    if (!impl) {
        /* Drain and drop so the ring doesn't back up while nothing is focused. */
        while (qc_poll_input() > 0) { }
        return;
    }

    for (int i = 0; i < 64; i++) {
        int64_t ch = qc_poll_input();
        if (ch <= 0) {
            break;
        }
        if (ch == 0x08) {                 /* backspace */
            ((void (*)(id, SEL))objc_msgSend)(impl, s_deleteFromInput);
            continue;
        }
        UniChar u = (UniChar)ch;          /* newline/space/printables all insert */
        CFStringRef s = CFStringCreateWithCharacters(NULL, &u, 1);
        if (s) {
            /* CFStringRef is toll-free bridged to NSString. */
            ((void (*)(id, SEL, id))objc_msgSend)(impl, s_acceptInputString, (id)s);
            CFRelease(s);
        }
    }
}

__attribute__((constructor))
static void it_kbd_agent_init(void)
{
    s_UIKeyboardImpl    = objc_getClass("UIKeyboardImpl");
    s_activeInstance    = sel_registerName("activeInstance");
    s_acceptInputString = sel_registerName("acceptInputString:");
    s_deleteFromInput   = sel_registerName("deleteFromInput");

    /* Poll on the main run loop so all UIKit work stays on the main thread. */
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent(),
                                               1.0 / 60.0, 0, 0, kbd_tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
}
