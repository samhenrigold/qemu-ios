#ifndef IT_SBS_LAUNCH_H
#define IT_SBS_LAUNCH_H
#include <errno.h>
#include <string.h>

/* Ask stock SpringBoard to launch; never exec a UIKit application directly.
 * 5F138 exports SBLaunchApplication(port, suspend, UTF8 identifier), whose
 * stock MIG stub carries a byte followed by a bounded 1024-byte string.
 * Later firmware exposes SBSLaunchApplicationWithIdentifier(CFString, bool).
 * Select the exported API, not a firmware version or an instruction address.
 */
static int it_sbs_launch(void *cf, void *sbs, const char *bundle)
{
    if (!*bundle || strlen(bundle) >= 1024) return -EINVAL;
    int (*launch)(void *, int) = dlsym(sbs, "SBSLaunchApplicationWithIdentifier");
    if (launch) {
        void *(*create)(void *, const char *, unsigned) = dlsym(cf, "CFStringCreateWithCString");
        void (*release)(void *) = dlsym(cf, "CFRelease");
        if (!create || !release) return -ENOSYS;
        void *identifier = create(0, bundle, 0x08000100);
        if (!identifier) return -EINVAL;
        int status = launch(identifier, 0);
        release(identifier);
        return status;
    }
    unsigned (*port)(void) = dlsym(sbs, "SBSSpringBoardServerPort");
    int (*legacy)(unsigned, unsigned char, const char *) = dlsym(sbs, "SBLaunchApplication");
    if (!port || !legacy) return -ENOSYS;
    unsigned server = port();
    return server ? legacy(server, 0, bundle) : -EIO;
}
#endif
