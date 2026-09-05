/* Period-correct SpringBoardServices ABI, shared by launch and status RPCs. */
static int agent_sbs(const char *op, const char *args)
{
    static void *cf, *sbs;
    if (!cf) cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    if (!sbs) sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", 2);
    if (!cf || !sbs) return -ENOSYS;
    void (*release)(void *) = dlsym(cf, "CFRelease");
    if (!release) return -ENOSYS;
    if (!strcmp(op, "lockstatus")) {
        unsigned (*port)(void) = dlsym(sbs, "SBSSpringBoardServerPort");
        int (*status)(unsigned, unsigned char *, unsigned char *) = dlsym(sbs, "SBGetScreenLockStatus");
        unsigned char locked = 0, passcode = 0;
        if (!port || !status) return -ENOSYS;
        if (status(port(), &locked, &passcode)) return -EIO;
        ag_response_len = snprintf((char *)ag_response, AG_RESPONSE_MAX,
                                  "locked=%u passcode=%u\n", locked, passcode);
        return 0;
    }
    if (!strcmp(op, "frontmost")) {
        void *(*frontmost)(void) = dlsym(sbs, "SBSCopyFrontmostApplicationDisplayIdentifier");
        void *(*name)(void *) = dlsym(sbs, "SBSCopyLocalizedApplicationNameForDisplayIdentifier");
        unsigned char (*string)(void *, char *, long, unsigned) = dlsym(cf, "CFStringGetCString");
        if (!frontmost || !name || !string) return -ENOSYS;
        void *identifier = frontmost();
        char bundle[1024] = "com.apple.springboard", title[1024] = "Home Screen";
        int status = 0;
        if (identifier) {
            void *label = name(identifier);
            if (!string(identifier, bundle, sizeof(bundle), 0x08000100) ||
                !string(label ? label : identifier, title, sizeof(title), 0x08000100)) status = -EIO;
            if (label) release(label);
            release(identifier);
        }
        if (!status) ag_response_len = snprintf((char *)ag_response, AG_RESPONSE_MAX, "%s\n%s\n", bundle, title);
        return status;
    }
    void *(*create)(void *, const char *, unsigned) = dlsym(cf, "CFStringCreateWithCString");
    int (*launch)(void *, int) = dlsym(sbs, "SBSLaunchApplicationWithIdentifier");
    if (!create || !launch) return -ENOSYS;
    if (!*args || strlen(args) > 1024) return -EINVAL;
    void *identifier = create(0, args, 0x08000100);
    if (!identifier) return -EINVAL;
    int status = launch(identifier, 0);
    release(identifier);
    return status;
}
