/*
 * it_boot -- the guest-package loader (docs/guest-package-bootstrap.md, "it_boot",
 * in the LightTouchMac tree). Baked once at prepare time, never upgraded itself;
 * everything it loads is. Runs from com.qemu.it-boot.plist at every boot, and on
 * the iPod again whenever the agent runs `launchctl start com.qemu.it-boot`.
 *
 * FAIL-OPEN: whatever goes wrong while pulling or staging an offer, the package
 * `current` already points at is what gets its hooks and jobs. The only things
 * that ever move `current` are a fully staged package (rename, then an atomic
 * symlink flip), the host's verdicts, and the no-verdict retry limit.
 *
 * Layout (ROOT = /usr/local/lighttouch, on the system volume, never /var):
 *   ROOT/pkgs/<serial>/        one package; its `offer` file is its own record
 *   ROOT/current -> pkgs/<serial>
 *   ROOT/state                 seed/previous/good/tries/bad/hook lines
 *
 * The offer (QC_PKG_OFFER), line text; the host serves each payload by index:
 *   ltpkg 1
 *   build 7E18                                   kern.osversion it is for
 *   serial 14 1.4.0                              0 = back to the seed (safe mode)
 *   verdict good 13 | verdict bad 12             the host's verdicts, any number
 *   file 0 bin/it_agent 755 <size> <sha256>
 *   job  1 jobs/com.qemu.it-agent.plist 644 <size> <sha256>
 *   hook 2 hooks/MBXGLEngine 755 <size> <sha256> /System/.../MBXGLEngine [respring]
 *
 * REPORT (QC_PKG_REPORT): token = the serial now current, offset = one of the
 * R_* codes below (negative: an install failed and current is unchanged),
 * buffer = a short "prev P good G tries T seed S" line.
 *
 * libSystem only. SHA-256 comes from CommonCrypto through dlsym; where there is
 * none (2.x), files are checked by size alone.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define QC_PKG_OFFER  0x170
#define QC_PKG_READ   0x171
#define QC_PKG_REPORT 0x172

#define WINDOW       1024              /* host's per-call cap */
#define OFFER_MAX    (64 * 1024)
#define FILE_MAX     (16 * 1024 * 1024)
#define MAX_ENT      64
#define MAX_SERIALS  16
#define MAX_HOOKS    32
#define PULL_SECONDS 10              /* minimum transfer allowance */
#define PULL_MAX_SECONDS 60          /* policy cap, not a 1-GiB throughput promise */
#define PULL_BYTES_PER_SECOND (128 * 1024)
#define MAX_TRIES    2                 /* boots without a verdict before reverting */
#define PATHN        512

enum {
    R_UNCHANGED = 0,
    R_INSTALLED = 1,
    R_SWITCHED = 2,        /* to a package already on disk: previous, seed, safe mode */
    R_REVERTED_BAD = 3,
    R_REVERTED_TRIES = 4,
    R_REFUSED = 5,         /* the offered serial is one this device already reverted */
};

#ifdef IT_BOOT_TEST
#include IT_BOOT_TEST      /* qc, launchctl, os_build, root_dir, sys_root, sha_disabled */
#else
#include <sys/sysctl.h>

/* qemu_call_t with the qc_ag_args_t arguments: 52 bytes, frozen (general.h). */
typedef struct __attribute__((packed)) {
    uint32_t call_number;
    uint32_t buffer;
    uint32_t offset;
    uint32_t length;
    uint64_t token;
    unsigned char pad[12];
    int64_t retval;
    int64_t error;
} qemu_call_t;

static int64_t qc(uint32_t op, void *buf, uint32_t off, uint32_t len, uint64_t token)
{
    qemu_call_t q;
    memset(&q, 0, sizeof(q));
    q.call_number = op;
    q.buffer = (uint32_t)(uintptr_t)buf;
    q.offset = off;
    q.length = len;
    q.token = token;
    q.retval = -1;          /* an emulator that ignores the call leaves this */
    void *a = &q;
    __asm__ volatile("mcr p15, 3, %0, c15, c15, 0" : : "r"(a) : "memory");
    return q.retval;
}

