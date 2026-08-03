/* sbunlock: dismiss the lock screen without touching the digitizer.
 *
 * SpringBoardServices exports SBApplicationRequestedDeviceUnlock -- the call an
 * app makes when it needs the device unlocked to continue. On a device with no
 * passcode SpringBoard just slides the lock screen away.
 *
 * This exists because headless verification needs a way past the lock screen
 * that does not depend on synthesised touch. Slide-to-unlock over QMP is the
 * step that fails when the digitizer has stopped reporting (see the touch
 * notes), and then every later assertion is really a touch test in disguise.
 *
 * See sbdlicon.c for why there is an explicit _start.
 */
extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(2, s, slen(s)); }

#define SBS "/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices"

__attribute__((naked)) void _start(void) { __asm__ volatile("b _main"); }

int main(void)
{
    void *sbs = dlopen(SBS, RTLD_NOW);
    if (!sbs) { w("sbunlock: cannot dlopen SpringBoardServices\n"); _exit(1); }

    unsigned (*server_port)(void) = (unsigned (*)(void))dlsym(sbs, "SBSSpringBoardServerPort");
    int (*req)(unsigned) = (int (*)(unsigned))dlsym(sbs, "SBApplicationRequestedDeviceUnlock");
    if (!server_port || !req) { w("sbunlock: missing stubs\n"); _exit(1); }

    unsigned sb = server_port();
    if (!sb) { w("sbunlock: SpringBoard is not answering\n"); _exit(1); }

    int rc = req(sb);
    w(rc == 0 ? "sbunlock: asked\n" : "sbunlock: SpringBoard declined\n");
    _exit(rc == 0 ? 0 : 1);
}
