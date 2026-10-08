/*
 * Assemble guest packages from built guest tools and pack them per arch.
 *
 *   mkpkg build SRC OUT     SRC: a tree the contrib/<component>/build.sh recipes ran in (build.sh here makes one).
 *                           Writes OUT/packages/<family>/{manifest.json, bin/, jobs/, hooks/} and OUT/<arch>.itpack
 *   mkpkg selfcheck ROOT    the itpack round trip, job rewrite, Mach-O check and FAMILIES table (ROOT: the checkout)
 *
 * A package is what contrib/it-boot/it_boot.c installs as /usr/local/lighttouch/pkgs/<serial>.
 * Its jobs run the package's binaries through /usr/local/lighttouch/current, so they are
 * rewritten here; hooks replace stock-path files (the preparer keeps a .baked copy of each).
 * The preparers' seed is FirmwareKit's GuestPackage.seed (LightTouchMac).
 *
 * The .itpack holds arbitrary files in one custom magic, a JSON index and one zlib stream, so the
 * notary service, which opens any archive it recognizes and rejects the unsigned guest Mach-Os
 * inside, sees neither.
 *
 * Built on use by build.sh: xcrun clang -O2 mkpkg.c -lz -framework CoreFoundation
 */
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define MAGIC "ITPACK01"
#define CURRENT "/usr/local/lighttouch/current"
#define MBX "/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine"
/* The GL front end replaces the framework binary whole: contrib/gles-public (every iPad build; one fat binary),
 * contrib/it-gles/gles1x.c (the iPod's 1.x) */
#define OPENGLES "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"
#define TYPEIN "/usr/lib/it_typein.dylib"
#define HOOK_PROVENANCE "file-or-absence 1\n"
#define LEGACY_LOADER "loader/it_boot-legacy" /* a legacy-linked family's loader, where the arch's own is modern */

typedef struct { const char *name, *src; } Bin;
typedef struct { const char *src, *target; int respring; } Hook;
typedef struct {
    const char *name, *arch;
    const char **boards, **builds; /* NULL-terminated */
    const Bin *bin;                /* NULL-name-terminated, or NULL */
    const char **jobs;
    const Hook *hooks;
} Family;

#define L(...) ((const char *[]){__VA_ARGS__, NULL})
#define IPOD_BIN ((const Bin[]){{"it_agent", "contrib/it-agent/it_agent"}, {"itmedia", "contrib/it-media/itmedia"}, \
    {"itphoto", "contrib/it-media/itphoto"}, {"ittrust", "contrib/it-proxy/ittrust"}, \
    {"itproxy", "contrib/it-proxy/itproxy"}, {"itstatus", "contrib/it-status/itstatus"}, \
    {"ithalt", "contrib/it-halt/ithalt"}, {"itorient", "contrib/it-orientation/itorient"}, \
    {"sbdlicon", "contrib/it-instprogress/sbdlicon"}, {"sblaunch", "contrib/it-gles/sblaunch"}, \
    {"it_prefs", "build/ipod-guest/it_prefs"}, {0}})
/* The 2.x/3.0 set shares the same legacy-linked helpers as later armv6 guests.
 * Media, networking and status tools retain their own qualification boundary. */
#define IPOD_LEGACY_BIN ((const Bin[]){{"it_agent", "contrib/it-agent/it_agent"}, \
    {"sblaunch", "contrib/it-gles/sblaunch"}, {"sbdlicon", "contrib/it-instprogress/sbdlicon"}, \
    {"it_prefs", "build/ipod-guest/it_prefs"}, {0}})
/* it_prefs (contrib/it-prefs/build-ipod.sh: no Wi-Fi location) runs from the package on every iPod 2G family,
 * so devices prepared with it baked get its later settings too. */
#define PREFS_JOB "contrib/it-prefs/com.qemu.it-prefs.plist"
#define IPOD_JOBS L("contrib/it-agent/com.qemu.it-agent.plist", PREFS_JOB)
/* it_agent (armv7, contrib/ipad1-guest/build.sh) replaces it_pbd from serial 2: the same pasteboard, plus the
 * foreground app, lock state, launch and sync that no stock service answers. Two pasteboard daemons would race. */
#define IPAD_BIN ((const Bin[]){{"it_agent", "build/ipad1-guest/it_agent"}, {"it_prefs", "build/ipad1-guest/it_prefs"}, {0}})
#define IPAD_JOBS IPOD_JOBS
/* armv7 on 3.0 (the 3GS's 7A341/7A400): the same helpers and hooks, legacy-linked against the 3.1.3 SDK
 * (guest-package/build.sh's ipad1-guest-legacy, appsync's libappsync-legacy.dylib) */
#define IPAD_LEGACY_BIN ((const Bin[]){{"it_agent", "build/ipad1-guest-legacy/it_agent"}, \
    {"it_prefs", "build/ipad1-guest-legacy/it_prefs"}, {0}})
/* it_msmquiet: the mounter has already loaded the previous shim when the hook changes, and a respring does not
 * drop a notice SpringBoard already holds (tested on 4.2.1), so the next boot's mounter is the one that changes. */
#define K48_HOOKS ((const Hook[]){{"contrib/gles-public/OpenGLES", OPENGLES, 1}, \
    {"build/ipad1-guest/it_msmquiet.dylib", "/usr/local/lib/it_msmquiet.dylib", 0}, \
    {"build/appsync/libappsync.dylib", "/usr/lib/libappsync.dylib", 0}, {0}})
