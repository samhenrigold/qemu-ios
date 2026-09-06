/* Period-correct SpringBoardServices ABI, shared by launch and status RPCs. */
static int agent_sbs_inner(const char *op, const char *args)
{
    static void *cf, *sbs;
    if (!cf) cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    if (!sbs) sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", 2);
    if (!cf || !sbs) return -ENOSYS;
    void (*release)(void *) = dlsym(cf, "CFRelease");
    if (!release) return -ENOSYS;
    unsigned (*port)(void) = dlsym(sbs, "SBSSpringBoardServerPort");
    int (*lock)(unsigned, unsigned char *, unsigned char *) = dlsym(sbs, "SBGetScreenLockStatus");
    unsigned char locked = 0, passcode = 0;
    if (!port || !lock || lock(port(), &locked, &passcode)) return -EIO;
    if (locked && (!strcmp(op, "type") || !strcmp(op, "backspace"))) return -EACCES;
    if (locked && !strcmp(op, "frontmost")) {
        const char *screen = "com.apple.springboard\nLock Screen\n";
        ag_response_len = strlen(screen);
        memcpy(ag_response, screen, ag_response_len);
        return 0;
    }
    if (!strcmp(op, "type") || !strcmp(op, "backspace") || !strcmp(op, "uidump")) {
        void *(*frontmost)(void) = dlsym(sbs, "SBSCopyFrontmostApplicationDisplayIdentifier");
        int (*process)(void *, int *) = dlsym(sbs, "SBSProcessIDForDisplayIdentifier");
        if (!frontmost || !process) return -ENOSYS;
        void *identifier = locked ? 0 : frontmost();
        /* Target zero names SpringBoard, which is not returned as a foreground
         * application display identifier (Spotlight and the lock screen). */
        if (!identifier) return qc(0x16a, 0, 0, 0) < 0 ? -EIO : 0;
        int pid = 0;
        int found = process(identifier, &pid);
        release(identifier);
        if (!found || pid <= 1) return -ESRCH;
        return qc(0x16a, 0, pid, 0) < 0 ? -EIO : 0;
    }
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

static int agent_sbs(const char *op, const char *args)
{
    static void *objc, *foundation;
    if (!objc) objc = dlopen("/usr/lib/libobjc.A.dylib", 2);
    if (!foundation) foundation = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", 2);
    if (!objc || !foundation) return -ENOSYS;
    void *(*get)(const char *) = dlsym(objc, "objc_getClass");
    void *(*selector)(const char *) = dlsym(objc, "sel_registerName");
    void *(*send)(void *, void *) = dlsym(objc, "objc_msgSend");
    if (!get || !selector || !send) return -ENOSYS;
    void *pool = send(send(get("NSAutoreleasePool"), selector("alloc")), selector("init"));
    if (!pool) return -ENOSYS;
    int status = agent_sbs_inner(op, args);
    send(pool, selector("release"));
    return status;
}
