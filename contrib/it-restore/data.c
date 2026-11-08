/* Native N42 initial Data preparation: original newfs_hfs/MobileKeyBag,
 * stock /private/var plus FirmwareKit seeds, followed by native remount checks.
 * This runs only in the temporary original restore ramdisk, never on System. */
#include <dlfcn.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "mobilegestalt.h"

typedef unsigned int mach_port_t, io_object_t;
typedef int kern_return_t;
extern mach_port_t mach_task_self_;
extern int reboot(int);
extern int unmount(const char *, int);

#define RB_HALT 0x08
#define RB_QUICK 0x400 /* skip proc_shutdown; caches are still synced */
/* The iPad's data volume is disk0s2; the iPod's generated NAND is one volume, disk0s1, with the data
 * directory at /private/var (contrib/it-keybag/build-ipod.sh overrides these). */
#ifndef DATA_DEV
#define DATA_DEV "/dev/disk0s2"
#define DATA_MNT "/mnt2"
#define DATA_DIR DATA_MNT
#endif
#define KEYBAG DATA_DIR "/keybags/systembag.kb"

static FILE *con;
/* Copy children explicitly: copyfile(RECURSIVE) would nest var below an
 * existing Data root. Keep native root protection metadata and merge keybags. */
static unsigned seed_records;
static int copy_seed(const char *source, const char *target)
{
    struct stat before, after;
    if (lstat(source, &before))
        return -1;
    if (S_ISDIR(before.st_mode)) {
        if (mkdir(target, 0755) && errno != EEXIST)
            return -1;
        DIR *directory = opendir(source);
        if (!directory)
            return -1;
        struct dirent *entry;
        int failed = 0;
        while ((entry = readdir(directory))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char from[1024], to[1024];
            if (snprintf(from, sizeof(from), "%s/%s", source, entry->d_name) >=
                    sizeof(from) ||
                snprintf(to, sizeof(to), "%s/%s", target, entry->d_name) >=
                    sizeof(to)) {
                errno = ENAMETOOLONG;
                failed = -1;
                break;
            }
            if (copy_seed(from, to)) {
                failed = -1;
                break;
            }
        }
        closedir(directory);
        if (failed)
            return -1;
        if (chown(target, before.st_uid, before.st_gid) ||
            chmod(target, before.st_mode & 07777))
            return -1;
    } else if (copyfile(source, target, NULL,
                        COPYFILE_ALL | COPYFILE_NOFOLLOW_SRC)) {
        return -1;
    }
    if (lstat(target, &after) || before.st_uid != after.st_uid ||
        before.st_gid != after.st_gid ||
        (before.st_mode & 07777) != (after.st_mode & 07777) ||
        ((before.st_mode ^ after.st_mode) & S_IFMT)) {
        errno = EIO;
        return -1;
    }
    seed_records++;
    return 0;
}
static void report_keybag(const char *stage)
{
    unsigned char bytes[4096];
    FILE *f = fopen(KEYBAG, "rb");
    if (!f) {
        fprintf(con, "A6NATIVEFORMAT %s keybag open failed\n", stage);
        return;
    }
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    fclose(f);
    unsigned tag = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        tag = (tag ^ bytes[i]) * 16777619u;
    }
    fprintf(con, "A6NATIVEFORMAT %s bytes=%u header=", stage, (unsigned)n);
    for (unsigned i = 0; i < 8 && i < n; i++) {
        fprintf(con, "%02x", bytes[i]);
    }
    fprintf(con, " bplist=%u tag=%08x\n",
            n >= 8 && !memcmp(bytes, "bplist00", 8), tag);
    fflush(con);
}