#define N72_HOOKS ((const Hook[]){{"contrib/gles-public/OpenGLES", OPENGLES, 1}, \
    {"contrib/it-agent/it_typein.dylib", TYPEIN, 1}, {0}})
#define ARMV7_BOARDS L("k48ap", "n81ap", "n90ap", "n18ap", "n88ap")
#define K48_IOS3_BUILDS L("7B367", "7B405", "7B500", "7C144", "7C145", "7D11", "7E18")

/* hooks: (source, stock target, respring). The GL shims are one binary per arch: they read the
 * firmware's dispatch layout at load (contrib/it-gles/gles_dispatch.c), so no hook is per build.
 * builds: exact ids or "<major>*" for every build of that iOS major (2.x = 5*, 3.x = 7*, 4.x = 8*, 5.x = 9*), so a
 * new point release needs no row here (LightTouchMac docs/matrix.md). The order is the itpacks' entry order. */
static const Family FAMILIES[] = {
    /* 1.x builds are 3A* and 3B* (1.1-1.1.2) and 4A* and 4B* (1.1.3-1.1.5), and the iPhone's 1A* and 1C* (1.0-1.0.2): no agent
     * or helpers yet, the GL front end only; the legacy-linked it_boot runs there (armv6-toolchain crt1old.c/legacy.h:
     * 1.x's crt1 and stat ABI) */
    {"n45-ios1", "armv6", L("n45ap", "m68ap"), L("1*", "3*", "4*"), NULL, NULL,
     (const Hook[]){{"contrib/it-gles/OpenGLES-1x", OPENGLES, 1}, {0}}},
    {"n72-ios2", "armv6", L("n72ap"), L("5*"), IPOD_LEGACY_BIN, IPOD_JOBS, N72_HOOKS},
    /* 3.0 (7A341, the iPod 2G's only 3.0 build) has 3.1's engine ABI but 2.x's dyld (no LC_DYLD_INFO_ONLY):
     * the legacy-linked loader, engine and core helpers. Family by dyld capability, so 3.1+ are listed
     * by build (the iPod's 3.x series is closed). */
    {"n72-ios30", "armv6", L("n72ap"), L("7A341"), IPOD_LEGACY_BIN, IPOD_JOBS, N72_HOOKS},
    {"n72-ios3", "armv6", L("n72ap"), L("7C144", "7C145", "7D11", "7E18"), IPOD_BIN, IPOD_JOBS,
     (const Hook[]){{"contrib/gles-public/OpenGLES", OPENGLES, 1}, {"contrib/it-agent/it_typein.dylib", TYPEIN, 1},
                    {"build/appsync/libappsync.dylib", "/usr/lib/libappsync.dylib", 0}, {0}}},
    /* 4.x: it_prefs alone so far; the agent and the rest are still baked there. */
    {"n72-ios4", "armv6", L("n72ap"), L("8*"), (const Bin[]){{"it_prefs", "build/ipod-guest/it_prefs"}, {0}},
     L(PREFS_JOB), NULL},
    /* every armv7 board: the payloads read what differs per board at load, as they do per firmware. The family
     * names stay k48-*, as prepared devices record them in their locks.
     * 3.x by build: 3.0 (the 3GS's 7A*) has 2.x's dyld (no LC_DYLD_INFO_ONLY), so it is k48-ios30's, as the
     * iPod's 3.0 is n72-ios30's (the 3.x series is closed) */
    {"k48-ios3", "armv7", ARMV7_BOARDS, K48_IOS3_BUILDS, IPAD_BIN, IPAD_JOBS, K48_HOOKS},
    {"k48-ios30", "armv7", L("n88ap"), L("7A341", "7A400"), IPAD_LEGACY_BIN, IPAD_JOBS,
     (const Hook[]){{"contrib/gles-public/OpenGLES", OPENGLES, 1},
                    {"build/ipad1-guest-legacy/it_msmquiet.dylib", "/usr/local/lib/it_msmquiet.dylib", 0},
                    {"build/appsync/libappsync-legacy.dylib", "/usr/lib/libappsync.dylib", 0}, {0}}},
    /* 4.x and 5.x: 3.x's payloads byte for byte. The GL front end, agent and mounter shim read what each changed off
     * the firmware at load (docs/ipad1/gles-public-seam.md, docs/ipad1/ios5.md); only the build range differs. */
    {"k48-ios4", "armv7", ARMV7_BOARDS, L("8*"), IPAD_BIN, IPAD_JOBS, K48_HOOKS},
    {"k48-ios5", "armv7", ARMV7_BOARDS, L("9*"), IPAD_BIN, IPAD_JOBS, K48_HOOKS},
    /* 6.x: the boards that run it (the iPad and the iPod touch 3G stop at 5.1.1). */
    {"k48-ios6", "armv7", L("n81ap", "n90ap", "n88ap"), L("10*"), IPAD_BIN, IPAD_JOBS, K48_HOOKS},
    /* 7.x (the iPhone 4 only): a read-only root (its launchd cannot `mount -uw /`), so the package is baked at
     * prepare time (seed) and changes only on a re-prepare; it_boot installs, swaps hooks and writes state nowhere. */
    {"k48-ios7", "armv7", L("n90ap"), L("11*"), IPAD_BIN, IPAD_JOBS, K48_HOOKS},
};
#define NFAM (sizeof(FAMILIES) / sizeof(FAMILIES[0]))
/* 1.x/2.x dyld refuses LC_DYLD_INFO_ONLY; everything the loader runs on it must be legacy-linked */
static const char *LEGACY_BUILDS[] = {"1*", "3*", "4*", "5*", "7A341", "7A400", NULL};

