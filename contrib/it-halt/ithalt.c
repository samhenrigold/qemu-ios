/* ithalt: shut the guest's filesystem down from the kernel, with no UI.
 *
 *     ithalt          # sync, unmount everything, halt
 *
 * WHY THIS EXISTS
 * ---------------
 * The only clean shutdown this machine had was a synthetic press-and-hold plus
 * a slide across the power-off sheet -- i.e. it went through SpringBoard. That
 * works when the device is healthy and is useless exactly when it isn't: a game
 * that has hung, a SpringBoard that has crashed, anything covering the screen.
 * The host asks for a shutdown precisely in those moments, and a gesture aimed
 * at a slider that is not on screen does nothing at all. (Measured: a powerdown
 * requested while a full-screen GL app was foreground never completed, while
 * the same build powers off in 14s from the home screen.)
 *
 * reboot(2) needs none of that. It is a syscall: XNU syncs, calls
 * vfs_unmountall(), and halts. No process has to cooperate, nothing has to be
 * drawn, and the HFS+ catalog -- which is the whole point, since 3.1.3 keeps it
 * in memory and only an unmount commits it -- lands on flash either way.
 *
 * /sbin/halt and /sbin/reboot do not exist on these images (checked on
 * nand-ultimate), which is why this is 20 lines of C rather than a shell
 * command. It is streamed to the guest over ssh and exec'd from /tmp, exactly
 * like contrib/it-orientation/itorient.
 *
 * RB_HALT rather than a reboot: the host is about to tear the machine down, and
 * a guest that comes back up would start dirtying the volume again.
 */

#define RB_HALT 0x08

extern void sync(void);
extern int reboot(int howto);
extern long write(int, const void *, unsigned long);
extern void _exit(int);

/* No crt1: the kernel enters at the entry point with argc/argv on the stack
 * where it left them. Link with -e __start. Same shape as itorient's. */
__attribute__((naked)) void _start(void)
{
    __asm__ volatile("ldr r0, [sp]\n\t"
                     "add r1, sp, #4\n\t"
                     "b   _main");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Say so before the volume goes away, so the host sees this ran even if the
     * ssh session dies with the unmount (it will). */
    write(1, "ithalt: syncing and halting\n", 28);
    sync();

    reboot(RB_HALT);

    /* Only reached if the syscall was refused. */
    write(2, "ithalt: reboot(RB_HALT) refused\n", 32);
    _exit(1);
    return 1;
}
