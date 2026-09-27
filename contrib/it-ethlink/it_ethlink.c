/*
 * it_ethlink -- raise the USB Ethernet link when a host bridges it.
 *
 * AppleUSBEthernetDevice starts its output queue and arms its first bulk read
 * only in setProperties({"LinkStatus": 1}). On a tethering iPhone configd's
 * USBEthernetSharing makes that call; a Wi-Fi iPad has no tethering, so
 * nothing ever does and en1 stays "Link Active: FALSE". This daemon does what
 * USBEthernetSharing would: watch the service and, whenever the kext reports
 * an interface change (the host selecting the Ethernet interface's alt 1 is
 * one), set LinkStatus 0 then 1 so it runs the whole link-up path again.
 * Stable IOKit API and property names only: no offsets, no patches, so it
 * works on any IPSW whose kext takes LinkStatus. docs/ipad1/guest-services.md.
 *
 * Plain C with IOKit and CoreFoundation dlopen'd (the old SDK stubs do not
 * link under a modern ld); built by contrib/ipad1-guest/build.sh.
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

#define RTLD_NOW 2

typedef unsigned int io_object_t;
typedef const void *CFTypeRef;

static void *(*IOServiceMatching_)(const char *);
static void *(*IONotificationPortCreate_)(unsigned);
static void *(*IONotificationPortGetRunLoopSource_)(void *);
static int (*IOServiceAddMatchingNotification_)(void *, const char *, void *,
                                               void (*)(void *, io_object_t),
                                               void *, io_object_t *);
static int (*IOServiceAddInterestNotification_)(void *, io_object_t, const char *,
                                               void (*)(void *, io_object_t, unsigned, void *),
                                               void *, io_object_t *);
static io_object_t (*IOIteratorNext_)(io_object_t);
static int (*IORegistryEntrySetCFProperty_)(io_object_t, CFTypeRef, CFTypeRef);
static CFTypeRef (*CFStringCreateWithCString_)(CFTypeRef, const char *, unsigned);
static CFTypeRef (*CFNumberCreate_)(CFTypeRef, int, const void *);
static void *(*CFRunLoopGetCurrent_)(void);
static void (*CFRunLoopAddSource_)(void *, void *, CFTypeRef);
static void (*CFRunLoopRun_)(void);
static CFTypeRef *kCFRunLoopDefaultMode_;

static CFTypeRef s_link_status, s_zero, s_one;
static void *s_port;

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void w(const char *s) { write(2, s, slen(s)); }

static void w_hex(unsigned v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) {
        b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 0xf];
    }
    b[10] = 0;
    w(b);
}

/* Run the kext's link-up path from the top: it ignores a repeat of the value
 * it already holds, and only starts the queue if alt 1 is selected now. */
static void kick(io_object_t svc)
{
    int down = IORegistryEntrySetCFProperty_(svc, s_link_status, s_zero);
    int up = IORegistryEntrySetCFProperty_(svc, s_link_status, s_one);
    w("it_ethlink: LinkStatus 0 -> 1 (");
    w_hex(down);
    w(", ");
    w_hex(up);
    w(")\n");
}

/*
 * The kext messages its clients on every interface change: the alt-setting
 * switch the host makes when it bridges Ethernet (0xe3ff8201 on 7B500) among
 * them. Rather than decode which, re-run the link-up on each; with alt 0 the
 * kext just records the value, and the next message after alt 1 starts it.
 */
static void interest(void *refcon, io_object_t svc, unsigned type, void *arg)
{
    w("it_ethlink: message ");
    w_hex(type);
    w("\n");
    kick((io_object_t)(long)refcon);
}

static void matched(void *refcon, io_object_t iter)
{
    io_object_t svc, note;

    while ((svc = IOIteratorNext_(iter))) {
        IOServiceAddInterestNotification_(s_port, svc, "IOGeneralInterest",
                                          interest, (void *)(long)svc, &note);
        w("it_ethlink: watching AppleUSBEthernetDevice\n");
    }
}

#define SYM(h, name) if (!(name##_ = dlsym(h, #name))) { w("it_ethlink: no " #name "\n"); _exit(1); }

int main(void)
{
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    io_object_t iter;
    int one = 1, zero = 0;

    if (!io || !cf) {
        w("it_ethlink: cannot load IOKit/CoreFoundation\n");
        _exit(1);
    }
    SYM(io, IOServiceMatching) SYM(io, IONotificationPortCreate)
    SYM(io, IONotificationPortGetRunLoopSource) SYM(io, IOServiceAddMatchingNotification)
    SYM(io, IOServiceAddInterestNotification) SYM(io, IOIteratorNext)
    SYM(io, IORegistryEntrySetCFProperty)
    SYM(cf, CFStringCreateWithCString) SYM(cf, CFNumberCreate)
    SYM(cf, CFRunLoopGetCurrent) SYM(cf, CFRunLoopAddSource)
    SYM(cf, CFRunLoopRun) SYM(cf, kCFRunLoopDefaultMode)

    s_link_status = CFStringCreateWithCString_(0, "LinkStatus", 0x08000100);
    s_zero = CFNumberCreate_(0, 9 /* kCFNumberIntType */, &zero);
    s_one = CFNumberCreate_(0, 9, &one);

    s_port = IONotificationPortCreate_(0);
    CFRunLoopAddSource_(CFRunLoopGetCurrent_(), IONotificationPortGetRunLoopSource_(s_port),
                        *kCFRunLoopDefaultMode_);
    if (IOServiceAddMatchingNotification_(s_port, "IOServiceFirstMatch",
                                          IOServiceMatching_("AppleUSBEthernetDevice"),
                                          matched, 0, &iter)) {
        w("it_ethlink: cannot register for AppleUSBEthernetDevice\n");
        _exit(1);
    }
    w("it_ethlink: up\n");
    matched(0, iter);           /* arms the notification and takes what exists */
    CFRunLoopRun_();
    _exit(0);
    return 0;
}