static int launchctl(const char *verb, const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/launchctl", "launchctl", verb, arg, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void os_build(char *out, size_t n)
{
    size_t len = n;
    if (sysctlbyname("kern.osversion", out, &len, NULL, 0) != 0) {
        out[0] = 0;
    }
    out[n - 1] = 0;
    if (out[0]) {
        return;
    }
    /* 1.x leaves kern.osversion empty: the build SystemVersion.plist (XML there) names */
    char text[2048], *p;
    int fd = open("/System/Library/CoreServices/SystemVersion.plist", O_RDONLY);
    ssize_t r = fd < 0 ? -1 : read(fd, text, sizeof(text) - 1);
    if (fd >= 0) {
        close(fd);
    }
    text[r > 0 ? r : 0] = 0;
    if ((p = strstr(text, "<key>ProductBuildVersion</key>")) && (p = strstr(p, "<string>"))) {
        p += 8;
        size_t i = 0;
        while (p[i] && p[i] != '<' && i + 1 < n) {
            out[i] = p[i];
            i++;
        }
        out[i] = 0;
    }
}

static const char *root_dir(void) { return "/usr/local/lighttouch"; }
static const char *sys_root(void) { return ""; }
static int sha_disabled(void) { return 0; }
#endif

struct entry {
    char kind;                     /* 'f' file, 'j' job, 'h' hook */
    unsigned idx;
    char path[256];                /* inside the package */
    unsigned mode;
    unsigned long size;
    char sha[65];
    char target[256];              /* hooks: absolute stock path */
    int respring;
};

struct offer {
    long serial;
    char build[32];
    int nent;
    struct entry e[MAX_ENT];
    long good[MAX_SERIALS], bad[MAX_SERIALS];
    int ngood, nbad;
};

struct state {
    long seed, previous, good, tries;
    long bad[MAX_SERIALS];
    int nbad;
    int nhooks;
    struct { char target[256]; int respring; } hooks[MAX_HOOKS];
};

static void say(const char *fmt, const char *a, long n)
{
    char line[600];
    int len = snprintf(line, sizeof(line), fmt, a, n);
    if (len > 0) {
        write(2, line, (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1);
    }
}

static void pathf(char *out, const char *fmt, const char *a, long n)
{
    char tail[PATHN];
    snprintf(tail, sizeof(tail), fmt, a, n);
    snprintf(out, PATHN, "%s/%s", root_dir(), tail);
}

static void pkg_path(char *out, long serial, const char *rel)
{
    char tail[PATHN];
    snprintf(tail, sizeof(tail), "pkgs/%ld%s%s", serial, rel ? "/" : "", rel ? rel : "");
    snprintf(out, PATHN, "%s/%s", root_dir(), tail);
}

/* --- SHA-256 through CommonCrypto, if this libSystem has it ------------------------ */

typedef int (*sha_init_fn)(void *);
typedef int (*sha_update_fn)(void *, const void *, uint32_t);
typedef int (*sha_final_fn)(unsigned char *, void *);
static sha_init_fn sha_init;
static sha_update_fn sha_update;
static sha_final_fn sha_final;

static void sha_load(void)
{
    if (sha_disabled()) {
        return;
    }
    sha_init = (sha_init_fn)dlsym(RTLD_DEFAULT, "CC_SHA256_Init");
    sha_update = (sha_update_fn)dlsym(RTLD_DEFAULT, "CC_SHA256_Update");
    sha_final = (sha_final_fn)dlsym(RTLD_DEFAULT, "CC_SHA256_Final");
    if (!sha_init || !sha_update || !sha_final) {
        sha_init = NULL;
        say("it_boot: no CommonCrypto%s, checking sizes only\n", "", 0);
    }
}

typedef struct { uint64_t space[16]; } sha_ctx;   /* CC_SHA256_CTX is 104 bytes */

static void sha_hex(sha_ctx *c, char out[65])
{
    unsigned char d[32];
    sha_final(d, c);
    for (int i = 0; i < 32; i++) {
        snprintf(out + 2 * i, 3, "%02x", d[i]);
    }
}

/* --- small filesystem helpers ----------------------------------------------------- */

static int write_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return -EIO;
        }
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

static void fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static int mkdirs(const char *path)
{
    char p[PATHN];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0755) != 0 && errno != EEXIST) {
                return -EIO;
            }
            *s = '/';
        }
    }
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -EIO;
}