#define MH_MAGIC_32 0xFEEDFACEu
#define FAT_MAGIC_32 0xCAFEBABEu
#define C_MAIN 0x80000028u
#define C_VERSION_MIN_IPHONEOS 0x25u
#define C_CODE_SIGNATURE 0x1Du
#define C_DYLD_INFO_ONLY 0x80000022u

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

typedef struct { uint8_t *p; size_t n; } Buf;

static Buf readfile(const char *path)
{
    Buf b = {0};
    FILE *f = fopen(path, "rb");
    if (!f)
        return b;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    b.p = malloc(n ? n : 1);
    if (n < 0 || fread(b.p, 1, n, f) != (size_t)n)
        die("%s: read failed", path);
    b.n = n;
    fclose(f);
    return b;
}

static void writefile(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f))
        die("%s: %s", path, strerror(errno));
}

static void mkdirs(const char *path)
{
    char p[4096];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0777);
            *s = '/';
        }
    if (mkdir(p, 0777) && errno != EEXIST)
        die("%s: %s", p, strerror(errno));
}

static void sha256(const void *p, size_t n, char out[65])
{
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(p, (CC_LONG)n, d);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* requires.builds membership: an exact id, or "<major>*" matching the build's leading number. */
static int build_matches(const char **builds, const char *build)
{
    size_t major = 0;
    while (isdigit((unsigned char)build[major]))
        major++;
    for (; *builds; builds++) {
        size_t n = strlen(*builds);
        if (!strcmp(*builds, build) ||
            (n && (*builds)[n - 1] == '*' && n - 1 == major && major && !strncmp(*builds, build, major)))
            return 1;
    }
    return 0;
}

static int family_legacy(const Family *f)
{
    for (const char **b = f->builds; *b; b++) {
        for (const char **l = LEGACY_BUILDS; *l; l++)
            if (!strcmp(*l, *b))
                return 1;
        if (build_matches(LEGACY_BUILDS, *b))
            return 1;
    }
    return 0;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

/* NULL if d is a Mach-O (or a fat one with a slice) for arch that old dyld takes (and, armv7 or signed,
 * signed). */
static const char *macho_problem(const uint8_t *d, size_t n, const char *arch, int legacy, int sign)
{
    static char why[128];
    int32_t want = strcmp(arch, "armv6") ? 9 : 6;
    if (n >= 8 && be32(d) == FAT_MAGIC_32) {
        for (uint32_t i = 0, nf = be32(d + 4); i < nf; i++) {
            size_t o = 8 + 20 * (size_t)i;
            if (o + 20 > n)
                return "truncated fat header";
            if ((int32_t)be32(d + o) == 12 && (int32_t)be32(d + o + 4) == want) {
                size_t a = be32(d + o + 8), b = a + be32(d + o + 12);
                a = a < n ? a : n;
                b = b < n ? b : n;
                return macho_problem(d + a, b > a ? b - a : 0, arch, legacy, sign);
            }
        }
        snprintf(why, sizeof why, "fat, without an %s slice", arch);
        return why;
    }
    if (n < 28 || le32(d) != MH_MAGIC_32)
        return "not a 32-bit Mach-O";
    int32_t cpu = le32(d + 4), sub = le32(d + 8);
    uint32_t filetype = le32(d + 12), ncmds = le32(d + 16);
    if (cpu != 12 || sub != want) {
        snprintf(why, sizeof why, "cpu %d/%d, not %s", cpu, sub, arch);
        return why;
    }
    int main_ = 0, vmin = 0, sig = 0, dyldinfo = 0;
    size_t off = 28;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (off + 8 > n)
            return "truncated load commands";
        uint32_t cmd = le32(d + off);
        main_ |= cmd == C_MAIN;
        vmin |= cmd == C_VERSION_MIN_IPHONEOS;
        sig |= cmd == C_CODE_SIGNATURE;
        dyldinfo |= cmd == C_DYLD_INFO_ONLY;
        off += le32(d + off + 4);
    }
    if (main_ || (filetype == 2 && vmin)) /* bundles keep theirs */
        return "LC_MAIN/LC_VERSION_MIN (not through mkold)";
    if (legacy && dyldinfo)
        return "LC_DYLD_INFO_ONLY (2.x dyld refuses it; LEGACY_LINK=1)";
    if (!sig && (!strcmp(arch, "armv7") || sign)) /* 3.2+ AMFI wants one; iPod helpers ship unsigned */
        return "unsigned (ldid -S)";
    return NULL;
}

static char *cfstr(CFTypeRef s)
{
    static char b[1024];
    if (!s || CFGetTypeID(s) != CFStringGetTypeID() || !CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8))
        return NULL;
    return b;
}

/* Point the job's program at the package, through the current symlink. The XML is what Python's
 * plistlib.dumps wrote (sorted keys, tabs), which CoreFoundation's writer matches. */
