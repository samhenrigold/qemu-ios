/*
 * it_msmquiet: hide MobileStorageMounter's "USB device is not supported"
 * notice, and nothing else.
 *
 * With an emulated USB keyboard on the EHCI root port, MobileStorageMounter
 * (com.apple.mobile.storage_mounter) finds a USB device that is not a camera
 * or card reader and puts up its unsupported-device notice on every boot.
 * While that alert is up SpringBoard will not lock: Hold blanks the display
 * and lights it again at once. The keyboard itself works either way.
 *
 * Loaded into that one launchd job with DYLD_INSERT_LIBRARIES and bound by
 * symbol through dyld interposing, so no binary is patched and nothing
 * depends on a particular build's addresses. One binary for every iOS: the
 * notice is recognized by its text, the key itself (if the caller lets CF
 * localize) or the string MobileStorageMounter's own bundle localizes that
 * key to, in whatever language is set. 3.2.x raises it with
 * CFUserNotificationDisplayNotice and the key UNSUPPORTED_FAILURE; 4.2.1
 * builds a CFUserNotificationCreate alert from UNSUPPORTED_FAILURE_TITLE and
 * UNSUPPORTED_FAILURE_BODY (the title, "Cannot Use Device", is also the
 * power-hungry notice's, so the body decides) and keeps it to cancel on
 * detach: a NULL from Create is a path it handles ("Could not create user
 * notification", then on). Every other notice (MOUNT_FAILURE,
 * VERIFICATION_FAILURE, POWER_HUNGRY_FAILURE, camera import) goes through.
 *
 * 5.x moved the notice out of the mounter into USBDeviceArbitrator
 * (com.apple.mobile.usb_device_arbitrator, LaunchBuddy's catch-all for an
 * IOUSBDevice no other plugin claims: 9B206 handle_start 0x242c "Assuming it
 * is unsupported"), with the 4.x keys and CFUserNotificationCreate from its
 * own bundle; the bake loads this into whichever of the two jobs raises it.
 *
 * 7.x (DeviceOMatic) names the device in front of the key's text ("QEMU USB
 * Keyboard: The connected device is not supported."), so a notice is also this
 * one when it ends with that text, or with what follows the text's last %@.
 */

/*
 * The few CoreFoundation declarations this needs, spelled out: the guest
 * toolchain builds with -nostdinc, and the iOS SDK's CF header omits
 * CFUserNotification anyway (CoreFoundation exports it on 3.2).
 */
typedef const struct __CFString *CFStringRef;
typedef const void *CFTypeRef;
typedef const void *CFAllocatorRef;
typedef struct __CFBundle *CFBundleRef;
typedef const struct __CFURL *CFURLRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFUserNotification *CFUserNotificationRef;
typedef double CFTimeInterval;
typedef unsigned long CFOptionFlags;
typedef int SInt32;
typedef unsigned char Boolean;
typedef struct { long location, length; } CFRange;
#define CFSTR(s) ((CFStringRef)__builtin___CFStringMakeConstantString(s))
#define NULL ((void *)0)
#define true 1
#define false 0

extern Boolean CFEqual(CFTypeRef a, CFTypeRef b);
extern void CFRelease(CFTypeRef cf);
extern unsigned long CFGetTypeID(CFTypeRef cf);
extern unsigned long CFStringGetTypeID(void);
extern long CFStringGetLength(CFStringRef s);
extern CFRange CFStringFind(CFStringRef s, CFStringRef find, CFOptionFlags options);
extern CFStringRef CFStringCreateWithSubstring(CFAllocatorRef allocator, CFStringRef s, CFRange range);
extern Boolean CFStringHasSuffix(CFStringRef s, CFStringRef suffix);
extern CFBundleRef CFBundleGetMainBundle(void);
extern CFStringRef CFBundleCopyLocalizedString(CFBundleRef bundle, CFStringRef key,
                                               CFStringRef value, CFStringRef table);
extern const void *CFDictionaryGetValue(CFDictionaryRef dict, const void *key);
extern const CFStringRef kCFUserNotificationAlertHeaderKey, kCFUserNotificationAlertMessageKey;
extern int open(const char *path, int flags, ...);
extern long write(int fd, const void *buf, unsigned long n);
extern int close(int fd);

