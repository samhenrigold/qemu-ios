/* isprogress: drive the App Store progress bar from inside itunesstored.
 *
 * README.md route 1. The bar under a placeholder icon is fed by an ISDownload
 * that reaches SpringBoard as a CPDistributedNotificationCenter notification on
 * com.apple.iTunesStore.daemon-notifications, and
 * -[CPDistributedNotificationCenter postNotificationName:userInfo:] raises
 * unless the caller owns that name. launchd hands the receive right to the job
 * labelled com.apple.itunesstored and to nobody else, so the only way to post
 * one is to be running inside that process. This dylib gets there through
 * DYLD_INSERT_LIBRARIES in com.apple.itunesstored.plist -- which probe_insert.c
 * measured as still working on 3.1.3.
 *
 * SpringBoard is deliberately NOT the injection target. A dylib that throws
 * inside SpringBoard wedges the boot and costs a reimage; one that throws in
 * here gets itunesstored restarted by launchd and the UI never notices.
 *
 * THIS IS THE MEASUREMENT BUILD, not the product, and IT DOES NOT WORK YET.
 * It exists to answer the question README.md flags as unproven -- "that
 * SpringBoard's already-checked-in ISDownloadQueue reacts to a post made this
 * way" -- by posting a download and ramping its status 0..100% on its own. No
 * host involvement, because the host channel is a separate unsolved problem and
 * mixing the two would mean a red result could not be attributed.
 *
 * WHAT FOUR RUNS AGAINST A LIVE GUEST ESTABLISHED (2026-08-06), in order:
 *
 *   1. The injection works. "loaded in pid 16" every boot, and objc_setup()
 *      succeeds: libobjc, Foundation and iTunesStore all dlopen, and
 *      ISDownload / ISDownloadStatus / ISOperationProgress all resolve. The
 *      README's premise about class availability is correct.
 *   2. ISGetDistributedNotificationCenter() returns a live center from in here.
 *   3. It does not break the boot. Home screen at t+20s, lit=223488..223641
 *      against a 223581 baseline (the delta is the clock digit), fsck clean.
 *   4. AND YET THE BAR NEVER MOVES, because itunesstored does not stay alive.
 *      With KeepAlive it restart-loops -- pids 16, 42, 47, 51, 56, 61, 64 in
 *      150s, which is launchd's 10s respawn throttle -- and each incarnation is
 *      gone in under 3 seconds. No crash report: it is exiting cleanly, not
 *      dying. A wait of 45s, then 30s, then 3s, then none at all each stopped
 *      the log one step further along; only with no wait at all does it reach
 *      "got notification center", and it still dies before the first post.
 *
 * So the blocker is not that posting is illegal. It is that the process we are
 * required to live inside exits in under three seconds, and a progress bar has
 * to track an install that runs for tens of seconds to minutes. Fixing this
 * means finding out why itunesstored exits (it wants something this image does
 * not have -- no store account, no network, a CoreData store it cannot open)
 * and giving it a reason to stay, or holding it open from the host. Until then
 * route 1 is not shippable, and the RunAtLoad/KeepAlive this was measured with
 * must NOT ship either: they leave a system daemon restart-looping forever.
 *
 * The object graph is all public setters over plain +alloc/-init (see
 * `objct.py iTunesStore_armv6 ISDownload 0x48c50 0x14c`), so nothing here
 * depends on the keyed-archive format being reverse engineered: we are inside
 * the process that owns the classes, and NSKeyedArchiver encodes them for us.
 *
 * No headers on purpose, matching it_pbd.c and the rest of the guest tooling.
 * Plain C with a dlopen'd ObjC runtime, because `ld -lobjc` against the 3.1.3
 * SDK is fatal.
 */

extern long write(int, const void *, unsigned long);
extern int open(const char *, int, ...);
extern int close(int);
extern int getpid(void);
extern unsigned sleep(unsigned);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int pthread_create(void **, const void *, void *(*)(void *), void *);
extern int pthread_detach(void *);