static int mkparent(const char *path)
{
    char p[PATHN];
    snprintf(p, sizeof(p), "%s", path);
    char *slash = strrchr(p, '/');
    if (!slash || slash == p) {
        return 0;
    }
    *slash = 0;
    return mkdirs(p);
}

static void rmtree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
                char child[PATHN];
                snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                rmtree(child);
            }
        }
        if (d) {
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static int read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t n = 0;
    ssize_t r;
    while (n + 1 < cap && (r = read(fd, buf + n, cap - 1 - n)) > 0) {
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (int)n;
}

/* write tmp, fsync, rename: the file is either the old or the new one */
static int write_atomic(const char *path, const char *data, size_t n, unsigned mode)
{
    char tmp[PATHN];
    snprintf(tmp, sizeof(tmp), "%s.lt-new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -EIO;
    }
    int rc = write_all(fd, data, n);
    if (!rc && fsync(fd) != 0) {
        rc = -EIO;
    }
    close(fd);
    if (!rc && (chmod(tmp, mode) != 0 || rename(tmp, path) != 0)) {
        rc = -EIO;
    }
    if (rc) {
        unlink(tmp);
    }
    return rc;
}

static int copy_atomic(const char *src, const char *dst, unsigned mode)
{
    char tmp[PATHN], buf[8192];
    snprintf(tmp, sizeof(tmp), "%s.lt-new", dst);
    int in = open(src, O_RDONLY), rc = 0;
    if (in < 0) {
        return -ENOENT;
    }
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) {
        close(in);
        return -EIO;
    }
    ssize_t r;
    while (!rc && (r = read(in, buf, sizeof(buf))) > 0) {
        rc = write_all(out, buf, (size_t)r);
    }
    if (!rc && (r < 0 || fsync(out) != 0)) {
        rc = -EIO;
    }
    close(in);
    close(out);
    if (!rc && (chmod(tmp, mode) != 0 || rename(tmp, dst) != 0)) {
        rc = -EIO;
    }
    if (rc) {
        unlink(tmp);
    }
    return rc;
}

static int same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0 || sa.st_size != sb.st_size) {
        return 0;
    }
    if ((sa.st_mode & 07777) != (sb.st_mode & 07777)) {
        return 0;
    }
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY), same = fa >= 0 && fb >= 0;
    char x[4096], y[4096];
    while (same) {
        ssize_t n = read(fa, x, sizeof(x));
        if (n <= 0) {
            same = n == 0;
            break;
        }
        same = read(fb, y, (size_t)n) == n && !memcmp(x, y, (size_t)n);
    }
    if (fa >= 0) {
        close(fa);
    }
    if (fb >= 0) {
        close(fb);
    }
    return same;
}

/* --- offer / package record ------------------------------------------------------- */

static int safe_rel(const char *p)
{
    return p[0] && p[0] != '/' && !strstr(p, "..") && strlen(p) < 255;
}

static int safe_abs(const char *p)
{
    return p[0] == '/' && !strstr(p, "..") && !strstr(p, ".lt-new") && strlen(p) < 240;
}

static int parse_long(const char *s, long *out)
{
    char *end;
    if (!s || !*s) {
        return -1;
    }
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*end || errno || v < 0) {
        return -1;
    }
    *out = v;
    return 0;
}

