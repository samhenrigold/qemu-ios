/* itorient: report SpringBoard's front-most-app UI orientation, forever, one
 * integer per line on stdout.
 *
 *     itorient            # 0 | 90 | 180 | -90, printed whenever it changes
 *
 * WHY THIS EXISTS
 * ---------------
 * The host wants to swing its on-screen iPod round when the user opens a
 * landscape-only app. iPhone OS 3.1.3 gives the *host* nothing to observe:
 *
 *   - `-[SpringBoard noteUIOrientationChanged:display:]` is the whole story on
 *     the device, and it posts nothing. It updates the `_frontMostAppOrientation`
 *     ivar, pokes IOAccessoryManager (only when a dock accessory is attached),
 *     and calls the imported `_GSEventRotateSimulator` synchronously. No
 *     notify_post, no CFNotificationCenter, nothing that leaves the process.
 *   - `com.apple.springboard.orientation` / `.rawOrientation` /
 *     `.unambiguousOrientation` ARE real Darwin notifications, and
 *     notification_proxy would relay them to the host -- but they are posted
 *     from the accelerometer path (sub_42380 in the 3.1.3 binary), i.e. they
 *     describe how the device is being HELD, which the host already knows
 *     because the host is the one faking the accelerometer. They say nothing
 *     about an app that forced landscape while lying flat. They also carry
 *     their value in the notify *state*, which no lockdown service exposes.
 *   - `springboardservicesrelay`, which backs the com.apple.springboardservices
 *     lockdown service the app already uses for the icon layout, implements
 *     exactly three commands on 3.1.3 -- `getIconState`, `setIconState`,
 *     `getIconPNGData`. libimobiledevice's `sbservices_get_interface_orientation`
 *     would have been the whole feature for free; the 3.1.3 relay has no such
 *     command, checked with `strings` against the binary on the root filesystem.
 *
 * What the guest DOES have is SpringBoardServices' MIG stub
 * `SBGetUIOrientation(mach_port_t, int *)` onto SpringBoard's own server port,
 * which `SBSSpringBoardServerPort()` hands to any process for the asking -- no
 * injection, no entitlement, exactly like contrib/it-instprogress/sbdlicon.
 * That stub returns the `_frontMostAppOrientation` ivar, which is the value
 * `noteUIOrientationChanged:` writes, which is what
 * `-[SBApplication defaultStatusBarOrientation]` computed from the app's
 * Info.plist. It is the authoritative answer, one cheap IPC at a time.
 *
 * (The signature was read off the armv6 stub in the 3.1.3 dyld shared cache
 * rather than guessed: the prologue stores r0 and keeps r1 in r5, and the tail
 * is `ldr r3, [sp, #0x30]; str r3, [r5]` -- two arguments, the second an out
 * pointer. Getting that wrong would scribble through a register full of
 * garbage, so it was worth the disassembly.)
 *
 * THE VALUES are degrees, and they are NOT the host's convention. They are what
 * `-[SBApplication defaultStatusBarOrientation]` maps the Info.plist
 * UIInterfaceOrientation onto:
 *
 *     Portrait           (1) ->    0
 *     PortraitUpsideDown (2) ->  180
 *     LandscapeLeft      (3) ->   90     home button on the RIGHT
 *     LandscapeRight     (4) ->  -90     home button on the LEFT
 *
 * The host tracks degrees the *device* is turned clockwise from portrait, where
 * home-on-the-right is 270. So host = (360 - guest) % 360. The conversion lives
 * on the host side; this program reports what SpringBoard said, unmodified.
 *
 * WHY IT POLLS. There is nothing to wait on -- the only edge is a method call
 * inside SpringBoard. 4 Hz is far below the noise floor of an emulated ARM1176
 * (one Mach round trip per tick) and well under the ~0.4 s the host spends
 * animating the swing, so a change is on screen before the animation of the
 * user's tap has finished.
 *
 * No headers on purpose, and no crt1: see sbdlicon.c, which this is modelled on.
 */

/* ../armv6-toolchain rewrites LC_MAIN as an LC_UNIXTHREAD whose pc points
 * straight at the entry symbol, so nothing sets up argc/argv. Take them off the
 * stack where the kernel left them. Link with -e __start. */
__attribute__((naked)) void _start(void)
{
    __asm__ volatile("ldr r0, [sp]\n\t"
                     "add r1, sp, #4\n\t"
                     "b   _main");
}

extern long write(int, const void *, unsigned long);
extern void _exit(int);
extern int usleep(unsigned);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
#define RTLD_NOW 2

#define SBS "/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices"

/* 4 Hz. See the header for why polling, and why this rate. */
#define POLL_US 250000

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(2, s, slen(s)); }

/* One line on stdout, e.g. "-90\n". Returns 0 once the host has gone away --
 * the ssh session closing is how this process is meant to end, and a write to a
 * dead pipe is the only notice we get. */
static int emit(int v)
{
    char buf[16];
    unsigned n = sizeof(buf);
    unsigned neg = v < 0;
    unsigned m = neg ? (unsigned)(-v) : (unsigned)v;

    buf[--n] = '\n';
    do { buf[--n] = (char)('0' + m % 10); m /= 10; } while (m);
    if (neg) buf[--n] = '-';
    return write(1, buf + n, sizeof(buf) - n) > 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    void *sbs = dlopen(SBS, RTLD_NOW);
    if (!sbs) { w("itorient: cannot dlopen SpringBoardServices\n"); _exit(1); }

    unsigned (*server_port)(void) = (unsigned (*)(void))dlsym(sbs, "SBSSpringBoardServerPort");
    int (*get_ui_orientation)(unsigned, int *) =
        (int (*)(unsigned, int *))dlsym(sbs, "SBGetUIOrientation");
    if (!server_port || !get_ui_orientation) {
        w("itorient: SpringBoardServices is missing SBGetUIOrientation\n");
        _exit(1);
    }

    /* Deliberately out of range so the first successful read always prints:
     * the host seeds its idea of the guest's orientation from that line and
     * only acts on later ones. */
    int last = 0x7fffffff;

    for (;;) {
        /* The port is looked up every tick rather than once. A respring gives
         * SpringBoard a new port, and the whole point of this program is to
         * outlive the app launches it is watching -- caching the port would
         * make it go quietly deaf the first time SpringBoard restarts, which is
         * something the host's own "Restart SpringBoard" menu item does. */
        unsigned port = server_port();
        int o;
        if (port && get_ui_orientation(port, &o) == 0 && o != last) {
            last = o;
            if (!emit(o)) _exit(0);
        }
        usleep(POLL_US);
    }
}
