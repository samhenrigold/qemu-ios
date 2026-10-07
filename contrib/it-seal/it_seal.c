/*
 * it_seal -- one clean shutdown for a freshly built NAND store, then gone.
 *
 * A store fresh from ipad1_nand.py has no YAFTL context ("CXT is not valid .
 * Performing full NAND R/O restore", ~13 s on every boot). The FTL writes one
 * when the kernel halts cleanly, but the stock power-off (SpringBoard's slider)
 * never completes on ipad1 because it waits on Bluetooth. So `ipad1_rootfs.py
 * bake --seal` installs this job for the one sealing boot (imgtools/ipad1_seal.py):
 * it lets the boot settle, deletes itself, and halts through reboot(2), which
 * syncs, unmounts and closes the FTL. docs/research/userland-boot.md.
 * 7.x keeps its root read-only, so the job cannot delete itself there; a marker
 * on the data volume makes the seal happen once all the same.
 */

extern unsigned int sleep(unsigned int);
extern int unlink(const char *);
extern int open(const char *, int, ...);
extern void sync(void);
extern int reboot(int);
extern long write(int, const void *, unsigned long);

#define RB_HALT 0x08
#define O_WRONLY 0x0001
#define O_CREAT 0x0200
#define O_EXCL 0x0800

int main(void)
{
    static const char msg[] = "it_seal: halting to seal the NAND store\n";

    sleep(40);                      /* launchd's jobs up, SpringBoard settled */
    if (unlink("/System/Library/LaunchDaemons/com.qemu.it-seal.plist") != 0 &&
        open("/private/var/.it_sealed", O_WRONLY | O_CREAT | O_EXCL, 0644) < 0)
        return 0;                   /* read-only root, already sealed */
    unlink("/usr/local/bin/it_seal");
    sync();
    write(2, msg, sizeof(msg) - 1);
    reboot(RB_HALT);
    return 1;
}