extern SInt32 CFUserNotificationDisplayNotice(CFTimeInterval timeout,
    CFOptionFlags flags, CFURLRef iconURL, CFURLRef soundURL,
    CFURLRef localizationURL, CFStringRef alertHeader,
    CFStringRef alertMessage, CFStringRef defaultButtonTitle);
extern CFUserNotificationRef CFUserNotificationCreate(CFAllocatorRef allocator,
    CFTimeInterval timeout, CFOptionFlags flags, SInt32 *error,
    CFDictionaryRef dictionary);

/* Apple's own keys for this notice, by generation (3.2.x; 4.x). */
static const CFStringRef keys[] = { CFSTR("UNSUPPORTED_FAILURE"), CFSTR("UNSUPPORTED_FAILURE_BODY") };
#define NKEYS 2

/* Whether the notice text `s` says `text`, a key's localization: is it, or ends with it behind the device's name
 * (7.x), `text` itself or what follows its last %@ where it is the format the name goes into. A tail too short to
 * tell one notice from another by (under 12 characters) says nothing. */
static Boolean says(CFTypeRef s, CFStringRef text)
{
    CFRange name;
    long length, start;
    CFStringRef tail;
    Boolean ends;

    if (CFEqual(s, text)) {
        return true;
    }
    if (CFGetTypeID(s) != CFStringGetTypeID()) {
        return false;
    }
    length = CFStringGetLength(text);
    name = CFStringFind(text, CFSTR("%@"), 4 /* kCFCompareBackwards */);
    start = name.location < 0 ? 0 : name.location + 2;
    if (length - start < 12) {
        return false;
    }
    tail = CFStringCreateWithSubstring(NULL, text, (CFRange){ start, length - start });
    ends = tail && CFStringHasSuffix((CFStringRef)s, tail);
    if (tail) {
        CFRelease(tail);
    }
    return ends;
}

static Boolean is_unsupported(CFTypeRef s)
{
    static CFStringRef localized[NKEYS];
    static Boolean resolved;
    int i;

    if (!s) {
        return false;
    }
    if (!resolved) {
        CFBundleRef b = CFBundleGetMainBundle();
        for (i = 0; i < NKEYS; i++) {
            localized[i] = b ? CFBundleCopyLocalizedString(b, keys[i], NULL, NULL) : NULL;
        }
        resolved = true;
    }
    for (i = 0; i < NKEYS; i++) {
        if (CFEqual(s, keys[i])) {
            return true;
        }
        /* a key this bundle does not carry localizes to itself */
        if (localized[i] && !CFEqual(localized[i], keys[i]) && says(s, localized[i])) {
            return true;
        }
    }
    return false;
}

static void note(void)
{
    static const char line[] = "it_msmquiet: hid the USB \"not supported\" notice\n";
    int fd = open("/dev/console", 1 /* O_WRONLY */);

    if (fd >= 0) {          /* one line on the serial console, for tests */
        write(fd, line, sizeof(line) - 1);
        close(fd);
    }
}

static SInt32 quiet_DisplayNotice(CFTimeInterval timeout, CFOptionFlags flags,
                                  CFURLRef iconURL, CFURLRef soundURL,
                                  CFURLRef localizationURL, CFStringRef header,
                                  CFStringRef message, CFStringRef button)
{
    if (is_unsupported(header) || is_unsupported(message)) {
        note();
        return 0;
    }
    return CFUserNotificationDisplayNotice(timeout, flags, iconURL, soundURL,
                                           localizationURL, header, message,
                                           button);
}

static CFUserNotificationRef quiet_Create(CFAllocatorRef allocator, CFTimeInterval timeout,
                                          CFOptionFlags flags, SInt32 *error,
                                          CFDictionaryRef dictionary)
{
    if (dictionary &&
        (is_unsupported(CFDictionaryGetValue(dictionary, kCFUserNotificationAlertHeaderKey)) ||
         is_unsupported(CFDictionaryGetValue(dictionary, kCFUserNotificationAlertMessageKey)))) {
        note();
        if (error) {
            *error = 0;
        }
        return NULL;
    }
    return CFUserNotificationCreate(allocator, timeout, flags, error, dictionary);
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } interposers[] = {
    { (const void *)quiet_DisplayNotice, (const void *)CFUserNotificationDisplayNotice },
    { (const void *)quiet_Create, (const void *)CFUserNotificationCreate },
};
