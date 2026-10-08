/*
 * it_location: print CoreLocation's location updates for N seconds (argv[1], default 20), one line
 * each, as CLLocation describes itself ("<+37.33490000, -122.00900000> +/- 4.87m (speed 1.40 mps /
 * course 90.00) @ ..."), then "it_location: done". A test probe for the GPS receiver model (the 3GS's,
 * hw/misc/ios_baseband_gps.c): the sessions check puts it at /usr/local/bin and runs it through the
 * guest agent's spawn. No prompt stands in the way: 4.x locationd allows executables under /usr/
 * ("allowing internal executable"), 6.x only whitelisted ones, which com.apple.locationd.preauthorized
 * makes it (it_location.entitlements). It turns Location Services on first if they are off.
 * Built by build.sh (armv6, every guest runs it).
 *
 * Plain C against the Objective-C runtime, everything looked up with dlsym, as contrib/it-heading.
 * didUpdateToLocation:fromLocation: is the 4.x delegate call; 6.x and 7.x still make it for a
 * delegate without didUpdateLocations:.
 */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern void _exit(int);
extern int atoi(const char *);
extern int printf(const char *, ...);
extern int fflush(void *);

__attribute__((naked)) void _start(void) { __asm__ volatile("ldr r0,[sp]\n\tadd r1,sp,#4\n\tb _main"); }

typedef void *id;
typedef void *SEL;
typedef id (*IMP)(id, SEL, ...);

static id (*msg)(id, SEL, ...);
static SEL (*sel)(const char *);

static void on_location(id self, SEL _cmd, id manager, id location, id old)
{
    printf("it_location: %s\n", (const char *)msg(msg(location, sel("description")), sel("UTF8String")));
    fflush(0);
}

static void on_status(id self, SEL _cmd, id manager, int status)
{
    printf("it_location: authorization %d\n", status);
    fflush(0);
}

static void on_error(id self, SEL _cmd, id manager, id error)
{
    printf("it_location: error %ld\n", (long)msg(error, sel("code")));
    fflush(0);
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atoi(argv[1]) : 20;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", 2);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);

    dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", 2);
    if (!objc || !cf || !dlopen("/System/Library/Frameworks/CoreLocation.framework/CoreLocation", 2)) {
        printf("it_location: dlopen failed\n");
        _exit(1);
    }
    msg = dlsym(objc, "objc_msgSend");
    sel = dlsym(objc, "sel_registerName");
    id (*cls)(const char *) = dlsym(objc, "objc_getClass");
    id (*alloc_pair)(id, const char *, unsigned long) = dlsym(objc, "objc_allocateClassPair");
    void (*reg_pair)(id) = dlsym(objc, "objc_registerClassPair");
    int (*add_method)(id, SEL, IMP, const char *) = dlsym(objc, "class_addMethod");
    void (*run_for)(void *, double, int) = dlsym(cf, "CFRunLoopRunInMode");
    void **default_mode = dlsym(cf, "kCFRunLoopDefaultMode");

    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    id delegate = alloc_pair(cls("NSObject"), "ITLocationDelegate", 0);
    add_method(delegate, sel("locationManager:didUpdateToLocation:fromLocation:"), (IMP)on_location, "v@:@@@");
    add_method(delegate, sel("locationManager:didFailWithError:"), (IMP)on_error, "v@:@@");
    add_method(delegate, sel("locationManager:didChangeAuthorizationStatus:"), (IMP)on_status, "v@:@i");
    reg_pair(delegate);

    /* Settings' Location Services switch (a 6.x/7.x Setup the test harness walked turns it off), as
     * Preferences flips it: the entitlement in it_location.entitlements lets the probe do the same. */
    if (!(char)(long)msg(cls("CLLocationManager"), sel("locationServicesEnabled"))) {
        ((void (*)(id, SEL, char))msg)(cls("CLLocationManager"), sel("setLocationServicesEnabled:"), 1);
        printf("it_location: Location Services were off, turned on: %d\n",
               (char)(long)msg(cls("CLLocationManager"), sel("locationServicesEnabled")));
    }
    id mgr = msg(msg(cls("CLLocationManager"), sel("alloc")), sel("init"));
    msg(mgr, sel("setDelegate:"), msg(msg(delegate, sel("alloc")), sel("init")));
    ((void (*)(id, SEL, double))msg)(mgr, sel("setDesiredAccuracy:"), -1.0);   /* kCLLocationAccuracyBest */
    msg(mgr, sel("startUpdatingLocation"));
    printf("it_location: started\n");
    fflush(0);
    run_for(*default_mode, seconds, 0);
    msg(mgr, sel("stopUpdatingLocation"));
    msg(pool, sel("drain"));
    printf("it_location: done\n");
    fflush(0);
    _exit(0);
    return 0;
}
