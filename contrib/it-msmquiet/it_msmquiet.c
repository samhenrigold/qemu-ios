/*
 * it_msmquiet: hide MobileStorageMounter's "The attached USB device is not
 * supported." notice, and nothing else.
 *
 * With an emulated USB keyboard on the EHCI root port, MobileStorageMounter
 * (com.apple.mobile.storage_mounter) finds a USB device that is not a camera
 * or card reader and puts up its UNSUPPORTED_FAILURE notice on every boot.
 * While that alert is up SpringBoard will not lock: Hold blanks the display
 * and lights it again at once. The keyboard itself works either way.
 *
 * Loaded into that one launchd job with DYLD_INSERT_LIBRARIES and bound by
 * symbol through dyld interposing, so no binary is patched and nothing
 * depends on a particular build's addresses. The notice is recognised by its
 * text: the UNSUPPORTED_FAILURE key itself (if the caller lets CF localise)
 * or the string MobileStorageMounter's own bundle localises that key to, in
 * whatever language is set. Every other notice (MOUNT_FAILURE,
 * VERIFICATION_FAILURE, camera import) goes through untouched.
 */

/*
 * The few CoreFoundation declarations this needs, spelled out: the guest
 * toolchain builds with -nostdinc, and the iOS SDK's CF header omits
 * CFUserNotification anyway (CoreFoundation exports it on 3.2).
 */
typedef const struct __CFString *CFStringRef;
typedef const void *CFTypeRef;
typedef struct __CFBundle *CFBundleRef;
typedef const struct __CFURL *CFURLRef;
typedef double CFTimeInterval;
typedef unsigned long CFOptionFlags;
typedef int SInt32;
typedef unsigned char Boolean;
#define CFSTR(s) ((CFStringRef)__builtin___CFStringMakeConstantString(s))
#define NULL ((void *)0)
#define true 1
#define false 0

extern Boolean CFEqual(CFTypeRef a, CFTypeRef b);
extern CFBundleRef CFBundleGetMainBundle(void);
extern CFStringRef CFBundleCopyLocalizedString(CFBundleRef bundle, CFStringRef key,
                                               CFStringRef value, CFStringRef table);
extern int open(const char *path, int flags, ...);
extern long write(int fd, const void *buf, unsigned long n);
extern int close(int fd);

extern SInt32 CFUserNotificationDisplayNotice(CFTimeInterval timeout,
    CFOptionFlags flags, CFURLRef iconURL, CFURLRef soundURL,
    CFURLRef localizationURL, CFStringRef alertHeader,
    CFStringRef alertMessage, CFStringRef defaultButtonTitle);

#define kKey CFSTR("UNSUPPORTED_FAILURE")

static Boolean is_unsupported(CFStringRef s)
{
    static CFStringRef localized;
    Boolean hit;

    if (!s) {
        return false;
    }
    if (CFEqual(s, kKey)) {
        return true;
    }
    if (!localized) {
        CFBundleRef b = CFBundleGetMainBundle();
        localized = b ? CFBundleCopyLocalizedString(b, kKey, NULL, NULL) : NULL;
    }
    hit = localized && !CFEqual(localized, kKey) && CFEqual(s, localized);
    return hit;
}

static SInt32 quiet_DisplayNotice(CFTimeInterval timeout, CFOptionFlags flags,
                                  CFURLRef iconURL, CFURLRef soundURL,
                                  CFURLRef localizationURL, CFStringRef header,
                                  CFStringRef message, CFStringRef button)
{
    if (is_unsupported(header) || is_unsupported(message)) {
        static const char note[] = "it_msmquiet: hid the USB \"not supported\" notice\n";
        int fd = open("/dev/console", 1 /* O_WRONLY */);

        if (fd >= 0) {          /* one line on the serial console, for tests */
            write(fd, note, sizeof(note) - 1);
            close(fd);
        }
        return 0;
    }
    return CFUserNotificationDisplayNotice(timeout, flags, iconURL, soundURL,
                                           localizationURL, header, message,
                                           button);
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } interposers[] = {
    { (const void *)quiet_DisplayNotice, (const void *)CFUserNotificationDisplayNotice },
};
