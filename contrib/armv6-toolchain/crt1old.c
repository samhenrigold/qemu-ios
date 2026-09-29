/*
 * The start routine a LEGACY_LINK executable gets (link6 -execute): what 1.x's crt1 did, so one binary
 * runs under 1.x, 2.x and 3.x+ libSystem alike.
 *
 * 1.x (10.4-era) libSystem does not initialize itself: its crt1 set NXArgc/NXArgv/environ/__progname
 * in the executable (libSystem finds `environ` there by name: _NSGetEnviron, execl) and then called
 * *mach_init_routine and *_cthread_init_routine. Entered at _main without that, the process has no
 * main-thread pthread (3A101a: snprintf dereferences NULL, SIGBUS in __PAGEZERO). 2.x+ libSystem runs
 * its own initializer from dyld and its crt1 only calls main; the hooks are called only where non-NULL,
 * as 1.x's crt1 did (it_boot runs this way on 3A101a, 5F138 and 7E18). dlsym, not an import: a
 * libSystem without the symbols must still bind the executable.
 */
#include <dlfcn.h>
#include <stdlib.h>

int NXArgc;
char **NXArgv;
char **environ;
const char *__progname;

int main(int, char **, char **);

static void run_hook(const char *name)
{
    void (**hook)(void) = dlsym(RTLD_DEFAULT, name);
    if (hook && *hook) {
        (*hook)();
    }
}

__attribute__((used, noreturn)) static void crt1old(int argc, char **argv, char **envp)
{
    NXArgc = argc;
    NXArgv = argv;
    environ = envp;
    const char *p = argv[0] ? argv[0] : "";
    __progname = p;
    for (; *p; p++) {
        if (*p == '/') {
            __progname = p + 1;
        }
    }
    run_hook("mach_init_routine");
    run_hook("_cthread_init_routine");
    exit(main(argc, argv, envp));
}

/* The kernel leaves argc, argv[], 0, envp[] on the stack (1.x crt1's start, verbatim). */
__attribute__((naked)) void start(void)
{
    __asm__ volatile("ldr r0, [sp]\n\t"
                     "add r1, sp, #4\n\t"
                     "add r2, r0, #1\n\t"
                     "add r2, r1, r2, lsl #2\n\t"
                     "bic sp, sp, #7\n\t"
                     "b _crt1old");
}