static Buf rewrite_job(Buf in)
{
    CFDataRef data = CFDataCreate(NULL, in.p, in.n);
    CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainersAndLeaves,
                                                        NULL, NULL);
    CFRelease(data);
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID())
        die("job: not a plist dictionary");
    CFMutableDictionaryRef job = (CFMutableDictionaryRef)pl;
    char label[256];
    snprintf(label, sizeof label, "%s", cfstr(CFDictionaryGetValue(job, CFSTR("Label"))) ?: "None");
    CFArrayRef args = CFDictionaryGetValue(job, CFSTR("ProgramArguments"));
    CFMutableArrayRef out = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFTypeRef exe;
    CFIndex rest = 1;
    if (args && CFGetTypeID(args) == CFArrayGetTypeID() && CFArrayGetCount(args)) {
        exe = CFArrayGetValueAtIndex(args, 0);
    } else {
        exe = CFDictionaryGetValue(job, CFSTR("Program"));
        args = NULL;
    }
    char *e = cfstr(exe);
    if (!e || strncmp(e, "/usr/local/bin/", 15))
        die("job %s: program '%s' is not a /usr/local/bin helper", label, e ?: "None");
    char path[1024];
    snprintf(path, sizeof path, CURRENT "/bin/%s", base(e));
    CFStringRef p = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFArrayAppendValue(out, p);
    CFRelease(p);
    for (CFIndex i = rest; args && i < CFArrayGetCount(args); i++)
        CFArrayAppendValue(out, CFArrayGetValueAtIndex(args, i));
    CFDictionaryRemoveValue(job, CFSTR("Program"));
    CFDictionarySetValue(job, CFSTR("ProgramArguments"), out);
    CFRelease(out);
    CFDataRef xml = CFPropertyListCreateData(NULL, job, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    if (!xml)
        die("job %s: cannot write the plist", label);
    Buf b = {malloc(CFDataGetLength(xml)), CFDataGetLength(xml)};
    memcpy(b.p, CFDataGetBytePtr(xml), b.n);
    CFRelease(xml);
    CFRelease(pl);
    return b;
}

/* JSON as Python's json.dump wrote it: indent=1 for manifests, ", "/": " separators for the index. */
static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c == '\n')
            fputs("\\n", f);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else if (c >= 0x80)
            die("non-ASCII name: %s", s); /* ponytail: json.dump's \u escapes for UTF-8 if a name ever needs them */
        else
            fputc(c, f);
    }
    fputc('"', f);
}

static void nl(FILE *f, int lvl)
{
    fputc('\n', f);
    for (int i = 0; i < lvl; i++)
        fputc(' ', f);
}

static void key(FILE *f, int lvl, const char *k, int first)
{
    if (!first)
        fputc(',', f);
    nl(f, lvl);
    jstr(f, k);
    fputs(": ", f);
}

static void strlist(FILE *f, int lvl, const char **v, int n)
{
    if (!n) {
        fputs("[]", f);
        return;
    }
    fputc('[', f);
    for (int i = 0; i < n; i++) {
        if (i)
            fputc(',', f);
        nl(f, lvl + 1);
        jstr(f, v[i]);
    }
    nl(f, lvl);
    fputc(']', f);
}

static int count(const char **v)
{
    int n = 0;
    while (v && v[n])
        n++;
    return n;
}

typedef struct { char *name; Buf data; } Entry;
typedef struct { Entry *e; int n, cap; } Entries;

static void push(Entries *es, const char *name, Buf data)
{
    if (es->n == es->cap)
        es->e = realloc(es->e, (es->cap = es->cap * 2 + 16) * sizeof(Entry));
    es->e[es->n++] = (Entry){strdup(name), data};
}

static int bincmp(const void *a, const void *b) { return strcmp(((const Bin *)a)->name, ((const Bin *)b)->name); }

typedef struct { char name[256]; unsigned mode; size_t size; char sha[65]; } FileRec;
typedef struct {
    const Family *fam;
    int legacy, nf;
    char dir[4096];
    FileRec files[64];
    Buf payload[64];
} Pkg;

static Buf read_src(const char *src, const Family *fam, const char *source)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", src, source);
    Buf b = readfile(path);
    if (!b.p)
        die("%s: %s was not built (see the logs)", fam->name, source);
    return b;
}

static void add(Pkg *k, const char *rel, Buf data, unsigned mode, int macho)
{
    const char *why = macho ? macho_problem(data.p, data.n, k->fam->arch, k->legacy, 0) : NULL;
    if (why)
        die("%s %s: %s", k->fam->name, rel, why);
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", k->dir, rel);
    *strrchr(path, '/') = 0;
    mkdirs(path);
    snprintf(path, sizeof path, "%s/%s", k->dir, rel);
    writefile(path, data.p, data.n);
    chmod(path, mode);
    FileRec *r = &k->files[k->nf];
    k->payload[k->nf++] = data;
    snprintf(r->name, sizeof r->name, "%s", rel);
    r->mode = mode;
    r->size = data.n;
    sha256(data.p, data.n, r->sha);
}

