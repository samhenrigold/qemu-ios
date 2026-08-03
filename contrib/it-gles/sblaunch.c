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
 * Build with contrib/it-gles/build.sh; run on the guest over ssh:
 *     sblaunch com.qemuios.gltest
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2

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
    /* No crt1 (LC_UNIXTHREAD), so there is no argv to read. The identifier is
     * baked in; this tool exists for one app. */
    static const char bundle_id[] = "com.qemuios.gltest";
    void *cf, *sbs;
    void *(*p_CFStringCreateWithCString)(void *, const char *, unsigned);
    int (*p_SBSLaunchApplicationWithIdentifier)(void *, int);
    void *str;
    int r;

    cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/"
                "CoreFoundation", RTLD_NOW);
    sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices."
                 "framework/SpringBoardServices", RTLD_NOW);
    if (!cf || !sbs) { w("sblaunch: dlopen failed\n"); _exit(1); }

    p_CFStringCreateWithCString = dlsym(cf, "CFStringCreateWithCString");
    p_SBSLaunchApplicationWithIdentifier =
        dlsym(sbs, "SBSLaunchApplicationWithIdentifier");
    if (!p_CFStringCreateWithCString || !p_SBSLaunchApplicationWithIdentifier) {
        w("sblaunch: missing symbols\n");
        _exit(1);
    }

    str = p_CFStringCreateWithCString(0, bundle_id, kCFStringEncodingUTF8);
    r = p_SBSLaunchApplicationWithIdentifier(str, 0);
    /* 0 is success; anything else is SpringBoard's own error code. */
    w("sblaunch: "); w(bundle_id); w(" -> "); wd((unsigned)r); w("\n");
    _exit(r ? 1 : 0);
    return 0;
}
