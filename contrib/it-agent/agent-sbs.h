#include <mach/mach.h>
#include <mach/ndr.h>

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
    if (!port) return -ENOSYS;
    if (!strcmp(op, "orientation")) {
        int (*orientation)(unsigned, int *) = dlsym(sbs, "SBGetUIOrientation");
        if (!orientation) return -ENOSYS;
        int degrees = 0;
        unsigned current = port(); // A respring replaces SpringBoard's server port.
        if (!current) return -EIO;
        /* 7E18 SBGetUIOrientation uses an unbounded mach_msg (options=3).
         * Keep its verified MIG wire ABI, but own a reply port and bound both
         * waits. Reject other firmware stubs instead of guessing their ABI. */
        static const unsigned char signature[] = {0xf0,0xb5,0x03,0xaf,0x8f,0xb0,0x34,0x4b};
        if (memcmp((void *)((uintptr_t)orientation & ~(uintptr_t)1), signature, sizeof(signature))) return -ENOSYS;
        struct {
            mach_msg_header_t header;
            NDR_record_t ndr;
            kern_return_t status;
            int degrees;
            mach_msg_trailer_t trailer;
        } reply = {0};
        mach_port_t receive = MACH_PORT_NULL;
        if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &receive)) return -EIO;
        reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
        reply.header.msgh_remote_port = current;
        reply.header.msgh_local_port = receive;
        reply.header.msgh_id = 0x1e8496;
        kern_return_t result = mach_msg(&reply.header,
            MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT,
            sizeof(mach_msg_header_t), sizeof(reply), receive, 250, MACH_PORT_NULL);
        mach_port_destroy(mach_task_self(), receive);
        if (result) return result == MACH_RCV_TIMED_OUT || result == MACH_SEND_TIMED_OUT ? -ETIMEDOUT : -EIO;
        if (reply.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
            mach_msg_destroy(&reply.header);
            return -EIO;
        }
        if (reply.header.msgh_id != 0x1e8496 + 100 || reply.header.msgh_size != 40 ||
            reply.ndr.int_rep != NDR_record.int_rep || reply.status) return -EIO;
        degrees = reply.degrees;
        if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != -90) return -ERANGE;
        ag_response_len = snprintf((char *)ag_response, AG_RESPONSE_MAX, "%d\n", degrees);
        return 0;
    }
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
