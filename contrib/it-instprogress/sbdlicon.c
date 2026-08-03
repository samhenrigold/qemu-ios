/* sbdlicon: put an App Store "downloading" placeholder icon on the home
 * screen, or take it away again, from an ordinary process.
 *
 *     sbdlicon add    <unique-id> <bundle-id>
 *     sbdlicon cancel <unique-id>
 *
 * SpringBoardServices exports SBAddDownloadingIconForDisplayIdentifier and
 * SBCancelDownloadingIconForDisplayIdentifier -- MIG stubs onto SpringBoard's
 * own server port, which SBSSpringBoardServerPort() hands to any process that
 * asks. No injection and no entitlement.
 *
 * SpringBoard's handler (sub_42e6c in the 3.1.3 binary) does:
 *     +[SBDownloadingIcon displayIdentifierForDownloadUniqueID:uniqueID]
 *     -[SBIconModel addDownloadingIconForDisplayIdentifier:]
 *     -[SBDownloadingIcon setBundleID:bundleID]
 *     -> -[SBIconModel addNewIconToDesignatedLocation:...]  if bundleID is
 *        already installed, otherwise -[SBIconController setIconToInstall:]
 * so uniqueID names the download and bundleID names the app it becomes.
 *
 * No headers on purpose: the 3.1.3 SDK's are not usable with -nostdinc, and
 * the rest of the guest-side tooling here is written the same way.
 */

/* There is no crt1: ../armv6-toolchain rewrites LC_MAIN as the LC_UNIXTHREAD
 * that 2010 dyld understands, and the pc in it points straight at the entry
 * symbol. So nothing sets up argc/argv -- main() would read whatever happened
 * to be in r0/r1 and dereference it, which is a SIGBUS at the first argv[]
 * access, before any output. Take them off the stack ourselves, where the
 * kernel left them: [sp] is argc and argv starts one word up. Link with
 * -e __start so the rewritten pc lands here. */
__attribute__((naked)) void _start(void)
{
    __asm__ volatile("ldr r0, [sp]\n\t"
                     "add r1, sp, #4\n\t"
                     "b   _main");
}
extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(2, s, slen(s)); }

#define SBS "/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices"

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        w("usage: sbdlicon add <unique-id> <bundle-id>\n"
          "       sbdlicon cancel <unique-id>\n");
        _exit(2);
    }

    void *sbs = dlopen(SBS, RTLD_NOW);
    if (!sbs) { w("sbdlicon: cannot dlopen SpringBoardServices\n"); _exit(1); }

    unsigned (*server_port)(void) = (unsigned (*)(void))dlsym(sbs, "SBSSpringBoardServerPort");
    int (*add)(unsigned, const char *, const char *) =
        (int (*)(unsigned, const char *, const char *))dlsym(sbs, "SBAddDownloadingIconForDisplayIdentifier");
    int (*cancel)(unsigned, const char *) =
        (int (*)(unsigned, const char *))dlsym(sbs, "SBCancelDownloadingIconForDisplayIdentifier");
    if (!server_port || !add || !cancel) {
        w("sbdlicon: SpringBoardServices is missing the download-icon stubs\n");
        _exit(1);
    }

    unsigned sb = server_port();
    if (!sb) { w("sbdlicon: SpringBoard is not answering\n"); _exit(1); }

    int rc;
    if (eq(argv[1], "add")) {
        if (argc < 4) { w("sbdlicon: add needs a bundle id\n"); _exit(2); }
        rc = add(sb, argv[2], argv[3]);
    } else if (eq(argv[1], "cancel")) {
        rc = cancel(sb, argv[2]);
    } else {
        w("sbdlicon: unknown command\n");
        _exit(2);
    }

    /* SpringBoard's handler returns 0 when it took the icon and 5 when it
     * declined -- a nil display identifier, or no icon model yet. */
    w(rc == 0 ? "sbdlicon: ok\n" : "sbdlicon: SpringBoard declined\n");
    _exit(rc == 0 ? 0 : 1);
}
