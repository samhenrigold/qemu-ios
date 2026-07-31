/*
 * it_kbd_agent — host-keyboard text-input agent for the qemu-ios iPod Touch 2G.
 *
 * Injected into SpringBoard (or any UIKit process) via DYLD_INSERT_LIBRARIES.
 * It polls the emulator's cp15 QEMU_CALL tunnel (QC_POLL_INPUT) for unichars the
 * host typed in the display window, and feeds each one to GraphicsServices'
 * _GSPostSyntheticKeyEvent, which drives the same text-input path the on-screen
 * keyboard uses — so text appears with no OSK.
 *
 * The QEMU half (key capture, Command-modifier button remap, the ring, and the
 * QC_POLL_INPUT op) is in hw/arm/ipod_touch_2g.c and hw/arm/guest-services.c.
 *
 * Build: see build.sh (clang -arch armv6 against iPhoneOS2.0.sdk).
 * Inject: set DYLD_INSERT_LIBRARIES=/path/to/it_kbd_agent.dylib in SpringBoard's
 * launchd plist (imgtools/patch_launchd_env.py), same route as other tweaks.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

/* QC_POLL_INPUT from include/hw/arm/guest-services/general.h */
#define QC_POLL_INPUT 0x130

/*
 * Must match qemu_call_t exactly: call_number(4) + args union(32, the largest
 * member is qc_read_file_args_t = 4 x uint64) + retval(8) + error(8) = 52 bytes,
 * packed. QEMU reads/writes sizeof(qemu_call_t) bytes at the pointer we pass.
 */
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
    /* QEMU_CALL: mcr p15, 3, <guest ptr>, c15, c15, 0 (PL0-accessible). QEMU
     * reads the struct at the virtual address we hand it and writes the reply
     * back into it. */
    __asm__ volatile("mcr p15, 3, %0, c15, c15, 0" : : "r"(p) : "memory");
    return q.retval;
}

typedef void (*gs_post_fn)(CFStringRef str, uint8_t isUp, uint8_t isRepeating);
static gs_post_fn gs_post;

static void kbd_tick(CFRunLoopTimerRef timer, void *info)
{
    (void)timer; (void)info;
    if (!gs_post) {
        return;
    }
    /* Drain everything queued this tick so fast typing keeps up. */
    for (int i = 0; i < 64; i++) {
        int64_t ch = qc_poll_input();
        if (ch <= 0) {
            break;
        }
        UniChar u = (UniChar)ch;
        CFStringRef s = CFStringCreateWithCharacters(NULL, &u, 1);
        if (s) {
            gs_post(s, 0, 0);   /* whole-string synthetic key event */
            CFRelease(s);
        }
    }
}

__attribute__((constructor))
static void it_kbd_agent_init(void)
{
    /* _GSPostSyntheticKeyEvent is a private GraphicsServices export; resolve it
     * at runtime (no ASLR/shared cache on 2.x, but dlsym keeps us build-agnostic). */
    gs_post = (gs_post_fn)dlsym(RTLD_DEFAULT, "GSPostSyntheticKeyEvent");
    if (!gs_post) {
        void *h = dlopen("/System/Library/PrivateFrameworks/"
                         "GraphicsServices.framework/GraphicsServices", RTLD_NOW);
        if (h) {
            gs_post = (gs_post_fn)dlsym(h, "GSPostSyntheticKeyEvent");
        }
    }

    /* Poll on the main run loop so all UIKit/GraphicsServices work stays on the
     * main thread. ~60 Hz is plenty for typing. */
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent(),
                                               1.0 / 60.0, 0, 0, kbd_tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
}
