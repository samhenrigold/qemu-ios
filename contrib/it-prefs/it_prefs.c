/*
 * it_prefs -- the guest preferences this machine needs, applied at boot.
 *
 * A one-shot job, started as root at boot, that writes each entry of SETTINGS
 * as mobile, through the stock CFPreferences API, into mobile's own
 * preferences, so nothing is seeded into the image's plists:
 *
 *   com.apple.springboard SBDidShowReorderText = true
 *       SpringBoard's first-run "Edit Home Screen" tip, which every emulated
 *       device would otherwise show on each fresh clone.
 *   com.apple.locationd AppleLocationServer = http://10.0.2.100:3128/clls/wloc,
 *                       AppleLocationServerRequiresCert = false
 *       Wi-Fi location (docs/ipad1/location.md): locationd ignores the PAC and
 *       asks Apple's location server directly, which no longer answers iOS 3;
 *       pointed at the web proxy's guestfwd address over plain HTTP, it gets a
 *       position from the host.
 *
 * Once Wi-Fi (en0) has an address it also restarts locationd, which otherwise
 * starts before Wi-Fi is powered and then never scans (see main()).
 *
 * IPSW-agnostic: each key name is confirmed in the binary that reads it first;
 * if an IPSW does not carry it, the job logs that and leaves that key alone.
 *
 * Plain C with CoreFoundation dlopen'd, like it_ethlink; built by
 * contrib/ipad1-guest/build.sh (the iPad's seed package), and for the iPod by
 * build-ipod.sh with IT_PREFS_TIP_ONLY (the SpringBoard key only), baked by
 * imgtools/ipod2g_device.py bake.
 */
extern long write(int, const void *, unsigned long);
extern long read(int, void *, unsigned long);
extern int open(const char *, int, ...);
extern int close(int);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int fork(void);
extern int execv(const char *, char *const[]);
extern int waitpid(int, int *, int);
extern int setgid(unsigned);
extern int setuid(unsigned);
extern int setenv(const char *, const char *, int);
extern int socket(int, int, int);
extern int ioctl(int, unsigned long, ...);
extern unsigned sleep(unsigned);

#define SPRINGBOARD "/System/Library/CoreServices/SpringBoard.app/SpringBoard"
#define LOCATIOND   "/usr/libexec/locationd"
#define LOCATIOND_JOB "/System/Library/LaunchDaemons/com.apple.locationd.plist"

enum kind { TRUE, FALSE, STRING };

static const struct setting {
    const char *domain, *key, *reader;
    enum kind kind;
    const char *string;
    /* The reader's launchd job, if it is already running by the time this job
     * does: locationd (root, OnDemand false) reads its server URL once at start
     * and rewrites its whole preferences file from memory, so it is unloaded
     * around the writes and loaded again to read the new values. */
    const char *job;
} SETTINGS[] = {
    { "com.apple.springboard", "SBDidShowReorderText", SPRINGBOARD, TRUE },
#ifndef IT_PREFS_TIP_ONLY   /* the iPod (build-ipod.sh): only the tip; no Wi-Fi location there */
    { "com.apple.locationd", "AppleLocationServer", LOCATIOND, STRING,
      "http://10.0.2.100:3128/clls/wloc", LOCATIOND_JOB },
    { "com.apple.locationd", "AppleLocationServerRequiresCert", LOCATIOND, FALSE,
      0, LOCATIOND_JOB },
#endif
};

