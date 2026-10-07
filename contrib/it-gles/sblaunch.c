/*
 * sblaunch -- ask SpringBoard to launch an app, by bundle identifier.
 *
 * Launching from SpringBoard is not a convenience here, it is the requirement:
 * a process started from a shell gets a UIWindow that SpringBoard never
 * composites, so a CAEAGLLayer in it has no drawable behind it. The normal way
 * in is to tap the icon, but touch on this emulator is load-sensitive and
 * another agent is actively fixing a desync in it -- and a failed tap and an
 * app that refused to launch look identical.
 *
 * SBSLaunchApplicationWithIdentifier goes through exactly the same SpringBoard
 * path a tap does (SpringBoard is what execs the app either way), so this
 * removes touch as a variable without weakening the test.
 *
 * Historical callers pass the identifier through /tmp/sblaunch.id; keep that
 * protocol for scripts that predate the legacy crt1 startup. The GL test app
 * is the default when the file is absent. Both exported SpringBoard launch
 * APIs are selected by the shared helper in it-agent/sbs-launch.h.
 *
 *     echo -n com.andyqua.CubeRunner > /tmp/sblaunch.id && sblaunch
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int open(const char *, int, ...);
extern long read(int, void *, unsigned long);
extern int close(int);
extern int strcmp(const char *, const char *);
#ifndef RTLD_NOW
#define RTLD_NOW 2
#endif
#define O_RDONLY 0
#include "../it-agent/sbs-launch.h"

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(1, s, slen(s)); }
static void wd(unsigned v)
{
    char b[12], *p = b + 11;
    *p = 0;
    if (!v) *--p = '0';
    while (v) { *--p = '0' + (v % 10); v /= 10; }
    w(p);
}

#define kCFStringEncodingUTF8 0x08000100

int main(void)
{
    static char bundle_id[128] = "com.qemuios.gltest";
    void *cf, *sbs;
    int r;

    /* Read the identifier, if one was left for us. Trailing whitespace is
     * trimmed so that a plain `echo` (which appends a newline) works -- an
     * identifier with a stray \n is rejected by SpringBoard with the same
     * error code as an app that is not installed, which is a confusing way to
     * lose an afternoon. */
    {
        int fd = open("/tmp/sblaunch.id", O_RDONLY);
        if (fd >= 0) {
            long n = read(fd, bundle_id, sizeof(bundle_id) - 1);
            close(fd);
            if (n > 0) {
                while (n > 0 && (unsigned char)bundle_id[n - 1] <= ' ') {
                    n--;
                }
                bundle_id[n] = 0;
            }
        }
    }

    cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/"
                "CoreFoundation", RTLD_NOW);
    sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices."
                 "framework/SpringBoardServices", RTLD_NOW);
    if (!cf || !sbs) { w("sblaunch: dlopen failed\n"); _exit(1); }

    /* Diagnostic requests use the same no-argv file protocol. The lock ABI
     * is verified against the 3.1.3 MIG stub: both out parameters are bytes. */
    if (!strcmp(bundle_id, ":lock-status")) {
        unsigned (*port)(void) = dlsym(sbs, "SBSSpringBoardServerPort");
        int (*status)(unsigned, unsigned char *, unsigned char *) =
            dlsym(sbs, "SBGetScreenLockStatus");
        unsigned char locked = 1, passcode = 1;
        if (!port || !status || status(port(), &locked, &passcode)) {
            w("sblaunch: lock status unavailable\n"); _exit(1);
        }
        w("sblaunch: locked="); wd(locked);
        w(" passcode="); wd(passcode); w("\n");
        _exit(0);
    }
    if (!strcmp(bundle_id, ":frontmost")) {
        void *(*frontmost)(void) = dlsym(sbs, "SBSCopyFrontmostApplicationDisplayIdentifier");
        unsigned char (*get_string)(void *, char *, long, unsigned) =
            dlsym(cf, "CFStringGetCString");
        void (*release)(void *) = dlsym(cf, "CFRelease");
        char name[1024];
        void *front;
        if (!frontmost || !get_string || !release) _exit(1);
        front = frontmost();
        if (!front || !get_string(front, name, sizeof(name), kCFStringEncodingUTF8)) {
            w("sblaunch: no foreground app\n"); _exit(1);
        }
        w("sblaunch: frontmost="); w(name); w("\n");
        release(front);
        _exit(0);
    }

    r = it_sbs_launch(cf, sbs, bundle_id);
    /* 0 is success; anything else is SpringBoard's own error code. */
    w("sblaunch: "); w(bundle_id); w(" -> "); wd((unsigned)r); w("\n");
    _exit(r ? 1 : 0);
    return 0;
}
