/*
 * it_heading: print CoreLocation's magnetic heading to stderr, once per
 * update, for RUN_SECONDS, after DELAY_SECONDS (to let locationd come up
 * when launchd starts it at boot). A test probe for the AK8973 model
 * (hw/arm/s5l8930_i2c.c): install it with com.qemu.it-heading.plist, and the
 * headings show up on the serial log while the host qom-sets compass-heading
 * (tests/ipad1/gl-drive.py heading:DEG). Built by contrib/ipad1-guest/build.sh.
 *
 * Plain C against the Objective-C runtime, everything looked up with dlsym:
 * the 3.2 SDK's framework stubs don't link under modern ld64 (see
 * contrib/it-msmquiet). magneticHeading needs no location fix.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef void *id;
typedef void *SEL;
typedef void *Class;
typedef id (*IMP)(id, SEL, ...);

static id (*msg)(id, SEL, ...);
/* stderr, which the launchd job points at /dev/console (the serial log). */
static void say(const char *fmt, ...)
{
    char buf[160];
    __builtin_va_list ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    write(2, buf, n > 0 && n < (int)sizeof(buf) ? n : 0);
}
static SEL (*sel)(const char *);
static Class (*cls)(const char *);

static void on_heading(id self, SEL _cmd, id manager, id heading)
{
    double (*get)(id, SEL) = (double (*)(id, SEL))msg;

    say("it_heading: magnetic %.1f accuracy %.1f\n",
           get(heading, sel("magneticHeading")),
           get(heading, sel("headingAccuracy")));
}

static void on_error(id self, SEL _cmd, id manager, id error)
{
    long (*code)(id, SEL) = (long (*)(id, SEL))msg;

    say("it_heading: error %ld\n", code(error, sel("code")));
}

#define DELAY_SECONDS   40
#define RUN_SECONDS     150

/*
 * mkold.py points LC_UNIXTHREAD straight at _main: no crt1, so no argc/argv
 * and main must not return (see contrib/armv6-toolchain/mkold.py).
 */
int main(void)
{
    double seconds = RUN_SECONDS;

    say("it_heading: started\n");
    sleep(DELAY_SECONDS);
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);

    dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
    if (!objc || !cf ||
        !dlopen("/System/Library/Frameworks/CoreLocation.framework/CoreLocation", RTLD_NOW)) {
        say("it_heading: dlopen failed: %s\n", dlerror());
        _exit(1);
    }
    msg = dlsym(objc, "objc_msgSend");
    sel = dlsym(objc, "sel_registerName");
    cls = dlsym(objc, "objc_getClass");
    Class (*alloc_pair)(Class, const char *, size_t) = dlsym(objc, "objc_allocateClassPair");
    void (*reg_pair)(Class) = dlsym(objc, "objc_registerClassPair");
    int (*add_method)(Class, SEL, IMP, const char *) = dlsym(objc, "class_addMethod");
    void (*run_for)(void *, double, int) = dlsym(cf, "CFRunLoopRunInMode");
    void **default_mode = dlsym(cf, "kCFRunLoopDefaultMode");

    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    Class delegate = alloc_pair(cls("NSObject"), "ITHeadingDelegate", 0);
    add_method(delegate, sel("locationManager:didUpdateHeading:"), (IMP)on_heading, "v@:@@");
    add_method(delegate, sel("locationManager:didFailWithError:"), (IMP)on_error, "v@:@@");
    reg_pair(delegate);

    id mgr = msg(msg(cls("CLLocationManager"), sel("alloc")), sel("init"));
    char (*avail)(id, SEL) = (char (*)(id, SEL))msg;
    /* an instance property on 3.2; the class method only came in 4.0 */
    say("it_heading: headingAvailable %d\n", avail(mgr, sel("headingAvailable")));
    msg(mgr, sel("setDelegate:"), msg(msg(delegate, sel("alloc")), sel("init")));
    ((void (*)(id, SEL, double))msg)(mgr, sel("setHeadingFilter:"), -1.0);  /* kCLHeadingFilterNone */
    msg(mgr, sel("startUpdatingHeading"));

    run_for(*default_mode, seconds, 0);
    msg(mgr, sel("stopUpdatingHeading"));
    msg(pool, sel("drain"));
    say("it_heading: done\n");
    _exit(0);
}