/* OUT/<family>/ with its payloads and manifest.json; appends the itpack entries (manifest first). */
static void assemble(const char *src, const char *out, const Family *fam, int serial, const char *version,
                     Entries *es)
{
    static Pkg k;
    memset(&k, 0, sizeof k);
    k.fam = fam;
    k.legacy = family_legacy(fam);
    int legacy = k.legacy;
    char cmd[4200];
    snprintf(k.dir, sizeof k.dir, "%s/%s", out, fam->name);
    snprintf(cmd, sizeof cmd, "/bin/rm -rf '%s'", k.dir);
    if (strchr(k.dir, '\'') || system(cmd))
        die("%s: cannot clear", k.dir);
    mkdirs(k.dir);
    const char *jobs[16], *provides[64];
    const Hook *hooks[16];
    int nj = 0, nh = 0, np = 0;

    Bin bins[32];
    int nb = 0;
    for (const Bin *b = fam->bin; b && b->name; b++)
        bins[nb++] = *b;
    qsort(bins, nb, sizeof *bins, bincmp);
    for (int i = 0; i < nb; i++) {
        char rel[256];
        snprintf(rel, sizeof rel, "bin/%s", bins[i].name);
        add(&k, rel, read_src(src, fam, bins[i].src), 0755, 1);
        provides[np++] = bins[i].name;
    }
    static char jobrel[16][256];
    for (const char **j = fam->jobs; j && *j; j++) {
        snprintf(jobrel[nj], sizeof jobrel[nj], "jobs/%s", base(*j));
        add(&k, jobrel[nj], rewrite_job(read_src(src, fam, *j)), 0644, 0);
        jobs[nj] = jobrel[nj];
        nj++;
    }
    static char hookrel[16][256];
    for (const Hook *h = fam->hooks; h && h->src; h++) {
        snprintf(hookrel[nh], sizeof hookrel[nh], "hooks/%s", base(h->target));
        Buf data = read_src(src, fam, h->src);
        const char *why;
        if ((!strcmp(h->target, MBX) || !strcmp(h->target, OPENGLES) || !strcmp(h->target, TYPEIN)) &&
            strcmp(fam->name, "n45-ios1") && (why = macho_problem(data.p, data.n, fam->arch, legacy, 1)))
            /* GL maps into every GL process, SpringBoard's included (3.0's even with software CA), and a
             * signed process is killed at an unsigned library's first page (1.x predates code signing) */
            die("%s %s: %s", fam->name, hookrel[nh], why);
        add(&k, hookrel[nh], data, 0755, 1);
        hooks[nh++] = h;
        provides[np++] = base(h->target);
    }
    const FileRec *files = k.files;
    const Buf *payload = k.payload;
    const char *pkg = k.dir;
    int nf = k.nf;

    char *m = NULL;
    size_t mlen = 0;
    FILE *f = open_memstream(&m, &mlen);
    fprintf(f, "{");
    key(f, 1, "format", 1);
    fprintf(f, "1");
    key(f, 1, "serial", 0);
    fprintf(f, "%d", serial);
    key(f, 1, "version", 0);
    jstr(f, version);
    key(f, 1, "family", 0);
    jstr(f, fam->name);
    key(f, 1, "arch", 0);
    jstr(f, fam->arch);
    key(f, 1, "stub", 0);
    fprintf(f, "false");
    key(f, 1, "requires", 0);
    fprintf(f, "{");
    key(f, 2, "boards", 1);
    strlist(f, 2, fam->boards, count(fam->boards));
    key(f, 2, "builds", 0);
    strlist(f, 2, fam->builds, count(fam->builds));
    key(f, 2, "link", 0);
    jstr(f, legacy ? "legacy" : "modern");
    /* host protocol ranges a package speaks: [oldest, newest]; gles 1 = the name-keyed wire (gles-names.h) */
    key(f, 2, "host", 0);
    fprintf(f, "{");
    const char *proto[] = {"guest-package", "gles"};
    for (int i = 0; i < 2; i++) {
        key(f, 3, proto[i], !i);
        fprintf(f, "[");
        nl(f, 4);
        fprintf(f, "1,");
        nl(f, 4);
        fprintf(f, "1");
        nl(f, 3);
        fprintf(f, "]");
    }
    nl(f, 2);
    fprintf(f, "}");
    nl(f, 1);
    fprintf(f, "}");
    key(f, 1, "provides", 0);
    strlist(f, 1, provides, np);
    key(f, 1, "files", 0);
    fprintf(f, "[");
    for (int i = 0; i < nf; i++) {
        fputs(i ? "," : "", f);
        nl(f, 2);
        fprintf(f, "{");
        key(f, 3, "name", 1);
        jstr(f, files[i].name);
        key(f, 3, "mode", 0);
        fprintf(f, "\"%o\"", files[i].mode);
        key(f, 3, "size", 0);
        fprintf(f, "%zu", files[i].size);
        key(f, 3, "sha256", 0);
        jstr(f, files[i].sha);
        nl(f, 2);
        fprintf(f, "}");
    }
    if (nf)
        nl(f, 1);
    fprintf(f, "]");
    key(f, 1, "jobs", 0);
    strlist(f, 1, jobs, nj);
    key(f, 1, "hooks", 0);
    fprintf(f, "[");
    for (int i = 0; i < nh; i++) {
        fputs(i ? "," : "", f);
        nl(f, 2);
        fprintf(f, "{");
        key(f, 3, "file", 1);
        jstr(f, hookrel[i]);
        key(f, 3, "target", 0);
        jstr(f, hooks[i]->target);
        key(f, 3, "respring", 0);
        fputs(hooks[i]->respring ? "true" : "false", f);
        nl(f, 2);
        fprintf(f, "}");
    }
    if (nh)
        nl(f, 1);
    fprintf(f, "]\n}\n");
    fclose(f);

    char path[4096], name[512];
    snprintf(path, sizeof path, "%s/manifest.json", pkg);
    writefile(path, m, mlen);
    snprintf(name, sizeof name, "%s/manifest.json", fam->name);
    push(es, name, (Buf){(uint8_t *)m, mlen});
    for (int i = 0; i < nf; i++) {
        snprintf(name, sizeof name, "%s/%s", fam->name, files[i].name);
        push(es, name, payload[i]);
    }
    printf("%-9s %s serial %d: %d files, %d jobs, %d hooks\n", fam->name, fam->arch, serial, nf, nj, nh);
}