#ifdef A6_PREPARED_DATA_SEED
static int verify_seed(const char *source, const char *target)
{
    struct stat a, b;
    if (lstat(source, &a) || lstat(target, &b) || a.st_uid != b.st_uid ||
        a.st_gid != b.st_gid || (a.st_mode & 07777) != (b.st_mode & 07777) ||
        ((a.st_mode ^ b.st_mode) & S_IFMT))
        return -1;
    if (S_ISDIR(a.st_mode)) {
        DIR *dir = opendir(source);
        if (!dir)
            return -1;
        struct dirent *entry;
        int failed = 0;
        while ((entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char from[1024], to[1024];
            if (snprintf(from, sizeof from, "%s/%s", source, entry->d_name) >=
                    sizeof from ||
                snprintf(to, sizeof to, "%s/%s", target, entry->d_name) >=
                    sizeof to ||
                verify_seed(from, to)) {
                failed = -1;
                break;
            }
        }
        closedir(dir);
        return failed;
    }
    if (S_ISLNK(a.st_mode)) {
        char x[1024], y[1024];
        ssize_t n = readlink(source, x, sizeof x),
                m = readlink(target, y, sizeof y);
        return n < 0 || m != n || n == sizeof x || memcmp(x, y, n) ? -1 : 0;
    }
    if (!S_ISREG(a.st_mode) || a.st_size != b.st_size)
        return -1;
    int src = open(source, O_RDONLY), dst = open(target, O_RDONLY), failed = 0;
    if (src < 0 || dst < 0)
        failed = -1;
    static unsigned char x[4096], y[4096];
    while (!failed) {
        ssize_t n = read(src, x, sizeof x), m = n > 0 ? read(dst, y, n) : 0;
        if (n < 0 || m != n || (n > 0 && memcmp(x, y, n))) {
            failed = -1;
            break;
        }
        if (!n)
            break;
    }
    if (src >= 0)
        close(src);
    if (dst >= 0)
        close(dst);
    return failed;
}
#endif

static void halt(const char *result)
{
    fprintf(con, "it_keybag: %s; halting\n", result);
    fflush(con);
    sync();
    /* RB_QUICK: a plain RB_HALT first SIGTERMs every process; the ramdisk's
     * launchd then saw its job die and raced us with its own
     * reboot(RB_AUTOBOOT), and the two shutdowns ended in an IOP panic
     * ("ARM7M: timed out waiting for workloop") instead of a halt. The data
     * volume is already unmounted, so nothing needs a graceful exit. */
    reboot(RB_HALT | RB_QUICK);
}

int it_prepare_data(void)
{
    struct stat st;
    int i, status;

    con = fopen("/dev/console", "w");
    if (!con)
        con = stderr;
    void *iok =
        dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
               RTLD_NOW);
    void *mkb = dlopen(
        "/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",
        RTLD_NOW);
    void *(*Matching)(const char *) = iok ? dlsym(iok, "IOServiceMatching") : 0;
    io_object_t (*GetService)(mach_port_t, void *) =
        iok ? dlsym(iok, "IOServiceGetMatchingService") : 0;
    kern_return_t (*Open)(io_object_t, mach_port_t, uint32_t, io_object_t *) =
        iok ? dlsym(iok, "IOServiceOpen") : 0;
    kern_return_t (*Scalar)(io_object_t, uint32_t, const uint64_t *, uint32_t,
                            uint64_t *, uint32_t *) =
        iok ? dlsym(iok, "IOConnectCallScalarMethod") : 0;
    int (*Create)(void *, const char *) =
        mkb ? dlsym(mkb, "MKBKeyBagCreateSystem") : 0;
    kern_return_t (*Services)(mach_port_t, void *, io_object_t *) =
        iok ? dlsym(iok, "IOServiceGetMatchingServices") : 0;
    io_object_t (*Next)(io_object_t) = iok ? dlsym(iok, "IOIteratorNext") : 0;
    kern_return_t (*Name)(io_object_t, char *) =
        iok ? dlsym(iok, "IORegistryEntryGetName") : 0;
    kern_return_t (*Class)(io_object_t, char *) =
        iok ? dlsym(iok, "IOObjectGetClass") : 0;
    kern_return_t (*Release)(io_object_t) =
        iok ? dlsym(iok, "IOObjectRelease") : 0;
    if (!Matching || !GetService || !Open || !Scalar || !Create) {
        fprintf(con, "it_keybag: %s\n", dlerror());
        halt("FAILED: IOKit / MobileKeyBag symbols missing");
        return 1;
    }

    /* The FTL and the effaceable driver come up after the RAM-disk root. */
    io_object_t svc = 0, conn = 0;
    for (i = 0; i < 120 &&
                (stat(DATA_DEV, &st) ||
                 !(svc = GetService(0, Matching("AppleEffaceableStorage"))));
         i++) {
        if (!(i % 15) && Services && Next && Name && Class && Release) {
            const char *classes[] = {"IOFlashMedia", "IOFlashStoragePartition",
                                     "AppleEffaceableStorage"};
            unsigned c;
            for (c = 0; c < 3; c++) {
                io_object_t iter = 0, item;
                kern_return_t r = Services(0, Matching(classes[c]), &iter);
                fprintf(con, "a6_keybag: inventory %s at %ds result=%x\n",
                        classes[c], i, r);
                if (!r) {
                    while ((item = Next(iter))) {
                        char name[128] = {0}, kind[128] = {0};
                        Name(item, name);
                        Class(item, kind);
                        fprintf(con, "a6_keybag: service name=%s class=%s\n",
                                name, kind);
                        Release(item);
                    }
                    Release(iter);
                }
            }
            fflush(con);
        }
        sleep(1);
    }
    if (!svc || stat(DATA_DEV, &st)) {
        halt(svc ? "FAILED: no " DATA_DEV
                 : "FAILED: no AppleEffaceableStorage");
        return 1;
    }
    kern_return_t kr = Open(svc, mach_task_self_, 0, &conn);
    uint64_t fmt = 0;
    uint32_t n = 1;
    kern_return_t k3 = kr ? kr : Scalar(conn, 3, NULL, 0, &fmt, &n);
    fprintf(con, "it_keybag: effaceable open %x isFormatted %x -> %llu\n", kr,
            k3, fmt);
    if (!k3 && !fmt) {
        n = 0;
        fprintf(con, "it_keybag: format %x\n",
                Scalar(conn, 4, NULL, 0, NULL, &n));
        n = 1;
        k3 = Scalar(conn, 3, NULL, 0, &fmt, &n);
    }
    fflush(con);
    if (k3 || !fmt) {
        halt("FAILED: effaceable storage not formatted");
        return 1;
    }

    fprintf(con, "A6NATIVEFORMAT START\n");
    fflush(con);
    pid_t pid = vfork();
    if (!pid) {
        execl("/sbin/newfs_hfs", "newfs_hfs", "-s", "-J", "8m", "-P", "-v",
              "Data", "/dev/rdisk0s2", (char *)0);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || status) {
        halt("FAILED: native newfs_hfs");
        return 1;
    }
    fprintf(con, "A6NATIVEFORMAT newfs completed\n");
    fflush(con);
    mkdir("/mnt1", 0755);
    pid = vfork();
    if (!pid) {
        execl("/sbin/mount_hfs", "mount_hfs", "-o", "ro", "/dev/disk0s1",
              "/mnt1", (char *)0);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || status) {
        halt("FAILED: readonly System mount");
        return 1;
    }
    pid = vfork();
    if (pid == 0) {
        execl("/sbin/mount_hfs", "mount_hfs", DATA_DEV, DATA_MNT, (char *)0);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || status) {
        halt("FAILED: mount_hfs " DATA_DEV " " DATA_MNT);
        return 1;
    }
    mkdir(DATA_MNT "/keybags", 0755);
    int r = Create(NULL, DATA_DIR);
    fprintf(
        con,
        "A6NATIVEFORMAT MKB initialization before protected seed writes -> %d\n",
        r);
    fflush(con);
    if (r) {
        halt("FAILED: native MKB initialization");
        return 1;
    }
    fprintf(
        con,
        "A6NATIVEFORMAT copying original Data seed using native libcopyfile\n");
    fflush(con);
    if (copy_seed("/mnt1/private/var", DATA_MNT)) {
        perror("A6NATIVEFORMAT copyfile");
        halt("FAILED: native Data seed copy");
        return 1;
    }
#ifdef A6_PREPARED_DATA_SEED
    /* Creation probe: seeds prepared by FirmwareKit are materialized only
     * after the native keybag exists. Never import a previous system keybag. */
    if (!lstat(A6_PREPARED_DATA_SEED "/keybags/systembag.kb", &st) ||
        errno != ENOENT) {
        halt("FAILED: prepared seed contains a keybag");
        return 1;
    }
    if (copy_seed(A6_PREPARED_DATA_SEED, DATA_MNT)) {
        perror("A6NATIVEFORMAT prepared seed");
        halt("FAILED: prepared Data seed copy");
        return 1;
    }
    fprintf(
        con,
        "A6NATIVEFORMAT FirmwareKit Data seed copied after native keybag creation\n");
    fflush(con);
#endif
    const char *required[] = {
        DATA_MNT "/tmp", DATA_MNT "/mobile/Library/Caches", DATA_MNT "/log"};
    for (unsigned check = 0; check < sizeof(required) / sizeof(required[0]);
         check++) {
        if (lstat(required[check], &st)) {
            halt("FAILED: required Data seed path missing");
            return 1;
        }
    }
    if (!lstat(DATA_MNT "/var", &st) || errno != ENOENT) {
        halt("FAILED: unexpected nested var directory");
        return 1;
    }
    fprintf(
        con,
        "A6NATIVEFORMAT original Data seed copied at correct root; %u records verified (type/uid/gid/mode), required paths present, no nested var\n",
        seed_records);
    fflush(con);
    int ok = !r && !stat(KEYBAG, &st) && st.st_size > 0;
    fprintf(con, "it_keybag: MKBKeyBagCreateSystem -> %d, %s %lld bytes\n", r,
            KEYBAG, ok ? (long long)st.st_size : -1LL);
    fflush(con);
    report_keybag("before-unmount");
    if (it_prepare_mobilegestalt(con)) {
        halt("FAILED: native MobileGestalt preparation");
        return 1;
    }
    sync();
    if (unmount(DATA_MNT, 0)) {
        halt("FAILED: Data unmount");
        return 1;
    }
    if (unmount("/mnt1", 0)) {
        halt("FAILED: System unmount");
        return 1;
    }
    fprintf(con, "A6NATIVEFORMAT both volumes unmounted cleanly\n");
    fflush(con);
    pid = vfork();
    if (!pid) {
        execl("/sbin/mount_hfs", "mount_hfs", DATA_DEV, DATA_MNT, (char *)0);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || status) {
        halt("FAILED: diagnostic remount");
        return 1;
    }
    report_keybag("after-remount");
    if (it_verify_mobilegestalt(con)) {
        halt("FAILED: native MobileGestalt remount verification");
        return 1;
    }
#ifdef A6_PREPARED_DATA_SEED
    if (verify_seed(A6_PREPARED_DATA_SEED, DATA_MNT)) {
        halt("FAILED: prepared Data seed remount verification");
        return 1;
    }
    fprintf(
        con,
        "A6NATIVEFORMAT FirmwareKit Data seed verified after remount (bytes/type/uid/gid/mode)\n");
    fflush(con);
#endif
    sync();
    if (unmount(DATA_MNT, 0)) {
        halt("FAILED: diagnostic unmount");
        return 1;
    }

    halt(ok ? "effaceable formatted, system keybag created"
            : "FAILED: no system keybag");
    return !ok;
}

#ifdef IT_PREPARE_DATA_STANDALONE
int main(void)
{
    return it_prepare_data();
}
#endif