static int is_hex64(const char *s)
{
    if (strlen(s) != 64) {
        return 0;
    }
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/* text is modified in place (tokenised) */
static int parse_offer(char *text, struct offer *o)
{
    memset(o, 0, sizeof(*o));
    o->serial = -1;
    int header = 0;
    char *save_line = NULL;
    for (char *line = strtok_r(text, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
        char *tok[10], *save = NULL;
        int n = 0;
        for (char *t = strtok_r(line, " \t\r", &save); t && n < 10; t = strtok_r(NULL, " \t\r", &save)) {
            tok[n++] = t;
        }
        if (!n) {
            continue;
        }
        if (!header) {
            if (n < 2 || strcmp(tok[0], "ltpkg") || strcmp(tok[1], "1")) {
                return -EINVAL;
            }
            header = 1;
        } else if (!strcmp(tok[0], "build") && n >= 2) {
            snprintf(o->build, sizeof(o->build), "%s", tok[1]);
        } else if (!strcmp(tok[0], "serial") && n >= 2) {
            if (parse_long(tok[1], &o->serial)) {
                return -EINVAL;
            }
        } else if (!strcmp(tok[0], "verdict") && n >= 3) {
            long s;
            if (parse_long(tok[2], &s)) {
                return -EINVAL;
            }
            if (!strcmp(tok[1], "good") && o->ngood < MAX_SERIALS) {
                o->good[o->ngood++] = s;
            } else if (!strcmp(tok[1], "bad") && o->nbad < MAX_SERIALS) {
                o->bad[o->nbad++] = s;
            }
        } else if ((!strcmp(tok[0], "file") || !strcmp(tok[0], "job") || !strcmp(tok[0], "hook")) && n >= 6) {
            if (o->nent == MAX_ENT) {
                return -EINVAL;
            }
            struct entry *e = &o->e[o->nent++];
            long idx, size;
            char *end;
            e->kind = tok[0][0];
            e->mode = (unsigned)strtoul(tok[3], &end, 8);
            if (parse_long(tok[1], &idx) || *end || e->mode > 07777 || parse_long(tok[4], &size) ||
                size > FILE_MAX || !safe_rel(tok[2]) || !is_hex64(tok[5])) {
                return -EINVAL;
            }
            e->idx = (unsigned)idx;
            e->size = (unsigned long)size;
            snprintf(e->path, sizeof(e->path), "%s", tok[2]);
            snprintf(e->sha, sizeof(e->sha), "%s", tok[5]);
            if (e->kind == 'h') {
                if (n < 7 || !safe_abs(tok[6])) {
                    return -EINVAL;
                }
                snprintf(e->target, sizeof(e->target), "%s", tok[6]);
                e->respring = n >= 8 && !strcmp(tok[7], "respring");
            }
        }
        /* anything else: a later protocol's line, ignored */
    }
    return header && o->serial >= 0 ? 0 : -EINVAL;
}

/* a package on disk: its record parses and every payload has the recorded size */
static int load_pkg(long serial, struct offer *o)
{
    static char text[OFFER_MAX];
    char path[PATHN];
    if (serial < 0) {
        return -1;
    }
    pkg_path(path, serial, "offer");
    if (read_file(path, text, sizeof(text)) <= 0 || parse_offer(text, o) || o->serial != serial) {
        return -1;
    }
    for (int i = 0; i < o->nent; i++) {
        struct stat st;
        pkg_path(path, serial, o->e[i].path);
        if (stat(path, &st) != 0 || (unsigned long)st.st_size != o->e[i].size) {
            return -1;
        }
    }
    return 0;
}

static int pkg_ok(long serial)
{
    static struct offer o;
    return load_pkg(serial, &o) == 0;
}

/* --- state ------------------------------------------------------------------------ */

static int in_list(const long *l, int n, long s)
{
    for (int i = 0; i < n; i++) {
        if (l[i] == s) {
            return 1;
        }
    }
    return 0;
}

static void add_bad(struct state *st, long s)
{
    if (in_list(st->bad, st->nbad, s)) {
        return;
    }
    if (st->nbad == MAX_SERIALS) {
        memmove(st->bad, st->bad + 1, sizeof(st->bad[0]) * (MAX_SERIALS - 1));
        st->nbad--;
    }
    st->bad[st->nbad++] = s;
}

/* returns 0 when there was no state file yet */
static int load_state(struct state *st)
{
    char text[8192], path[PATHN];
    memset(st, 0, sizeof(*st));
    st->seed = st->previous = st->good = -1;
    pathf(path, "state", "", 0);
    if (read_file(path, text, sizeof(text)) < 0) {
        return 0;
    }
    char *save_line = NULL;
    for (char *line = strtok_r(text, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
        char key[16];
        long v;
        int respring;
        char target[256];
        if (sscanf(line, "hook %d %255s", &respring, target) == 2 && safe_abs(target) &&
            st->nhooks < MAX_HOOKS) {
            snprintf(st->hooks[st->nhooks].target, sizeof(target), "%s", target);
            st->hooks[st->nhooks++].respring = respring;
        } else if (sscanf(line, "%15s %ld", key, &v) == 2) {
            if (!strcmp(key, "seed")) {
                st->seed = v;
            } else if (!strcmp(key, "previous")) {
                st->previous = v;
            } else if (!strcmp(key, "good")) {
                st->good = v;
            } else if (!strcmp(key, "tries")) {
                st->tries = v;
            } else if (!strcmp(key, "bad")) {
                add_bad(st, v);
            }
        }
    }
    return 1;
}

static int save_state(const struct state *st)
{
    char text[16384], path[PATHN];
    int n = snprintf(text, sizeof(text), "seed %ld\nprevious %ld\ngood %ld\ntries %ld\n",
                     st->seed, st->previous, st->good, st->tries);
    for (int i = 0; i < st->nbad && n < (int)sizeof(text); i++) {
        n += snprintf(text + n, sizeof(text) - n, "bad %ld\n", st->bad[i]);
    }
    for (int i = 0; i < st->nhooks && n < (int)sizeof(text); i++) {
        n += snprintf(text + n, sizeof(text) - n, "hook %d %s\n", st->hooks[i].respring, st->hooks[i].target);
    }
    if (n >= (int)sizeof(text)) {
        return -EIO;
    }
    pathf(path, "state", "", 0);
    return write_atomic(path, text, (size_t)n, 0644);
}

static long current_serial(void)
{
    char path[PATHN], link[PATHN];
    long s;
    pathf(path, "current", "", 0);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n <= 5) {
        return -1;
    }
    link[n] = 0;
    return strncmp(link, "pkgs/", 5) || parse_long(link + 5, &s) ? -1 : s;
}

static int flip(long serial)
{
    char path[PATHN], tmp[PATHN], target[64];
    pathf(path, "current", "", 0);
    pathf(tmp, "current.lt-new", "", 0);
    snprintf(target, sizeof(target), "pkgs/%ld", serial);
    unlink(tmp);
    if (symlink(target, tmp) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -EIO;
    }
    fsync_dir(root_dir());
    return 0;
}

/* --- pulling --------------------------------------------------------------------- */

/*
 * The host copies into guest memory by virtual address and fails on a page
 * the process has not touched yet (bss is mapped on first write), so every
 * window is written before it is handed over.
 */
static int pull_offer(char *text, size_t cap)
{
    int64_t total = qc(QC_PKG_OFFER, NULL, 0, 0, 0);
    if (total <= 0) {
        return -ENOENT;                    /* no host, or no offer this boot */
    }
    if ((uint64_t)total >= cap) {
        return -EINVAL;
    }
    for (int64_t off = 0; off < total;) {
        uint32_t want = total - off > WINDOW ? WINDOW : (uint32_t)(total - off);
        memset(text + off, 0, want);
        int64_t n = qc(QC_PKG_OFFER, text + off, (uint32_t)off, want, 0);
        if (n <= 0 || n > want) {
            return -EIO;
        }
        off += n;
    }
    text[total] = 0;
    return (int)total;
}

/* mach_absolute_time exists on the oldest supported guests. Wall time changes
 * when the agent synchronizes the guest clock and cannot bound a transfer. */
struct pull_clock { uint64_t start; uint32_t numer, denom, seconds; };
static int pull_clock_start(struct pull_clock *clock, const struct offer *offer)
{
    mach_timebase_info_data_t scale;
    if (mach_timebase_info(&scale) != 0 || !scale.numer || !scale.denom) {
        return -EIO;
    }
    /* All sizes are validated <= FILE_MAX and there are <= MAX_ENT files;
     * the maximum sum (1 GiB) fits uint32_t on the oldest guest. */
    uint32_t total = 0;
    for (int i = 0; i < offer->nent; i++) {
        total += (uint32_t)offer->e[i].size;
    }
    uint32_t allowance = PULL_SECONDS +
        (total + PULL_BYTES_PER_SECOND - 1) / PULL_BYTES_PER_SECOND;
    clock->seconds = allowance < PULL_MAX_SECONDS ? allowance : PULL_MAX_SECONDS;
    clock->numer = scale.numer;
    clock->denom = scale.denom;
    clock->start = mach_absolute_time();
    return 0;
}

static int pull_clock_expired(const struct pull_clock *clock)
{
    uint64_t ticks = mach_absolute_time() - clock->start;
    /* Split the conversion: legacy libSystem does not export compiler-rt's
     * 64-bit division/conversion helpers. Both ARM variants have VFP. */
    double elapsed = (double)(uint32_t)(ticks >> 32) * 4294967296.0 +
                     (double)(uint32_t)ticks;
    return elapsed * clock->numer / clock->denom >= clock->seconds * 1000000000.0;
}

static int fetch(const struct entry *e, const char *dst, const struct pull_clock *clock)
{
    static char buf[WINDOW];
    sha_ctx c;
    if (qc(QC_PKG_READ, NULL, 0, 0, e->idx) != (int64_t)e->size) {
        return -EBADMSG;
    }
    if (mkparent(dst)) {
        return -EIO;
    }
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600), rc = 0;
    if (fd < 0) {
        return -EIO;
    }
    if (sha_init) {
        sha_init(&c);
    }
    for (unsigned long off = 0; !rc && off < e->size;) {
        uint32_t want = e->size - off > WINDOW ? WINDOW : (uint32_t)(e->size - off);
        memset(buf, 0, want);
        int64_t n = qc(QC_PKG_READ, buf, (uint32_t)off, want, e->idx);
        if (n <= 0 || n > want) {
            rc = -EIO;
        } else if (pull_clock_expired(clock)) {
            rc = -ETIMEDOUT;
        } else {
            rc = write_all(fd, buf, (size_t)n);
            if (sha_init) {
                sha_update(&c, buf, (uint32_t)n);
            }
            off += (unsigned long)n;
        }
    }
    if (!rc && fsync(fd) != 0) {
        rc = -EIO;
    }
    close(fd);
    if (!rc && chmod(dst, e->mode) != 0) {
        rc = -EIO;
    }
    if (!rc && sha_init) {
        char hex[65];
        sha_hex(&c, hex);
        if (strcmp(hex, e->sha)) {
            rc = -EBADMSG;
        }
    }
    return rc;
}

/* stage pkgs/<s>.tmp, then rename it into place */
static int install(const struct offer *o, const char *text, size_t len)
{
    char tmp[PATHN], dir[PATHN], dst[PATHN];
    struct pull_clock clock;
    int clock_error = pull_clock_start(&clock, o);
    if (clock_error) {
        return clock_error;
    }
    pkg_path(dir, o->serial, NULL);
    snprintf(tmp, sizeof(tmp), "%s.tmp", dir);
    rmtree(tmp);
    if (mkdirs(tmp)) {
        return -EIO;
    }
    int rc = 0;
    for (int i = 0; !rc && i < o->nent; i++) {
        snprintf(dst, sizeof(dst), "%s/%s", tmp, o->e[i].path);
        rc = fetch(&o->e[i], dst, &clock);
    }
    if (!rc) {
        snprintf(dst, sizeof(dst), "%s/offer", tmp);
        rc = write_atomic(dst, text, len, 0644);
    }
    if (!rc) {
        fsync_dir(tmp);
        rmtree(dir);                       /* an incomplete copy from before */
        rc = rename(tmp, dir) == 0 ? 0 : -EIO;
    }
    if (rc) {
        rmtree(tmp);
    } else {
        pathf(dst, "pkgs", "", 0);
        fsync_dir(dst);
    }
    return rc;
}

/* --- activation ------------------------------------------------------------------ */

static int respring_needed;

static void apply_hooks(struct state *st, long serial)
{
    static struct offer o;
    if (load_pkg(serial, &o)) {
        o.nent = 0;
    }
    for (int i = 0; i < o.nent; i++) {       /* remember every target ever hooked */
        const struct entry *e = &o.e[i];
        int k;
        if (e->kind != 'h') {
            continue;
        }
        for (k = 0; k < st->nhooks && strcmp(st->hooks[k].target, e->target); k++) {
        }
        if (k == st->nhooks && k < MAX_HOOKS) {
            snprintf(st->hooks[k].target, sizeof(st->hooks[k].target), "%s", e->target);
            st->nhooks++;
        }
        if (k < MAX_HOOKS) {
            st->hooks[k].respring = e->respring;
        }
    }
    for (int k = 0; k < st->nhooks; k++) {
        char target[PATHN], baked[PATHN], src[PATHN];
        const struct entry *want = NULL;
        struct stat bs;
        snprintf(target, sizeof(target), "%s%s", sys_root(), st->hooks[k].target);
        snprintf(baked, sizeof(baked), "%s.baked", target);
        for (int i = 0; i < o.nent; i++) {
            if (o.e[i].kind == 'h' && !strcmp(o.e[i].target, st->hooks[k].target)) {
                want = &o.e[i];
            }
        }
        if (want) {
            pkg_path(src, serial, want->path);
            struct stat ts;
            if (access(baked, F_OK) != 0 && stat(target, &ts) == 0 &&
                link(target, baked) != 0 && copy_atomic(target, baked, ts.st_mode & 07777) != 0) {
                say("it_boot: cannot keep %s.baked; hook skipped\n", st->hooks[k].target, 0);
                continue;
            }
        } else if (stat(baked, &bs) == 0) {
            snprintf(src, sizeof(src), "%s", baked);
        } else {
            continue;                        /* nothing baked to go back to */
        }
        if (same_file(src, target)) {
            continue;
        }
        struct stat ss;
        mkparent(target);
        if (stat(src, &ss) == 0 && copy_atomic(src, target, ss.st_mode & 07777) == 0) {
            say("it_boot: hook %s updated\n", st->hooks[k].target, 0);
            respring_needed |= st->hooks[k].respring;
        } else {
            say("it_boot: hook %s failed\n", st->hooks[k].target, 0);
        }
    }
}

static void jobs(long serial, const char *verb)
{
    static struct offer o;
    char path[PATHN];
    if (load_pkg(serial, &o)) {
        return;
    }
    for (int i = 0; i < o.nent; i++) {
        if (o.e[i].kind == 'j') {
            pkg_path(path, serial, o.e[i].path);
            launchctl(verb, path);
        }
    }
}

static long revert_target(const struct state *st, long cur)
{
    const long order[] = { st->previous, st->good, st->seed };
    for (int i = 0; i < 3; i++) {
        if (order[i] >= 0 && order[i] != cur && !in_list(st->bad, st->nbad, order[i]) && pkg_ok(order[i])) {
            return order[i];
        }
    }
    return -1;
}

int it_boot_run(void)
{
    static char text[OFFER_MAX], record[OFFER_MAX];
    static struct offer o;
    struct state st;
    char path[PATHN];
    int result = R_UNCHANGED;

    sha_load();
    respring_needed = 0;
    mkdirs(root_dir());
    pathf(path, "pkgs", "", 0);
    mkdirs(path);
    pathf(path, "current.lt-new", "", 0);
    unlink(path);
    pathf(path, "state.lt-new", "", 0);
    unlink(path);
    /* torn installs: a staging directory is never trusted */
    pathf(path, "pkgs", "", 0);
    DIR *d = opendir(path);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        size_t n = strlen(de->d_name);
        if (n > 4 && !strcmp(de->d_name + n - 4, ".tmp")) {
            char child[PATHN];
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            rmtree(child);
        }
    }
    if (d) {
        closedir(d);
    }

    int had_state = load_state(&st);
    long cur = current_serial();
    if (!pkg_ok(cur)) {
        long back = pkg_ok(st.seed) ? st.seed : -1;
        say("it_boot: current package %s%ld unusable\n", "", cur);
        if (back >= 0 && flip(back) == 0) {
            cur = back;
        } else {
            cur = -1;
        }
    }
    if (!had_state) {
        st.seed = cur;                     /* the preparer normally writes this */
    }
    const long start = cur;

    int len = pull_offer(text, sizeof(text));
    if (len > 0) {
        memcpy(record, text, (size_t)len + 1);
    }
    if (len > 0 && parse_offer(text, &o) != 0) {
        say("it_boot: malformed offer\n", "", 0);
        result = -EINVAL;
    } else if (len > 0) {
        int verdict = 0;
        for (int i = 0; i < o.ngood; i++) {
            if (o.good[i] == cur) {
                st.good = cur;
                st.tries = 0;
                verdict = 1;
            }
        }
        for (int i = 0; i < o.nbad; i++) {
            if (o.bad[i] != st.seed) {       /* the seed is the floor; it cannot go */
                add_bad(&st, o.bad[i]);
            }
        }
        if (cur >= 0 && in_list(st.bad, st.nbad, cur)) {
            long back = revert_target(&st, cur);
            verdict = 1;
            if (back >= 0 && flip(back) == 0) {
                say("it_boot: host judged %s%ld bad, reverted\n", "", cur);
                cur = back;
                st.tries = 0;
                result = R_REVERTED_BAD;
            } else {
                result = -ENOENT;
            }
        }

        long want = o.serial == 0 ? st.seed : o.serial;
        char build[32];
        os_build(build, sizeof(build));
        if (want < 0 || want == cur) {
            /* nothing to move to */
        } else if (in_list(st.bad, st.nbad, want)) {
            if (result == R_UNCHANGED) {
                result = R_REFUSED;
            }
        } else if (o.serial != 0 && o.build[0] && strcmp(build, o.build)) {
            say("it_boot: offer is for build %s\n", o.build, 0);
            say("it_boot: this is build %s\n", build, 0);
            result = -ENOEXEC;
        } else {
            int rc = 0, fresh = 0;
            if (!pkg_ok(want)) {
                rc = o.serial == 0 ? -ENOENT : install(&o, record, (size_t)len);
                fresh = 1;
            }
            if (!rc) {
                rc = flip(want);
            }
            if (rc) {
                say("it_boot: install of %s%ld failed, keeping current\n", "", want);
                say("it_boot: install error %s%ld\n", "", rc);
                result = rc;
            } else {
                if (cur >= 0 && !in_list(st.bad, st.nbad, cur)) {
                    st.previous = cur;
                }
                cur = want;
                st.tries = cur == st.seed || cur == st.good ? 0 : 1;
                verdict = 1;
                if (result == R_UNCHANGED) {
                    result = fresh ? R_INSTALLED : R_SWITCHED;
                }
            }
        }
        if (in_list(o.good, o.ngood, cur) && cur >= 0) {   /* judged before we switched to it */
            st.good = cur;
            st.tries = 0;
        }
        /* a package the host never judged gets MAX_TRIES boots */
        if (!verdict && cur >= 0 && cur != st.seed && cur != st.good) {
            if (st.tries >= MAX_TRIES) {
                long back = revert_target(&st, cur);
                if (back >= 0 && flip(back) == 0) {
                    say("it_boot: %s%ld never judged, reverted\n", "", cur);
                    add_bad(&st, cur);
                    cur = back;
                    st.tries = 0;
                    result = R_REVERTED_TRIES;
                }
            } else {
                st.tries++;
            }
        }
    }

    if (cur >= 0) {
        apply_hooks(&st, cur);
    }
    if (start != cur && start >= 0) {
        jobs(start, "unload");
    }
    if (cur >= 0) {
        jobs(cur, "load");
    }
    if (respring_needed) {
        launchctl("stop", "com.apple.SpringBoard");
    }
    if (save_state(&st)) {
        say("it_boot: cannot save state\n", "", 0);
    }
    sync();
    if (len > 0) {
        char line[160];
        int n = snprintf(line, sizeof(line), "prev %ld good %ld tries %ld seed %ld",
                         st.previous, st.good, st.tries, st.seed);
        qc(QC_PKG_REPORT, line, (uint32_t)result, (uint32_t)n, cur < 0 ? 0 : (uint64_t)cur);
    }
    say("it_boot: package %s%ld\n", "", cur);
    return result;
}

#ifndef IT_BOOT_TEST
/* LC_UNIXTHREAD enters here with no crt1: never return (armv6-toolchain/README.md). */
int main(void)
{
    it_boot_run();
    exit(0);
}
#endif