/* entries -> out: MAGIC, the index's length (LE), the JSON index, one zlib stream (level 9) of every entry. */
static void pack(const Entries *es, const char *out)
{
    char *idx = NULL;
    size_t ilen = 0;
    FILE *f = open_memstream(&idx, &ilen);
    fprintf(f, "{\"format\": 1, \"entries\": [");
    for (int i = 0; i < es->n; i++) {
        fprintf(f, i ? ", {\"name\": " : "{\"name\": ");
        jstr(f, es->e[i].name);
        fprintf(f, ", \"size\": %zu}", es->e[i].data.n);
    }
    fprintf(f, "]}");
    fclose(f);

    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *o = fopen(tmp, "wb");
    uint8_t len[4] = {ilen, ilen >> 8, ilen >> 16, ilen >> 24};
    if (!o || fwrite(MAGIC, 1, 8, o) != 8 || fwrite(len, 1, 4, o) != 4 || fwrite(idx, 1, ilen, o) != ilen)
        die("%s: write failed", tmp);
    /* as zlib.compressobj(9): one compress() per entry, then flush() */
    z_stream z = {0};
    if (deflateInit(&z, 9) != Z_OK)
        die("deflateInit");
    static uint8_t chunk[1 << 16];
    size_t total = 0;
    for (int i = 0; i <= es->n; i++) {
        int flush = i == es->n ? Z_FINISH : Z_NO_FLUSH;
        z.next_in = i < es->n ? es->e[i].data.p : NULL;
        z.avail_in = i < es->n ? (uInt)es->e[i].data.n : 0;
        int rc;
        do {
            z.next_out = chunk;
            z.avail_out = sizeof chunk;
            rc = deflate(&z, flush);
            if (rc == Z_STREAM_ERROR)
                die("deflate");
            fwrite(chunk, 1, sizeof chunk - z.avail_out, o);
        } while (z.avail_out == 0 || (flush == Z_FINISH && rc != Z_STREAM_END));
        if (i < es->n)
            total += es->e[i].data.n;
    }
    deflateEnd(&z);
    if (fclose(o))
        die("%s: write failed", tmp);
    if (rename(tmp, out))
        die("%s: %s", out, strerror(errno));

    /* the round trip: the index as written and one stream that is every entry, in order, and nothing else */
    Buf blob = readfile(out);
    if (blob.n < 12 || memcmp(blob.p, MAGIC, 8) || le32(blob.p + 8) != ilen || blob.n < 12 + ilen ||
        memcmp(blob.p + 12, idx, ilen))
        die("%s: round trip mismatch", out);
    uint8_t *data = malloc(total + 1);
    uLongf dlen = total + 1;
    if (uncompress(data, &dlen, blob.p + 12 + ilen, blob.n - 12 - ilen) != Z_OK || dlen != total)
        die("%s: round trip mismatch", out);
    size_t off = 0;
    for (int i = 0; i < es->n; i++) {
        const char *n = es->e[i].name;
        if (n[0] == '/' || !strcmp(n, "..") || !strncmp(n, "../", 3) || strstr(n, "/../") ||
            (strlen(n) >= 3 && !strcmp(n + strlen(n) - 3, "/..")))
            die("%s: bad entry name '%s'", out, n);
        if (memcmp(data + off, es->e[i].data.p, es->e[i].data.n))
            die("%s: round trip mismatch", out);
        off += es->e[i].data.n;
    }
    free(data);
    free(blob.p);
    free(idx);
}

static void loader(const char *src, const char *dir, const char *arch, int legacy, const char *label, Entries *es,
                   const char *name)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/build/it-boot/%s/it_boot", src, dir);
    Buf b = readfile(path);
    if (!b.p)
        die("%s: %s", path, strerror(errno));
    const char *why = macho_problem(b.p, b.n, arch, legacy, 0);
    if (why)
        die("loader/%s: %s", label, why);
    push(es, name, b);
}

static void build(const char *src, const char *out)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/contrib/guest-package/VERSION", src);
    FILE *v = fopen(path, "r");
    if (!v)
        die("%s: %s", path, strerror(errno));
    int serial = -1;
    char version[128] = "", line[256];
    while (fgets(line, sizeof line, v)) {
        char k[64], val[128];
        if (sscanf(line, "%63s %127[^\n]", k, val) == 2) {
            if (!strcmp(k, "serial"))
                serial = atoi(val);
            else if (!strcmp(k, "version")) {
                size_t n = strlen(val);
                while (n && isspace((unsigned char)val[n - 1]))
                    val[--n] = 0;
                snprintf(version, sizeof version, "%s", val);
            }
        }
    }
    fclose(v);
    if (serial < 0 || !*version)
        die("%s: no serial or version", path);
    char packages[4096];
    snprintf(packages, sizeof packages, "%s/packages", out);
    mkdirs(packages);
    Entries by_arch[2] = {{0}};   /* armv6, armv7: sorted, as the itpacks are written */
    const char *arch[2] = {"armv6", "armv7"};
    for (size_t i = 0; i < NFAM; i++)
        assemble(src, packages, &FAMILIES[i], serial, version, &by_arch[!strcmp(FAMILIES[i].arch, "armv7")]);
    for (int a = 0; a < 2; a++) {
        Entries *es = &by_arch[a];
        if (!es->n)
            continue;
        loader(src, arch[a], arch[a], a == 0, arch[a], es, "loader/it_boot");
        /* armv7's loader is modern-linked; its legacy families (k48-ios30) get the legacy-linked one (seed picks) */
        if (a == 1)
            loader(src, "armv7-legacy", "armv7", 1, "armv7-legacy", es, LEGACY_LOADER);
        push(es, "loader/hook-provenance", (Buf){(uint8_t *)HOOK_PROVENANCE, strlen(HOOK_PROVENANCE)});
        snprintf(path, sizeof path, "%s/build/it-boot/%s/com.qemu.it-boot.plist", src, arch[a]);
        Buf plist = readfile(path);
        if (!plist.p)
            die("%s: %s", path, strerror(errno));
        push(es, "loader/com.qemu.it-boot.plist", plist);
        snprintf(path, sizeof path, "%s/%s.itpack", out, arch[a]);
        pack(es, path);
        struct stat st;
        stat(path, &st);
        printf("%s.itpack: %d entries, %lld bytes\n", arch[a], es->n, (long long)st.st_size);
    }
}

