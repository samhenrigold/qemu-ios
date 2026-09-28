/*
 * it_keybag -- the data-protection step of a restore, for a new 4.x device, then halt.
 *
 * 4.x userland needs a system keybag (/private/var/keybags/systembag.kb), and the keybag
 * needs formatted effaceable storage (NOR). Both are made by the restore ramdisk's
 * restored_update and are refused from a normal boot ("format attempt from untrusted
 * root"): AppleARMPlatform publishes the SecureRoot resource only when the root device
 * matches the DeviceTree's secure-root-prefix ('md', the RAM disk). So imgtools/
 * ipad1_keybag.py boots the IPSW's own restore ramdisk (a private copy) as md0, with this
 * helper as /usr/local/bin/restored_external, which the ramdisk's rc.boot runs first.
 *
 * Same calls as restored_update: AppleEffaceableStorage user client selector 3
 * (isFormatted) and 4 (format), then MKBKeyBagCreateSystem(NULL, <data mount>), both by
 * symbol. Mounts the device's data volume at /mnt2 like restored does, checks the keybag,
 * unmounts and halts through reboot(2). docs/ipad1/ios4.md.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

typedef unsigned int mach_port_t, io_object_t;
typedef int kern_return_t;
extern mach_port_t mach_task_self_;
extern int reboot(int);
extern int unmount(const char *, int);

#define RB_HALT 0x08
#define DATA_DEV "/dev/disk0s2"
#define DATA_MNT "/mnt2"
#define KEYBAG DATA_MNT "/keybags/systembag.kb"

static FILE *con;

static void halt(const char *result)
{
    fprintf(con, "it_keybag: %s; halting\n", result);
    fflush(con);
    sync();
    reboot(RB_HALT);
}

int main(void)
{
    struct stat st;
    int i, status;

    con = fopen("/dev/console", "w");
    if (!con)
        con = stderr;
    void *iok = dlopen("/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", RTLD_NOW);
    void *mkb = dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag", RTLD_NOW);
    void *(*Matching)(const char *) = iok ? dlsym(iok, "IOServiceMatching") : 0;
    io_object_t (*GetService)(mach_port_t, void *) = iok ? dlsym(iok, "IOServiceGetMatchingService") : 0;
    kern_return_t (*Open)(io_object_t, mach_port_t, uint32_t, io_object_t *) = iok ? dlsym(iok, "IOServiceOpen") : 0;
    kern_return_t (*Scalar)(io_object_t, uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t *) =
        iok ? dlsym(iok, "IOConnectCallScalarMethod") : 0;
    int (*Create)(void *, const char *) = mkb ? dlsym(mkb, "MKBKeyBagCreateSystem") : 0;
    if (!Matching || !GetService || !Open || !Scalar || !Create) {
        fprintf(con, "it_keybag: %s\n", dlerror());
        halt("FAILED: IOKit / MobileKeyBag symbols missing");
        return 1;
    }

    /* The FTL and the effaceable driver come up after the RAM-disk root. */
    io_object_t svc = 0, conn = 0;
    for (i = 0; i < 120 && (stat(DATA_DEV, &st) || !(svc = GetService(0, Matching("AppleEffaceableStorage")))); i++)
        sleep(1);
    if (!svc || stat(DATA_DEV, &st)) {
        halt(svc ? "FAILED: no " DATA_DEV : "FAILED: no AppleEffaceableStorage");
        return 1;
    }
    kern_return_t kr = Open(svc, mach_task_self_, 0, &conn);
    uint64_t fmt = 0;
    uint32_t n = 1;
    kern_return_t k3 = kr ? kr : Scalar(conn, 3, NULL, 0, &fmt, &n);
    fprintf(con, "it_keybag: effaceable open %x isFormatted %x -> %llu\n", kr, k3, fmt);
    if (!k3 && !fmt) {
        n = 0;
        fprintf(con, "it_keybag: format %x\n", Scalar(conn, 4, NULL, 0, NULL, &n));
        n = 1;
        k3 = Scalar(conn, 3, NULL, 0, &fmt, &n);
    }
    fflush(con);
    if (k3 || !fmt) {
        halt("FAILED: effaceable storage not formatted");
        return 1;
    }

    pid_t pid = vfork();
    if (pid == 0) {
        execl("/sbin/mount_hfs", "mount_hfs", DATA_DEV, DATA_MNT, (char *)0);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || status) {
        halt("FAILED: mount_hfs " DATA_DEV " " DATA_MNT);
        return 1;
    }
    int r = Create(NULL, DATA_MNT);
    int ok = !r && !stat(KEYBAG, &st) && st.st_size > 0;
    fprintf(con, "it_keybag: MKBKeyBagCreateSystem -> %d, %s %lld bytes\n", r, KEYBAG,
            ok ? (long long)st.st_size : -1LL);
    fflush(con);
    sync();
    unmount(DATA_MNT, 0);
    halt(ok ? "effaceable formatted, system keybag created" : "FAILED: no system keybag");
    return !ok;
}
