/* Native shutdown through launchd, the same reboot2(RB_HALT, NULL) path
 * SpringBoard uses. Calling the raw reboot syscall bypasses launchd's service
 * shutdown and leaves 7E18 in _halt_all_cpus without the PMU standby command.
 * No guest UI or injected gestures are involved. Acceptance of this request
 * is not completion: the host must wait for the guest's PMU power-off signal.
 */

#define RB_HALT 0x08

extern int reboot2(int howto, const char *message);
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

    /* Keep the existing marker understood by the frontend. It is a progress
     * message, not evidence that shutdown has completed. */
    write(1, "ithalt: syncing and halting\n", sizeof("ithalt: syncing and halting\n") - 1);
    if (reboot2(RB_HALT, (void *)0) != 0) {
        write(2, "ithalt: launchd shutdown refused\n", sizeof("ithalt: launchd shutdown refused\n") - 1);
        _exit(1);
    }
    _exit(0);
    return 0;
}