#define RTLD_NOW 2
#define O_WRONLY 0x0001
#define O_CREAT  0x0200
#define O_APPEND 0x0008

typedef void *id_;
typedef void *SEL_;
typedef long long i64;
typedef unsigned long long u64;

#define IS_FRAMEWORK "/System/Library/PrivateFrameworks/iTunesStore.framework/iTunesStore"
#define FOUNDATION   "/System/Library/Frameworks/Foundation.framework/Foundation"

/* The identifier the placeholder is named by. +[SBDownloadingIcon
 * displayIdentifierForDownload:] is "com.apple.downloadingicon-" + the
 * decimal itemIdentifier, which is the same space sbdlicon's `add` uses --
 * so a placeholder raised there and a download posted here can name one icon.
 * 0 would collide with a real download that has no metadata; this will not. */
#define DEMO_ITEM_ID 8675309ULL

/* ---- logging: this runs inside a system daemon with nowhere to print ---- */

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }

static unsigned utoa(u64 v, char *out)
{
    char tmp[24];
    unsigned n = 0, i = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) out[i++] = tmp[--n];
    return i;
}

static void logline(const char *msg, i64 value, int have_value)
{
    char line[256];
    unsigned n = 0;
    const char *tag = "isprogress: ";
    while (tag[n] && n < 32) { line[n] = tag[n]; n++; }
    for (unsigned i = 0; msg[i] && n < sizeof(line) - 32; i++) line[n++] = msg[i];
    if (have_value) {
        line[n++] = ' ';
        n += utoa((u64)value, line + n);
    }
    line[n++] = '\n';
    int fd = open("/tmp/it-isprog.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, line, n); close(fd); }
}

/* ---- the ObjC runtime, resolved at run time ---- */

static id_ (*p_getClass)(const char *);
static SEL_ (*p_sel)(const char *);
static id_ (*m0)(id_, SEL_);
static id_ (*m1)(id_, SEL_, id_);
static id_ (*m1s)(id_, SEL_, const char *);
static id_ (*m1u)(id_, SEL_, unsigned);
static id_ (*m1q)(id_, SEL_, u64);
static id_ (*m2)(id_, SEL_, id_, id_);
static void (*mset_q)(id_, SEL_, i64);
static void (*mset_i)(id_, SEL_, int);

#define C(n) p_getClass(n)
#define S(n) p_sel(n)

static SEL_ sel_alloc, sel_init, sel_release, sel_retain;
static id_ cls_pool, cls_string, cls_number, cls_array, cls_dict, cls_indexset,
           cls_archiver;

static id_ str(const char *s) { return m1s(cls_string, S("stringWithUTF8String:"), s); }

/* NSKeyedArchiver +archivedDataWithRootObject:, the encoding every one of these
 * notifications carries its payload in. */
static id_ archived(id_ object)
{
    return m1(cls_archiver, S("archivedDataWithRootObject:"), object);
}

static int objc_setup(void)
{
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    /* Both of these live in the dyld shared cache and have no file on disk --
     * normal on 3.1.3, and dlopen resolves the path anyway. AppSupport is not
     * opened directly: iTunesStore links it, so it comes along. */
    dlopen(FOUNDATION, RTLD_NOW);
    void *is = dlopen(IS_FRAMEWORK, RTLD_NOW);
    if (!objc || !is) { logline("dlopen failed", 0, 0); return 0; }

    p_getClass = (id_ (*)(const char *))dlsym(objc, "objc_getClass");
    p_sel = (SEL_ (*)(const char *))dlsym(objc, "sel_registerName");
    void *msg = dlsym(objc, "objc_msgSend");
    if (!p_getClass || !p_sel || !msg) { logline("no objc runtime", 0, 0); return 0; }

    m0 = (id_ (*)(id_, SEL_))msg;
    m1 = (id_ (*)(id_, SEL_, id_))msg;
    m1s = (id_ (*)(id_, SEL_, const char *))msg;
    m1u = (id_ (*)(id_, SEL_, unsigned))msg;
    m1q = (id_ (*)(id_, SEL_, u64))msg;
    m2 = (id_ (*)(id_, SEL_, id_, id_))msg;
    /* 64-bit args land in r2:r3 here: r2 is the next free register after
     * self/_cmd and is already even-aligned, so AAPCS inserts no padding. */
    mset_q = (void (*)(id_, SEL_, i64))msg;
    mset_i = (void (*)(id_, SEL_, int))msg;

    sel_alloc = S("alloc");
    sel_init = S("init");
    sel_release = S("release");
    sel_retain = S("retain");
    cls_pool = C("NSAutoreleasePool");
    cls_string = C("NSString");
    cls_number = C("NSNumber");
    cls_array = C("NSArray");
    cls_dict = C("NSMutableDictionary");
    cls_indexset = C("NSIndexSet");
    cls_archiver = C("NSKeyedArchiver");
    if (!cls_pool || !cls_string || !cls_archiver) {
        logline("Foundation classes missing", 0, 0);
        return 0;
    }
    if (!C("ISDownload") || !C("ISDownloadStatus") || !C("ISOperationProgress")) {
        logline("iTunesStore classes missing", 0, 0);
        return 0;
    }
    return 1;
}

/* ---- the objects ---- */

/* ISDownloadMetadata is initWithDictionary: over raw iTunes manifest keys.
 * `songId` is the one that matters: -[ISDownload uniqueID] is its decimal
 * form, and that is what names the icon. */
static id_ make_metadata(u64 item_id, const char *title, const char *bundle_id)
{
    id_ d = m0(cls_dict, S("dictionary"));
    SEL_ setobj = S("setObject:forKey:");
    m2(d, setobj, m1q(cls_number, S("numberWithUnsignedLongLong:"), item_id),
       str("songId"));
    m2(d, setobj, str(title), str("itemName"));
    m2(d, setobj, str(bundle_id), str("bundle-id"));
    return m1(m0(C("ISDownloadMetadata"), sel_alloc), S("initWithDictionary:"), d);
}

/* THE TRAP, from README.md: -init sets normalizedCurrentValue/normalizedMaxValue
 * to -1, and the getters fall back to the raw pair while the normalized one is
 * negative. So set the raw pair and never touch the normalized one. maxValue
 * must also be non-zero in the very first status -- -[SBDownloadingIcon
 * downloadStatusChanged:] divides with no guard, and 0/0 is a NaN straight into
 * setProgress:. operationType 1 is what flips the label to "Installing...". */
static id_ make_status(i64 current, i64 max, int installing)
{
    id_ p = m0(m0(C("ISOperationProgress"), sel_alloc), sel_init);
    mset_q(p, S("setCurrentValue:"), current);
    mset_q(p, S("setMaxValue:"), max < 1 ? 1 : max);
    mset_i(p, S("setOperationType:"), installing ? 1 : 0);

    id_ s = m0(m0(C("ISDownloadStatus"), sel_alloc), sel_init);
    m1(s, S("setProgress:"), p);
    m0(p, sel_release);
    return s;
}

/* ---- posting ---- */

static id_ center(void)
{
    void *is = dlopen(IS_FRAMEWORK, RTLD_NOW);
    id_ (*get)(void) = (id_ (*)(void))dlsym(is, "ISGetDistributedNotificationCenter");
    if (!get) { logline("no ISGetDistributedNotificationCenter", 0, 0); return 0; }
    return get();
}

static void post(id_ c, const char *name, id_ userinfo)
{
    m2(c, S("postNotificationName:userInfo:"), str(name), userinfo);
}

static void *worker(void *unused)
{
    (void)unused;
    logline("loaded in pid", getpid(), 1);

    if (!objc_setup()) return 0;
    logline("objc ready", 0, 0);

    /* itunesstored has to have finished standing its notification server up,
     * and SpringBoard's ISDownloadQueue has to have checked in, before a post
     * reaches anything -- and the constructor runs long before either. Nothing
     * exported says when that is, so this waits. The home screen appears around
     * t+20s on this emulator; the daemon starts well before that.
     *
     * Measured the hard way: at 45s the log stopped dead after "loaded in pid"
     * with no crash report, because itunesstored had simply exited -- it has no
     * KeepAlive and nothing to do at boot, and an exiting process takes this
     * thread with it. The plist this is baked alongside now sets KeepAlive.  */
     * Measured again with KeepAlive on: the daemon restart-loops, pid 16, 42,
     * 47, 51 ... each incarnation exiting cleanly within 30s and never reaching
     * the post. So the wait cannot be long enough to be safe; post immediately
     * instead and let the ramp run across whatever lifetime there is. Each log
     * line below doubles as a heartbeat, so where the log stops IS the
     * daemon's lifetime. */
     * Then measured a third time from the constructor, which main() cannot
     * outlive: the log stopped in the identical place. So the process itself is
     * gone within ~3s of load, and no amount of waiting will help. This waits
     * not at all, to establish the one thing still worth knowing -- whether a
     * post from this process is legal at all. */
    logline("posting immediately", 0, 0);

    id_ pool = m0(m0(cls_pool, sel_alloc), sel_init);
    id_ c = center();
    if (!c) { m0(pool, sel_release); return 0; }
    logline("got notification center", 0, 0);

    /* ISNotificationDownloadsAdded first, and the item identifiers have to
     * match what follows: _downloadStatusChanged: looks the download up by
     * identifier and calls setStatus: on the one already in the queue, so a
     * status for an unknown item has nothing to land on. Both `param` and
     * `indexSet` are required -- the handler unarchives each and calls
     * insertObjects:atIndexes:, which throws on a nil index set. */
    id_ dl = m0(m0(C("ISDownload"), sel_alloc), sel_init);
    m1(dl, S("setMetadata:"), make_metadata(DEMO_ITEM_ID, "Progress Probe", "com.qemu.isprogress"));
    m1(dl, S("setStatus:"), make_status(0, 100, 0));

    id_ info = m0(cls_dict, S("dictionary"));
    m2(info, S("setObject:forKey:"), archived(m1(cls_array, S("arrayWithObject:"), dl)),
       str("param"));
    m2(info, S("setObject:forKey:"),
       archived(m1u(cls_indexset, S("indexSetWithIndex:"), 0)), str("indexSet"));
    post(c, "ISNotificationDownloadsAdded", info);
    logline("posted ISNotificationDownloadsAdded", 0, 0);
    m0(pool, sel_release);

    /* Then ramp it. If the placeholder's bar tracks this, route 1 is real. */
    for (int pct = 0; pct <= 100; pct += 5) {
        id_ p2 = m0(m0(cls_pool, sel_alloc), sel_init);
        id_ ui = m0(cls_dict, S("dictionary"));
        /* item-id is a plain NSNumber, read with unsignedLongLongValue -- it is
         * the one value in this protocol that is NOT archived. */
        m2(ui, S("setObject:forKey:"),
           m1q(cls_number, S("numberWithUnsignedLongLong:"), DEMO_ITEM_ID), str("item-id"));
        m2(ui, S("setObject:forKey:"),
           archived(make_status(pct, 100, pct > 60)), str("param"));
        post(c, "ISNotificationDownloadStatusChanged", ui);
        logline("posted status", pct, 1);
        m0(p2, sel_release);
        /* no sleep: the daemon does not live long enough for one. */
    }
    logline("ramp complete", 0, 0);
    return 0;
}

/* Normally this would hand off to a detached thread so the daemon is not held
 * up. It does the work inline instead, to separate the two things that produce
 * an identical log ("loaded in pid N", "objc ready", nothing further): the
 * daemon exiting and taking a detached thread with it, or the thread itself
 * being torn down. A constructor runs before main(), so main() returning cannot
 * outlive it -- if the log still stops in the same place from here, the process
 * is not surviving long enough to post no matter who does it. */
__attribute__((constructor)) static void isprogress_loaded(void)
{
    worker(0);
}
