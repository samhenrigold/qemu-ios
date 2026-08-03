/*
 * pbprobe -- can the guest's general pasteboard be written from a process that
 * is not the app that will read it?
 *
 * On iOS 3.1.3 the pasteboard is NOT per-process state and NOT a plain file the
 * host could edit: /System/Library/LaunchDaemons/com.apple.UIKit.pasteboardd.plist
 * launches /System/Library/Frameworks/UIKit.framework/Support/pasteboardd on
 * demand (MachService com.apple.UIKit.pasteboardd, UserName mobile), and that
 * daemon owns the live state. It persists to
 * ~/Library/Caches/com.apple.UIKit.pboard/pasteboardDB, but only as its own
 * backing store -- a process that wants the running system to see new content
 * has to talk to the daemon, and UIPasteboard is the only client of it.
 *
 * So this probe does the smallest possible version of the real thing: dlopen
 * UIKit, -[UIPasteboard generalPasteboard], setString:, and read it straight
 * back. If the read-back matches, then any process can seed the pasteboard and
 * the whole host-clipboard route is open.
 *
 * Plain C with a dlopen'd ObjC runtime, for the reason in
 * contrib/it-gles/glapp.c: `ld -lobjc` against the 3.1.3 SDK is fatal.
 *
 * Build: contrib/it-pasteboard/build.sh
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

#define RTLD_NOW 2

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(1, s, slen(s)); }

typedef void *id_;
typedef void *SEL_;

static id_ (*p_objc_getClass)(const char *);
static SEL_ (*p_sel_registerName)(const char *);
static id_ (*m0)(id_, SEL_);
static id_ (*m1)(id_, SEL_, id_);
static id_ (*m1s)(id_, SEL_, const char *);
static long long (*m0ll)(id_, SEL_);

#define S(n) (p_sel_registerName(n))
#define C(n) (p_objc_getClass(n))

int main(void)
{
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    void *found = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
    void *uikit = dlopen("/System/Library/Frameworks/UIKit.framework/UIKit", RTLD_NOW);

    if (!objc || !found || !uikit) {
        w("pbprobe: dlopen failed\n");
        _exit(1);
    }

    p_objc_getClass = (id_ (*)(const char *))dlsym(objc, "objc_getClass");
    p_sel_registerName = (SEL_ (*)(const char *))dlsym(objc, "sel_registerName");
    void *msg = dlsym(objc, "objc_msgSend");
    m0 = (id_ (*)(id_, SEL_))msg;
    m1 = (id_ (*)(id_, SEL_, id_))msg;
    m1s = (id_ (*)(id_, SEL_, const char *))msg;
    m0ll = (long long (*)(id_, SEL_))msg;

    id_ pool = m0(m0(C("NSAutoreleasePool"), S("alloc")), S("init"));
    (void)pool;

    id_ cls = C("UIPasteboard");
    if (!cls) {
        w("pbprobe: no UIPasteboard class\n");
        _exit(1);
    }

    id_ pb = m0(cls, S("generalPasteboard"));
    if (!pb) {
        w("pbprobe: generalPasteboard returned nil\n");
        _exit(1);
    }
    w("pbprobe: got generalPasteboard\n");

    /* Deliberately contains the three things the synthesised-tap path gets
     * wrong: a period, a space, and characters off the numbers/symbols page. */
    const char *text = "Hello. World 42 #tag";
    id_ s = m1s(C("NSString"), S("stringWithUTF8String:"), text);
    m1(pb, S("setString:"), s);
    w("pbprobe: setString: returned\n");

    id_ back = m0(pb, S("string"));
    if (!back) {
        w("pbprobe: read back nil\nRESULT=FAIL\n");
        _exit(1);
    }
    const char *utf8 = (const char *)m0(back, S("UTF8String"));
    w("pbprobe: read back [");
    w(utf8);
    w("]\n");

    unsigned i = 0;
    while (text[i] && utf8[i] && text[i] == utf8[i]) i++;
    if (text[i] == 0 && utf8[i] == 0) {
        w("RESULT=PASTEBOARD_WRITABLE_FROM_ANY_PROCESS\n");
    } else {
        w("RESULT=MISMATCH\n");
    }
    _exit(0);
    return 0;
}
