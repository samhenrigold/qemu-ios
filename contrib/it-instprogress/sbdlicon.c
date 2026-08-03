/* sbdlicon: put an App Store "downloading" placeholder icon on the home
 * screen, or take it away again, from an ordinary process.
 *
 *     sbdlicon add    <unique-id> [<bundle-id>]
 *     sbdlicon cancel <unique-id>
 *
 * SpringBoardServices exports SBAddDownloadingIconForDisplayIdentifier and
 * SBCancelDownloadingIconForDisplayIdentifier -- MIG stubs onto SpringBoard's
 * own server port, which SBSSpringBoardServerPort() hands to any process that
 * asks. No injection and no entitlement.
 *
 * SpringBoard's handler (sub_42e6c in the 3.1.3 binary) does:
 *     displayID = +[SBDownloadingIcon displayIdentifierForDownloadUniqueID:uid]
 *     icon      = -[SBIconModel addDownloadingIconForDisplayIdentifier:displayID]
 *                 -[SBDownloadingIcon setBundleID:bundleID]
 *     if ([iconModel iconForDisplayIdentifier:bundleID])
 *             -[SBIconModel addNewIconToDesignatedLocation:icon ...]
 *     [SBIconController setIconToInstall:icon]
 *
 * WHY THE BUNDLE ID DEFAULTS TO THE PLACEHOLDER'S OWN DISPLAY IDENTIFIER
 * ---------------------------------------------------------------------
 * That lookup is the whole difficulty. SpringBoard only PLACES the icon when
 * the bundle id it is handed already has an icon -- the App Store's update
 * case, where the placeholder takes over the existing app's slot. Hand it the
 * bundle id of an app that is not installed yet, which is the case that
 * matters when an .ipa is dropped on the window, and the icon is created but
 * never placed: it goes into SBIconController's iconToInstall ivar and nothing
 * appears on the home screen. That was measured both ways.
 *
 * The way round is in the ordering above: the icon is created and registered
 * under "com.apple.downloadingicon-<uid>" BEFORE the lookup runs, so passing
 * that same string as the bundle id makes the lookup find the icon we just
 * made, and it is placed after the last icon on the home screen. That is what
 * `add` does when no bundle id is given, because it is what a caller
 * installing a new app always wants; pass one explicitly only to get the
 * update behaviour, where the placeholder sits in the existing app's slot.
 *
 * The placement is NOT saved to disk (SpringBoard passes saveIconState:NO), so
 * a placeholder that is somehow left behind does not survive a respring.
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

#define DL_PREFIX "com.apple.downloadingicon-"

int main(int argc, char **argv)
{
    if (argc < 3) {
        w("usage: sbdlicon add <unique-id> [<bundle-id>]\n"
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
        const char *bundle;
        static char self_id[512];

        if (argc > 3) {
            bundle = argv[3];
        } else {
            /* "com.apple.downloadingicon-" + uid: the identifier SpringBoard
             * will have just registered the icon under. See the header. */
            unsigned i = 0, j = 0;
            const char *p = DL_PREFIX;
            while (p[i] && i < sizeof(self_id) - 1) { self_id[i] = p[i]; i++; }
            while (argv[2][j] && i < sizeof(self_id) - 1) self_id[i++] = argv[2][j++];
            self_id[i] = 0;
            bundle = self_id;
        }
        rc = add(sb, argv[2], bundle);
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
