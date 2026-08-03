/* pbset: put the contents of /tmp/pbtext on the general pasteboard, with an
 * explicit UTI. No reads -- pasteboardd crashes on an empty type string and the
 * reads are what send one. */
extern long write(int, const void *, unsigned long);
extern long read(int, void *, unsigned long);
extern int open(const char *, int, ...);
extern int close(int);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2
#define O_RDONLY 0
static unsigned slen(const char *s){unsigned n=0;while(s&&s[n])n++;return n;}
static void w(const char *s){write(1,s,slen(s));}
typedef void *id_; typedef void *SEL_;
static id_ (*p_getClass)(const char *);
static SEL_ (*p_sel)(const char *);
static id_ (*m0)(id_, SEL_);
static id_ (*m1s)(id_, SEL_, const char *);
static id_ (*m2)(id_, SEL_, id_, id_);
#define S(n) p_sel(n)
#define C(n) p_getClass(n)
int main(void)
{
    static char buf[4096];
    int fd = open("/tmp/pbtext", O_RDONLY);
    if (fd < 0) { w("pbset: no /tmp/pbtext\n"); _exit(1); }
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) n = 0;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) n--;
    buf[n] = 0;

    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
    void *uikit = dlopen("/System/Library/Frameworks/UIKit.framework/UIKit", RTLD_NOW);
    if (!objc || !uikit) { w("pbset: dlopen failed\n"); _exit(1); }
    p_getClass = (id_(*)(const char *))dlsym(objc, "objc_getClass");
    p_sel = (SEL_(*)(const char *))dlsym(objc, "sel_registerName");
    void *msg = dlsym(objc, "objc_msgSend");
    m0 = (id_(*)(id_, SEL_))msg;
    m1s = (id_(*)(id_, SEL_, const char *))msg;
    m2 = (id_(*)(id_, SEL_, id_, id_))msg;
    id_ pool = m0(m0(C("NSAutoreleasePool"), S("alloc")), S("init"));
    (void)pool;
    id_ pb = m0(C("UIPasteboard"), S("generalPasteboard"));
    if (!pb) { w("pbset: nil pasteboard\n"); _exit(1); }
    id_ s = m1s(C("NSString"), S("stringWithUTF8String:"), buf);
    id_ t = m1s(C("NSString"), S("stringWithUTF8String:"), "public.utf8-plain-text");
    m2(pb, S("setValue:forPasteboardType:"), s, t);
    w("pbset: set ["); w(buf); w("]\n");
    _exit(0);
    return 0;
}
