/* Lockbot on 2.x only forwards ProgramArguments, not launchd's environment dictionary.
 * Set the helper for this installation service alone, then replace the launcher.
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
__attribute__((naked)) void _start(void) {
    __asm__ volatile("ldr r0, [sp]\n\tadd r1, sp, #4\n\tb _main");
}
int main(int argc, char **argv) {
    if (argc < 2) _exit(64);
    if (setenv("DYLD_INSERT_LIBRARIES", "/usr/lib/libappsync.dylib", 1)) _exit(71);
    execv(argv[1], argv + 1);
    perror("AppSync installation service");
    _exit(71);
}
