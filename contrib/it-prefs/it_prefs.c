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
 * SETTINGS are re-applied on every boot. DEFAULTS are applied once per device
 * (the com.qemu.it-prefs DefaultsSet marker), so whatever the user picks in
 * Settings afterwards stays: Brightness at maximum and Auto-Lock at Never,
 * each written where that firmware's Settings writes it (see defaults()).
 *
 * Also once per device, on an iPhone whose CommCenter reads it (4.x; 1.0's does not): Data Roaming on
 * (see roaming()).
 *
 * Once Wi-Fi (en0) has an address it also restarts locationd, which otherwise
 * starts before Wi-Fi is powered and then never scans (see main()).
 *
 * IPSW-agnostic: each key name is confirmed in the binary that reads it first;
 * if an IPSW does not carry it, the job logs that and leaves that key alone.
 *
 * Plain C with CoreFoundation dlopen'd, like it_ethlink; built by
 * contrib/ipad1-guest/build.sh (the iPad's packages), and for the iPod by
 * build-ipod.sh with IT_PREFS_NO_LOCATION (no Wi-Fi location there) for the
 * n72 packages. Both run from the package's com.qemu.guest-prefs job; iPods
 * prepared before that had it baked, a pair this job removes (main()). Both are signed with it_prefs-entitlements.xml:
 * 4.x/5.x profiled only takes Auto-Lock from an entitled client.
 */
extern long write(int, const void *, unsigned long);
extern long read(int, void *, unsigned long);
extern int open(const char *, int, ...);
extern int unlink(const char *);
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
struct passwd {   /* Darwin's, up to pw_dir */
    char *pw_name, *pw_passwd;
    unsigned pw_uid, pw_gid;
    long pw_change;
    char *pw_class, *pw_gecos, *pw_dir;
};
extern struct passwd *getpwnam(const char *);

#define SPRINGBOARD "/System/Library/CoreServices/SpringBoard.app/SpringBoard"
#define LOCATIOND   "/usr/libexec/locationd"
#define LOCATIOND_JOB "/System/Library/LaunchDaemons/com.apple.locationd.plist"
#define COMMCENTER  "/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter"
#define COMMCENTER_JOB "/System/Library/LaunchDaemons/com.apple.CommCenter.plist"
#define ROAMING     "InternationalRoamingEDGE"

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
#ifndef IT_PREFS_NO_LOCATION   /* the iPod (build-ipod.sh): no Wi-Fi location there */
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

/* CoreFoundation, dlopen'd in the mobile child. */
static const void *(*str_)(const void *, const char *, unsigned);
static const void *(*get)(const void *, const void *);
static unsigned char (*equal)(const void *, const void *);
static void (*set)(const void *, const void *, const void *);
static unsigned char (*sync)(const void *);
static const void *(*num)(const void *, long, const void *);
static const void **yes, **no;

static int cf_load(void)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);

    if (!cf || !(str_ = dlsym(cf, "CFStringCreateWithCString")) || !(get = dlsym(cf, "CFPreferencesCopyAppValue"))
        || !(equal = dlsym(cf, "CFEqual")) || !(set = dlsym(cf, "CFPreferencesSetAppValue"))
        || !(sync = dlsym(cf, "CFPreferencesAppSynchronize")) || !(num = dlsym(cf, "CFNumberCreate"))
        || !(yes = dlsym(cf, "kCFBooleanTrue")) || !(no = dlsym(cf, "kCFBooleanFalse"))) {
        say("CoreFoundation preferences API not found; changing nothing", "", "");
        return 0;
    }
    return 1;
}

static const void *str(const char *s)
{
    return str_(0, s, 0x08000100);   /* kCFStringEncodingUTF8 */
}

/*
 * As mobile: apply every setting whose key the reader knows. With write false,
 * only compare. Returns a bit per job (index into jobs[]) whose settings are
 * not yet what they should be.
 */
