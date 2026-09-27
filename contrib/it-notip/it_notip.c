/*
 * it_notip -- mark SpringBoard's first-run "Edit Home Screen" tip as already shown.
 *
 * SpringBoard shows REORDER_INFO ("Edit Home Screen") once, and records that it did
 * in its own preferences domain (com.apple.springboard) under SBDidShowReorderText.
 * Every emulated device is a first run, so the tip covered the home screen on each
 * fresh clone. This one-shot job, run as mobile, sets that key through the stock
 * CFPreferences API before the first unlock. IPSW-agnostic: the key name is
 * confirmed in the installed SpringBoard binary first; if an IPSW does not carry
 * it, the job logs that and changes nothing.
 *
 * Plain C with CoreFoundation dlopen'd, like it_ethlink; built by
 * contrib/ipad1-guest/build.sh, baked by imgtools/ipad1_rootfs.py bake.
 */
extern long write(int, const void *, unsigned long);
extern long read(int, void *, unsigned long);
extern int open(const char *, int, ...);
extern int close(int);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

#define KEY "SBDidShowReorderText"
#define SPRINGBOARD "/System/Library/CoreServices/SpringBoard.app/SpringBoard"

static void say(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    write(2, "it_notip: ", 10);
    write(2, s, p - s);
    write(2, "\n", 1);
}

/* Is KEY (with its terminating NUL) somewhere in SpringBoard's executable? */
static int springboard_has_key(void)
{
    static char buf[1 << 16];
    const int klen = sizeof(KEY);   /* includes the NUL */
    int fd = open(SPRINGBOARD, 0), keep = 0, found = 0;
    long n;
    if (fd < 0)
        return 0;
    while (!found && (n = read(fd, buf + keep, sizeof(buf) - keep)) > 0) {
        long end = keep + n, i;
        for (i = 0; i + klen <= end; i++) {
            int j = 0;
            while (j < klen && buf[i + j] == KEY[j])
                j++;
            if (j == klen) {
                found = 1;
                break;
            }
        }
        keep = end < klen ? end : klen - 1;   /* carry the tail across reads */
        for (i = 0; i < keep; i++)
            buf[i] = buf[end - keep + i];
    }
    close(fd);
    return found;
}

int main(void)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    const void *(*str)(const void *, const char *, unsigned) = cf ? dlsym(cf, "CFStringCreateWithCString") : 0;
    void (*set)(const void *, const void *, const void *) = cf ? dlsym(cf, "CFPreferencesSetAppValue") : 0;
    unsigned char (*sync)(const void *) = cf ? dlsym(cf, "CFPreferencesAppSynchronize") : 0;
    const void **yes = cf ? dlsym(cf, "kCFBooleanTrue") : 0;

    if (!str || !set || !sync || !yes) {
        say("CoreFoundation preferences API not found; leaving the tip alone");
        _exit(0);
    }
    if (!springboard_has_key()) {
        say("SpringBoard has no " KEY "; leaving the tip alone");
        _exit(0);
    }
    const void *app = str(0, "com.apple.springboard", 0x08000100);
    set(str(0, KEY, 0x08000100), *yes, app);
    say(sync(app) ? KEY " set" : "CFPreferencesAppSynchronize failed");
    _exit(0);
    return 0;
}
