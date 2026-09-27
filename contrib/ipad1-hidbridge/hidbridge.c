/*
 * hidbridge: a hardware keyboard for iOS 3.2.2 (iPad 1) fed over TCP.
 *
 * Creates an IOHIDUserDevice with a boot-keyboard report descriptor (the same
 * IOHIDResource path BTServer and iapd use for BT and dock keyboards) and
 * passes every 8-byte boot report read from a TCP client to
 * IOHIDUserDeviceHandleReport. See docs/ipad1/keyboard-and-network.md §1 for
 * the wire format.
 *
 * 3.2's dyld can't load a modern ld64 executable (LC_MAIN), so this is a dylib
 * whose constructor never returns, loaded into any Apple binary through
 * DYLD_INSERT_LIBRARIES (see the LaunchDaemon plist). CF and IOKit come from
 * dlsym because the 3.2 SDK stubs don't link under modern ld64.
 */
#include <dlfcn.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 5213

typedef const void *CFTypeRef;
typedef long CFIndex;

static const uint8_t descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,             /* Generic Desktop, Keyboard, Application */
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,             /* 8 modifier bits */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             /* reserved byte */
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,             /* 5 LEDs + pad (output) */
    0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x95, 0x06, 0x75, 0x08, 0x81, 0x00,             /* 6 key slots */
    0xc0,
};

static CFTypeRef (*CFDataCreate)(CFTypeRef, const uint8_t *, CFIndex);
static CFTypeRef (*CFNumberCreate)(CFTypeRef, int, const void *);
static CFTypeRef (*CFStringCreateWithCString)(CFTypeRef, const char *, uint32_t);
static void *(*CFDictionaryCreateMutable)(CFTypeRef, CFIndex, const void *, const void *);
static void (*CFDictionarySetValue)(void *, const void *, const void *);
static CFTypeRef (*IOHIDUserDeviceCreate)(CFTypeRef, void *);
static int (*IOHIDUserDeviceHandleReport)(CFTypeRef, const uint8_t *, CFIndex);

static void set(void *d, const char *k, CFTypeRef v)
{
    CFDictionarySetValue(d, CFStringCreateWithCString(0, k, 0x08000100), v);
}

static void setnum(void *d, const char *k, int n)
{
    set(d, k, CFNumberCreate(0, 9 /* kCFNumberIntType */, &n));
}

static int readn(int fd, uint8_t *p, size_t n)
{
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r, n -= r;
    }
    return 0;
}

#define S(h, n) (*(void **)&n = dlsym(h, #n))
__attribute__((constructor)) static void hidbridge(void)
{
    setvbuf(stderr, 0, _IONBF, 0);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    if (!cf || !io || !S(cf, CFDataCreate) || !S(cf, CFNumberCreate) || !S(cf, CFStringCreateWithCString) ||
        !S(cf, CFDictionaryCreateMutable) || !S(cf, CFDictionarySetValue) ||
        !S(io, IOHIDUserDeviceCreate) || !S(io, IOHIDUserDeviceHandleReport)) {
        fprintf(stderr, "hidbridge: dlsym failed: %s\n", dlerror());
        _exit(1);
    }
    void *props = CFDictionaryCreateMutable(0, 0, dlsym(cf, "kCFTypeDictionaryKeyCallBacks"),
                                            dlsym(cf, "kCFTypeDictionaryValueCallBacks"));
    set(props, "ReportDescriptor", CFDataCreate(0, descriptor, sizeof descriptor));
    setnum(props, "VendorID", 0x05ac);
    setnum(props, "ProductID", 0x0220);
    setnum(props, "PrimaryUsagePage", 1);
    setnum(props, "PrimaryUsage", 6);
    set(props, "Transport", CFStringCreateWithCString(0, "Virtual", 0x08000100));
    set(props, "Product", CFStringCreateWithCString(0, "hidbridge keyboard", 0x08000100));
    CFTypeRef dev = IOHIDUserDeviceCreate(0, props);
    if (!dev) {
        fprintf(stderr, "hidbridge: IOHIDUserDeviceCreate failed\n");
        _exit(1);
    }

    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = { .sin_len = sizeof a, .sin_family = AF_INET, .sin_port = htons(PORT) };
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (void *)&a, sizeof a) || listen(s, 1)) {
        perror("hidbridge: bind");
        _exit(1);
    }
    fprintf(stderr, "hidbridge: keyboard up, listening on %d\n", PORT);
    for (;;) {
        int c = accept(s, 0, 0);
        if (c < 0)
            continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        uint8_t r[8];
        while (!readn(c, r, 8)) {
            int e = IOHIDUserDeviceHandleReport(dev, r, 8);
            if (getenv("HIDBRIDGE_TRACE") || e)
                fprintf(stderr, "hidbridge: %02x %02x %02x %02x %02x %02x %02x %02x -> %#x\n",
                        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], e);
        }
        memset(r, 0, 8);                        /* release everything if the host goes away mid-press */
        IOHIDUserDeviceHandleReport(dev, r, 8);
        close(c);
    }
}