static unsigned apply(int write, const char *const *jobs, unsigned njobs)
{
    unsigned i, j, stale = 0;

    if (!cf_load())
        return 0;
    for (i = 0; i < sizeof(SETTINGS) / sizeof(SETTINGS[0]); i++) {
        const struct setting *s = &SETTINGS[i];
        const void *app = str(s->domain), *key = str(s->key);
        const void *value = s->kind == TRUE ? *yes : s->kind == FALSE ? *no : str(s->string);
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

/* Settings' Never: 1.x-3.x store -1 in SBAutoLockTime, and SpringBoard takes it only with SBAutoDimTime
 * not above it (else it resets both to its defaults, 300/285 s on 3.2.2), so the dim time is -1 too;
 * from 4.0 setScreenLock:specifier: hands ManagedConfiguration INT_MAX for it (and screenLock: reads
 * INT_MAX back as Never). */
#define MC_FRAMEWORK "/System/Library/PrivateFrameworks/ManagedConfiguration.framework/ManagedConfiguration"
#define MC_NEVER 0x7fffffff

/* [[MCProfileConnection sharedConnection] setValue:@(INT_MAX) forSetting:feature], as Settings does; 1 once
 * the effective value reads back as Never. */
static int mc_never(const void *feature)
{
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", 2);
    void *(*cls)(const char *) = objc ? dlsym(objc, "objc_getClass") : 0;
    void *(*sel)(const char *) = objc ? dlsym(objc, "sel_registerName") : 0;
    void *(*msg)(void *, void *, ...) = objc ? dlsym(objc, "objc_msgSend") : 0;
    int never = MC_NEVER;
    const void *value = num(0, 3, &never);   /* kCFNumberSInt32Type; an NSNumber, toll-free */
    void *pool, *conn, *now;
    int ok;

    if (!cls || !sel || !msg || !cls("MCProfileConnection") || !cls("NSAutoreleasePool"))
        return 0;
    pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    conn = msg(cls("MCProfileConnection"), sel("sharedConnection"));
    msg(conn, sel("setValue:forSetting:"), value, feature);
    now = msg(conn, sel("effectiveValueForSetting:"), feature);
    ok = now && equal(now, value);
    msg(pool, sel("drain"));
    return ok;
}

/*
 * As mobile, once per device: Brightness at maximum and Auto-Lock at Never, the first time this job
 * runs on a device (its seal boot when prepared, its next boot when the package arrives later); never
 * again once the com.qemu.it-prefs DefaultsSet marker is down, so the user's later choices in Settings
 * stand. Each value goes where that firmware's own Settings puts it, found at run time:
 *   Brightness  com.apple.springboard SBBacklightLevel2 (2.x-5.x) or SBBacklightLevel (1.x), whichever
 *               SpringBoard names, = 1.0; then GSEventSetBacklightLevel(1.0), as the slider does, so a
 *               running SpringBoard takes it now rather than at its next launch (the iPod's first boot
 *               is the user's: no sealing boot runs this job first).
 *   Auto-Lock   ManagedConfiguration's MCFeatureAutoLockTime when the framework exports it (4.x, 5.x),
 *               else com.apple.springboard SBAutoLockTime and SBAutoDimTime = -1 when SpringBoard names
 *               them (1.x-3.x), then GSSendAppPreferencesChanged, as Settings does, for a running SpringBoard.
 */
static unsigned defaults(void)
{
    static const char *const backlight[] = { "SBBacklightLevel2", "SBBacklightLevel" };
    static const char *const autolock[] = { "SBAutoLockTime", "SBAutoDimTime" };
    const void *mine = str("com.qemu.it-prefs"), *marker = str("DefaultsSet"), *sb = str("com.apple.springboard");
    void *mc = dlopen(MC_FRAMEWORK, 2), *gs;
    const void **feature = mc ? dlsym(mc, "MCFeatureAutoLockTime") : 0;
    void (*live)(float);
    void (*changed)(const void *, const void *);
    float full = 1;
    int never = -1;
    unsigned i;

    if (get(marker, mine)) {
        say("defaults already set once; left to the user", "", "");
        return 0;
    }
    for (i = 0; i < sizeof(backlight) / sizeof(backlight[0]); i++)
        if (file_has(SPRINGBOARD, backlight[i])) {
            set(str(backlight[i]), num(0, 12, &full), sb);   /* kCFNumberFloatType */
            say(backlight[i], " = 1.0 (Brightness at maximum)", "");
        }
    if (feature && *feature)
        say("MCFeatureAutoLockTime", mc_never(*feature) ? " = Never" : " not taken by ManagedConfiguration", "");
    else if (file_has(SPRINGBOARD, autolock[0])) {
        for (i = 0; i < sizeof(autolock) / sizeof(autolock[0]); i++)
            if (file_has(SPRINGBOARD, autolock[i])) {
                set(str(autolock[i]), num(0, 3, &never), sb);   /* kCFNumberSInt32Type */
                say(autolock[i], " = -1 (Auto-Lock Never)", "");
            }
    } else
        say(SPRINGBOARD, " has no ", autolock[0]);
    if (!sync(sb))
        say("com.apple.springboard not saved: CFPreferencesAppSynchronize failed", "", "");
    gs = dlopen("/System/Library/PrivateFrameworks/GraphicsServices.framework/GraphicsServices", 2);
    if (gs && (live = dlsym(gs, "GSEventSetBacklightLevel")))
        live(full);
    if (gs && !(feature && *feature) && (changed = dlsym(gs, "GSSendAppPreferencesChanged")))
        changed(sb, str(autolock[0]));
    set(marker, *yes, mine);
    sync(mine);
    return 0;
}

/* Run apply() (what 0 compare, 1 write) or defaults() (2) as mobile in a child; apply()'s result
 * comes back as the exit code. */
static unsigned as_mobile(int what, const char *const *jobs, unsigned njobs)
{
    int pid = fork(), status = 0;
    if (pid == 0) {
        /* CFPreferences finds the user's preferences through HOME, which
         * is still root's in a child of a root job. */
        if (setgid(501) || setuid(501) || setenv("HOME", "/var/mobile", 1)) {   /* mobile */
            say("could not become mobile; changing nothing", "", "");
            _exit(0);
        }
        _exit(what == 2 ? (cf_load() ? defaults() : 0) : apply(what, jobs, njobs));
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return 0;
    return (status >> 8) & 0xff;
}

/*
 * Data Roaming on, once per device (its own com.qemu.it-prefs RoamingSet marker, so the user's later
 * choice stands). The emulated network is the test PLMN 001/01, for which no carrier bundle exists, so
 * CommCenter counts the SIM as roaming and keeps packet data off unless Data Roaming is on
 * (docs/baseband/commcenter-4.2.1-3gs.md, "Roaming"). The switch is com.apple.commcenter
 * InternationalRoamingEDGE in CommCenter's own user's preferences (it runs as _wireless and reads it with
 * kCFPreferencesCurrentUser); Settings changes it through CommCenter, which holds it in memory, so
 * CommCenter is unloaded around the write and loaded again.
 */
static int roaming_step(int write)
{
    struct passwd *pw = getpwnam("_wireless");
    const void *mine, *marker, *cc;

    if (!pw || setgid(pw->pw_gid) || setuid(pw->pw_uid) || setenv("HOME", pw->pw_dir, 1)) {
        say("could not become _wireless; Data Roaming left alone", "", "");
        return 0;
    }
    if (!cf_load())
        return 0;
    mine = str("com.qemu.it-prefs"), marker = str("RoamingSet"), cc = str("com.apple.commcenter");
    if (get(marker, mine))
        return 0;
    if (!write)
        return 1;
    set(str(ROAMING), *yes, cc);
    say(ROAMING, sync(cc) ? " = true (Data Roaming on)" : " not saved: CFPreferencesAppSynchronize failed", "");
    set(marker, *yes, mine);
    sync(mine);
    return 0;
}

static int as_wireless(int write)
{
    int pid = fork(), status = 0;
    if (pid == 0)
        _exit(roaming_step(write));
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return 0;
    return (status >> 8) & 0xff;
}

static void roaming(void)
{
    if (!file_has(COMMCENTER, ROAMING) || !as_wireless(0))
        return;
    if (launchctl("unload", COMMCENTER_JOB) == 0) {
        as_wireless(1);
        say(COMMCENTER_JOB, launchctl("load", COMMCENTER_JOB) == 0 ? " reloaded" : " reload failed", "");
    }
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
/* The iPod's baked copy, from before the packages carried it. It ran this boot too (launchd loaded it first),
 * as the tip-only build: harmless, as it writes only an unset tip. Gone from the next boot on.
 * ponytail: on a device never booted before, both may write com.apple.springboard at once and one sync can
 * drop the other's keys; such devices are fresh prepares, which no longer bake it. */
static void retire_baked(void)
{
    if (unlink("/System/Library/LaunchDaemons/com.qemu.it-prefs.plist") == 0) {
        unlink("/usr/local/bin/it_prefs");
        say("removed the baked com.qemu.it-prefs job; the package's runs instead", "", "");
    }
}

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
    retire_baked();
    as_mobile(2, jobs, n);          /* first: the seal boot halts 40 s in, and Wi-Fi can take longer */
    roaming();
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