/* selfcheck: what the build relies on, against the real FAMILIES table and formats. */
#define CHECK(c)                                                                                       \
    do {                                                                                               \
        if (!(c))                                                                                      \
            die("selfcheck: line %d: %s", __LINE__, #c);                                               \
    } while (0)

static const Family *fam(const char *name)
{
    for (size_t i = 0; i < NFAM; i++)
        if (!strcmp(FAMILIES[i].name, name))
            return &FAMILIES[i];
    die("no family %s", name);
    return NULL;
}

static int has(const char **v, const char *s)
{
    for (; v && *v; v++)
        if (!strcmp(*v, s))
            return 1;
    return 0;
}

static const char *bin_src(const Family *f, const char *name)
{
    for (const Bin *b = f->bin; b && b->name; b++)
        if (!strcmp(b->name, name))
            return b->src;
    return NULL;
}

static int is_gl(const char *t) { return !strcmp(t, MBX) || !strcmp(t, OPENGLES); }

/* the one family for board and build */
static const char *one_family(const char *board, const char *build)
{
    const char *found = NULL;
    for (size_t i = 0; i < NFAM; i++)
        if (has(FAMILIES[i].boards, board) && build_matches(FAMILIES[i].builds, build)) {
            if (found)
                die("selfcheck: %s %s: %s and %s", board, build, found, FAMILIES[i].name);
            found = FAMILIES[i].name;
        }
    return found ? found : "(none)";
}

static CFArrayRef job_args(Buf xml, char *label, size_t n)
{
    CFDataRef d = CFDataCreate(NULL, xml.p, xml.n);
    CFDictionaryRef job = CFPropertyListCreateWithData(NULL, d, 0, NULL, NULL);
    CFRelease(d);
    CHECK(job && CFGetTypeID(job) == CFDictionaryGetTypeID());
    snprintf(label, n, "%s", cfstr(CFDictionaryGetValue(job, CFSTR("Label"))) ?: "");
    return CFDictionaryGetValue(job, CFSTR("ProgramArguments"));
}

static void selfcheck(const char *root)
{
    char t[] = "/tmp/mkpkg-selfcheck.XXXXXX", path[4096], label[256];
    CHECK(mkdtemp(t));
    /* the itpack round trip (pack re-reads it) and opacity: payload bytes are only in the stream */
    static uint8_t xs[5000];
    memset(xs, 'x', sizeof xs);
    char sha[65];
    sha256(xs, sizeof xs, sha);
    char man[256];
    snprintf(man, sizeof man, "{\"files\": [{\"name\": \"bin/x\", \"mode\": \"755\", \"sha256\": \"%s\"}]}", sha);
    Entries es = {0};
    push(&es, "a/manifest.json", (Buf){(uint8_t *)man, strlen(man)});
    push(&es, "a/bin/x", (Buf){xs, sizeof xs});
    push(&es, "loader/it_boot", (Buf){(uint8_t *)"\xfe\xed", 2});
    snprintf(path, sizeof path, "%s/t.itpack", t);
    pack(&es, path);
    Buf blob = readfile(path);
    CHECK(memmem(blob.p, blob.n < 200 ? blob.n : 200, "bin/x", 5) && !memmem(blob.p, blob.n, "xxxx", 4));
    unlink(path);
    rmdir(t);

    /* the job rewrite */
    const char *job = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
                      "<key>Label</key><string>l</string><key>ProgramArguments</key><array>"
                      "<string>/usr/local/bin/it_pbd</string><string>-v</string></array></dict></plist>\n";
    CFArrayRef args = job_args(rewrite_job((Buf){(uint8_t *)job, strlen(job)}), label, sizeof label);
    CHECK(args && CFArrayGetCount(args) == 2);
    CHECK(!strcmp(cfstr(CFArrayGetValueAtIndex(args, 0)), CURRENT "/bin/it_pbd"));
    CHECK(!strcmp(cfstr(CFArrayGetValueAtIndex(args, 1)), "-v"));

    /* the Mach-O check */
    uint8_t thin[28] = {0xce, 0xfa, 0xed, 0xfe, 12, 0, 0, 0, 9, 0, 0, 0, 2};
    CHECK(!strcmp(macho_problem(thin, 28, "armv7", 0, 0) ?: "", "unsigned (ldid -S)"));
    CHECK(!strcmp(macho_problem(thin, 28, "armv6", 0, 0) ?: "", "cpu 12/9, not armv6"));
    uint8_t thin6[28] = {0xce, 0xfa, 0xed, 0xfe, 12, 0, 0, 0, 6, 0, 0, 0, 8};
    CHECK(!macho_problem(thin6, 28, "armv6", 0, 0));
    CHECK(!strcmp(macho_problem(thin6, 28, "armv6", 0, 1) ?: "", "unsigned (ldid -S)"));

    /* every shipped iPad build has exactly one family, and each carries the agent and the one GL front end */
    const char *ipad[][2] = {{"7B500", "k48-ios3"}, {"8C148", "k48-ios4"}, {"8L1", "k48-ios4"},
                             {"9B206", "k48-ios5"}, {"10B329", "k48-ios6"}, {"10B500", "k48-ios6"},
                             {"11D257", "k48-ios7"}};
    for (int i = 0; i < 7; i++) {
        const Family *w = fam(ipad[i][1]);
        for (const char **b = w->boards; *b; b++)
            CHECK(!strcmp(one_family(*b, ipad[i][0]), ipad[i][1]));
        CHECK(bin_src(w, "it_agent"));
        int gl = 0;
        for (const Hook *h = w->hooks; h->src; h++)
            if (is_gl(h->target))
                CHECK(gl++ == 0 && !strcmp(h->src, "contrib/gles-public/OpenGLES") && !strcmp(h->target, OPENGLES));
        CHECK(gl == 1);
    }
    /* the 3GS's 3.0 is the legacy-linked armv7 family; 3.1+ stay modern */
    const char *n88[][2] = {{"7A341", "k48-ios30"}, {"7A400", "k48-ios30"}, {"7C144", "k48-ios3"}, {"7E18", "k48-ios3"}};
    for (int i = 0; i < 4; i++)
        CHECK(!strcmp(one_family("n88ap", n88[i][0]), n88[i][1]));
    const Family *k30 = fam("k48-ios30"), *k3 = fam("k48-ios3");
    int nb = 0;
    for (const Bin *b = k30->bin; b->name; b++, nb++)
        CHECK(bin_src(k3, b->name) && strstr(b->src, "legacy"));
    CHECK(nb == 2);
    const Hook *h30 = k30->hooks, *h3 = k3->hooks;
    for (; h30->src && h3->src; h30++, h3++)
        CHECK(!strcmp(h30->target, h3->target));
    CHECK(!h30->src && !h3->src);
    for (const char **b = k30->builds; *b; b++)
        CHECK(build_matches(LEGACY_BUILDS, *b));
    CHECK(family_legacy(k30) && !family_legacy(k3));
    /* every iPod 2G build has one family; 3.0's is legacy-linked (its dyld is 2.x's) and carries the engine */
    const char *n72[][2] = {{"5F138", "n72-ios2"}, {"7A341", "n72-ios30"}, {"7C145", "n72-ios3"},
                            {"7E18", "n72-ios3"}, {"8C148", "n72-ios4"}};
    for (int i = 0; i < 5; i++)
        CHECK(!strcmp(one_family("n72ap", n72[i][0]), n72[i][1]));
    CHECK(build_matches(LEGACY_BUILDS, "7A341") && !build_matches(LEGACY_BUILDS, "7E18"));
    const char *old[] = {"n72-ios2", "n72-ios30"};
    for (int i = 0; i < 2; i++) {
        const Family *f = fam(old[i]);
        nb = 0;
        for (const Bin *b = f->bin; b->name; b++, nb++)
            ;
        CHECK(nb == 4 && bin_src(f, "it_agent") && bin_src(f, "sblaunch") && bin_src(f, "sbdlicon") &&
              bin_src(f, "it_prefs"));
        CHECK(count(f->jobs) == 2 && !strcmp(f->jobs[0], "contrib/it-agent/com.qemu.it-agent.plist") &&
              !strcmp(f->jobs[1], PREFS_JOB));
        CHECK(f->hooks[0].src && !strcmp(f->hooks[0].target, OPENGLES) && f->hooks[1].src &&
              !strcmp(f->hooks[1].target, TYPEIN) && !f->hooks[2].src);
        /* Older guests have no native media qualification yet. */
        CHECK(!bin_src(f, "itmedia"));
        CHECK(family_legacy(f));
    }
    /* it_prefs and its job ride every iPod 2G family: one delivery path, existing devices included */
    for (size_t i = 0; i < NFAM; i++)
        if (has(FAMILIES[i].boards, "n72ap"))
            CHECK(bin_src(&FAMILIES[i], "it_prefs") &&
                  !strcmp(bin_src(&FAMILIES[i], "it_prefs"), "build/ipod-guest/it_prefs") &&
                  has(FAMILIES[i].jobs, PREFS_JOB));
    /* the baked com.qemu.it-prefs job would keep the package's from loading */
    snprintf(path, sizeof path, "%s/" PREFS_JOB, root);
    Buf prefs = readfile(path);
    CHECK(prefs.p);
    job_args(rewrite_job(prefs), label, sizeof label);
    CHECK(!strcmp(label, "com.qemu.guest-prefs"));
    /* the one GL front end, byte for byte, wherever a family hooks GL (1.x's is its own: no EAGL, old ObjC) */
    for (size_t i = 0; i < NFAM; i++)
        for (const Hook *h = FAMILIES[i].hooks; h && h->src; h++)
            if (is_gl(h->target))
                CHECK(!strcmp(h->src, strcmp(FAMILIES[i].name, "n45-ios1") ? "contrib/gles-public/OpenGLES"
                                                                              : "contrib/it-gles/OpenGLES-1x"));
    printf("PASS: itpack round trip and opacity, job rewrite, Mach-O check, "
           "one family per armv7 board and iPod 2G build\n");
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "build"))
        build(argv[2], argv[3]);
    else if (argc == 3 && !strcmp(argv[1], "selfcheck"))
        selfcheck(argv[2]);
    else
        die("usage: mkpkg build SRC OUT | mkpkg selfcheck ROOT");
    return 0;
}
