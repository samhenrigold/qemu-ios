/* Same-boot native cache preparation. System stays mounted read-only; a
 * temporary worker executes with its matching System frameworks and Data root.
 * The worker and plan are removed before native remount verification. */
#define IT_MG_PARENT 1
#include "mobilegestalt.c"
#include <sys/wait.h>
extern int unmount(const char *, int);

static int copy_private(const char *src, const char *dst, unsigned mode)
{
    int in = open(src, O_RDONLY), out = -1, failed = in < 0;
    if (!failed) {
        out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
        failed = out < 0;
    }
    unsigned char buffer[16384];
    while (!failed) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (!n)
            break;
        if (n < 0) {
            failed = 1;
            break;
        }
        ssize_t done = 0;
        while (done < n) {
            ssize_t r = write(out, buffer + done, n - done);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0) {
                failed = 1;
                break;
            }
            done += r;
        }
    }
    if (in >= 0)
        close(in);
    if (out >= 0) {
        if (fsync(out))
            failed = 1;
        if (close(out))
            failed = 1;
    }
    return failed ? -1 : 0;
}
static int mount_data(const char *target)
{
    int status;
    pid_t child = vfork();
    if (!child) {
        execl("/sbin/mount_hfs", "mount_hfs", "/dev/disk0s2", target,
              (char *)0);
        _exit(127);
    }
    return child < 0 || waitpid(child, &status, 0) != child || status ? -1 : 0;
}
int it_prepare_mobilegestalt(FILE *log)
{
    struct stat st;
    if (stat(PLAN, &st))
        return errno == ENOENT ? 0 : -1;
    if (load_cf())
        return -1;
    plan = read_plist(PLAN);
    if (!plan || CFGetTypeID(plan) != CFDictionaryGetTypeID())
        return -1;
    const char *temp = "/mnt2/tmp/it-mobilegestalt";
    if (mkdir(temp, 0700) ||
        copy_private("/usr/local/bin/it_mobilegestalt",
                     "/mnt2/tmp/it-mobilegestalt/worker", 0755) ||
        copy_private(PLAN, "/mnt2/tmp/it-mobilegestalt/plan.plist", 0600)) {
        fprintf(log, "A6NATIVEGESTALT worker staging failed errno=%d\n", errno);
        return -1;
    }
    sync();
    if (unmount("/mnt2", 0) || mount_data("/mnt1/private/var")) {
        fprintf(
            log,
            "A6NATIVEGESTALT native Data mount transition failed errno=%d\n",
            errno);
        return -1;
    }
    fprintf(
        log,
        "A6NATIVEGESTALT starting worker against read-only System and native Data\n");
    fflush(log);
    pid_t child = fork();
    int status = -1;
    if (!child) {
        if (chroot("/mnt1") || chdir("/")) {
            perror("A6NATIVEGESTALT chroot");
            _exit(126);
        }
        char *const argv[] = {"it_mobilegestalt", NULL};
        char *const environment[] = {"DYLD_SHARED_REGION=private",
                                     "HOME=/var/root",
                                     "TMPDIR=/private/var/tmp", NULL};
        execve("/private/var/tmp/it-mobilegestalt/worker", argv, environment);
        perror("A6NATIVEGESTALT worker exec");
        _exit(127);
    }
    int failed = child < 0 || waitpid(child, &status, 0) != child || status;
    fprintf(log, "A6NATIVEGESTALT full-system worker status=%d\n", status);
    fflush(log);
    if (unlink("/mnt1/private/var/tmp/it-mobilegestalt/worker") ||
        unlink("/mnt1/private/var/tmp/it-mobilegestalt/plan.plist") ||
        rmdir("/mnt1/private/var/tmp/it-mobilegestalt"))
        failed = 1;
    sync();
    if (unmount("/mnt1/private/var", 0) || mount_data("/mnt2"))
        return -1;
    CFDictionaryRef cache = read_plist(CACHE);
    if (!valid_cache(cache, 1))
        failed = 1;
    if (!failed) {
        original_data =
            CFRetain(CFDictionaryGetValue(cache, CFSTR("CacheData")));
        original_uuid =
            CFRetain(CFDictionaryGetValue(cache, CFSTR("CacheUUID")));
    }
    if (cache)
        CFRelease(cache);
    return failed ? -1 : it_verify_mobilegestalt(log);
}