static long len(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void say(const char *a, const char *b, const char *c)
{
    write(2, "it_prefs: ", 10);
    write(2, a, len(a));
    write(2, b, len(b));
    write(2, c, len(c));
    write(2, "\n", 1);
}

/* Is key (with its terminating NUL) somewhere in the file at path? */
static int file_has(const char *path, const char *key)
{
    static char buf[1 << 16];
    const long klen = len(key) + 1;   /* includes the NUL */
    int fd = open(path, 0), found = 0;
    long n, keep = 0;
    if (fd < 0)
        return 0;
    while (!found && (n = read(fd, buf + keep, sizeof(buf) - keep)) > 0) {
        long end = keep + n, i;
        for (i = 0; i + klen <= end; i++) {
            long j = 0;
            while (j < klen && buf[i + j] == key[j])
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

/* /bin/launchctl load|unload JOB; 0 on success. */
static int launchctl(const char *verb, const char *job)
{
    char *argv[] = { "launchctl", (char *)verb, (char *)job, 0 };
    int pid = fork(), status = -1;
    if (pid == 0) {
        execv("/bin/launchctl", argv);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return -1;
    return status;
}

/*
 * As mobile: apply every setting whose key the reader knows. With write false,
 * only compare. Returns a bit per job (index into jobs[]) whose settings are
 * not yet what they should be.
 */
static unsigned apply(int write, const char *const *jobs, unsigned njobs)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    const void *(*str)(const void *, const char *, unsigned) = cf ? dlsym(cf, "CFStringCreateWithCString") : 0;
    const void *(*get)(const void *, const void *) = cf ? dlsym(cf, "CFPreferencesCopyAppValue") : 0;
    unsigned char (*equal)(const void *, const void *) = cf ? dlsym(cf, "CFEqual") : 0;
    void (*set)(const void *, const void *, const void *) = cf ? dlsym(cf, "CFPreferencesSetAppValue") : 0;
    unsigned char (*sync)(const void *) = cf ? dlsym(cf, "CFPreferencesAppSynchronize") : 0;
    const void **yes = cf ? dlsym(cf, "kCFBooleanTrue") : 0;
    const void **no = cf ? dlsym(cf, "kCFBooleanFalse") : 0;
    unsigned i, j, stale = 0;

    if (!str || !get || !equal || !set || !sync || !yes || !no) {
        say("CoreFoundation preferences API not found; changing nothing", "", "");
        return 0;
    }
    for (i = 0; i < sizeof(SETTINGS) / sizeof(SETTINGS[0]); i++) {
        const struct setting *s = &SETTINGS[i];
        const void *app = str(0, s->domain, 0x08000100);   /* kCFStringEncodingUTF8 */
        const void *key = str(0, s->key, 0x08000100);
        const void *value = s->kind == TRUE ? *yes : s->kind == FALSE ? *no
                          : str(0, s->string, 0x08000100);
        const void *current;

        if (!file_has(s->reader, s->key)) {
            if (write)
                say(s->reader, " has no ", s->key);
            continue;
        }
        current = get(key, app);
        if (current && equal(current, value))
            continue;
        for (j = 0; j < njobs && jobs[j] != s->job; j++)
            ;
        stale |= 1u << j;           /* j == njobs: no job to restart */
        if (write) {
            set(key, value, app);
            say(s->key, sync(app) ? " set" : " not saved: CFPreferencesAppSynchronize failed", "");
        }
    }
    return stale;
}

/* Run apply() as mobile in a child; its result comes back as the exit code. */
static unsigned as_mobile(int write, const char *const *jobs, unsigned njobs)
{
    int pid = fork(), status = 0;
    if (pid == 0) {
        /* CFPreferences finds the user's preferences through HOME, which
         * is still root's in a child of a root job. */
        if (setgid(501) || setuid(501) || setenv("HOME", "/var/mobile", 1)) {   /* mobile */
            say("could not become mobile; changing nothing", "", "");
            _exit(0);
        }
        _exit(apply(write, jobs, njobs));
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return 0;
    return (status >> 8) & 0xff;
}

/* Wait up to secs seconds for en0 (Wi-Fi) to have an IPv4 address; 1 if it did. */
static int wifi_up(unsigned secs)
{
    struct { char name[16]; unsigned char addr[16]; } ifr = { "en0" };
    int fd = socket(2, 2, 0);   /* AF_INET, SOCK_DGRAM */

    while (fd >= 0) {
        if (ioctl(fd, 0xc0206921UL, &ifr) == 0) {   /* SIOCGIFADDR */
            close(fd);
            return 1;
        }
        if (!secs--)
            break;
        sleep(1);
    }
    if (fd >= 0)
        close(fd);
    return 0;
}

/*
 * locationd only scans for Wi-Fi if Wi-Fi was powered when it started. It
 * starts at boot, before configd powers the BCM4329 up, and on a fast
 * (sealed) boot it usually loses that race: no scan all session, so Maps gets
 * "Your location could not be determined" (docs/ipad1/location.md). So once
 * en0 has an address, locationd is restarted, and any setting that has to
 * change is written while it is down. Other jobs are restarted only when one
 * of their settings changes.
 */
int main(void)
{
    const char *jobs[sizeof(SETTINGS) / sizeof(SETTINGS[0])];
    unsigned i, j, n = 0, stale, stopped = 0, locationd = 0;

    for (i = 0; i < sizeof(SETTINGS) / sizeof(SETTINGS[0]); i++) {
        for (j = 0; j < n && jobs[j] != SETTINGS[i].job; j++)
            ;
        if (SETTINGS[i].job && j == n)
            jobs[n++] = SETTINGS[i].job;
    }
    for (j = 0; j < n && !streq(jobs[j], LOCATIOND_JOB); j++)
        ;
    if (j < n && wifi_up(120))
        locationd = 1u << j;
    stale = as_mobile(0, jobs, n);
    if (!stale)
        say("preferences already set", "", "");
    if (!(stale | locationd))
        _exit(0);
    for (j = 0; j < n; j++)
        if (((stale | locationd) & (1u << j)) && launchctl("unload", jobs[j]) == 0)
            stopped |= 1u << j;
    if (stale)
        as_mobile(1, jobs, n);
    for (j = 0; j < n; j++)
        if (stopped & (1u << j))
            say(jobs[j], launchctl("load", jobs[j]) == 0 ? " reloaded" : " reload failed",
                locationd & (1u << j) ? " (Wi-Fi up)" : "");
    _exit(0);
    return 0;
}
