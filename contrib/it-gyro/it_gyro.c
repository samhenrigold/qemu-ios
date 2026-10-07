/*
 * it_gyro: start CoreMotion's gyro (pull mode) and print a few samples to
 * stderr: a test probe for the L3G4200D model (hw/arm/s5l8930_i2c.c) on
 * N81/N90. Run it through the guest agent's exec. Built like it_heading
 * (contrib/ipad1-guest/build.sh): plain C against the Objective-C runtime,
 * everything looked up with dlsym.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

typedef void *id;
typedef void *SEL;
typedef void *Class;
typedef struct { double x, y, z; } Rate;

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

/* mkold.py enters _main with no crt1: main must not return. */
int main(void)
{
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);

    dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
    if (!objc || !dlopen("/System/Library/Frameworks/CoreMotion.framework/CoreMotion", RTLD_NOW)) {
        say("it_gyro: dlopen failed: %s\n", dlerror());
        _exit(1);
    }
    id (*msg)(id, SEL, ...) = dlsym(objc, "objc_msgSend");
    void (*msg_stret)(void *, id, SEL, ...) = dlsym(objc, "objc_msgSend_stret");
    SEL (*sel)(const char *) = dlsym(objc, "sel_registerName");
    Class (*cls)(const char *) = dlsym(objc, "objc_getClass");
    char (*get_bool)(id, SEL) = (char (*)(id, SEL))msg;
    double (*get_double)(id, SEL) = (double (*)(id, SEL))msg;

    id pool = msg(msg(cls("NSAutoreleasePool"), sel("alloc")), sel("init"));
    id mgr = msg(msg(cls("CMMotionManager"), sel("alloc")), sel("init"));
    say("it_gyro: gyroAvailable %d\n", get_bool(mgr, sel("isGyroAvailable")));
    ((void (*)(id, SEL, double))msg)(mgr, sel("setGyroUpdateInterval:"), 0.01);
    msg(mgr, sel("startGyroUpdates"));
    for (int i = 0; i < 6; i++) {
        usleep(500000);
        id data = msg(mgr, sel("gyroData"));
        Rate r = { 0, 0, 0 };

        if (data) {
            msg_stret(&r, data, sel("rotationRate"));
        }
        say("it_gyro: active %d sample %s t=%.3f rate %.4f %.4f %.4f\n", get_bool(mgr, sel("isGyroActive")),
            data ? "yes" : "no", data ? get_double(data, sel("timestamp")) : 0.0, r.x, r.y, r.z);
    }
    msg(mgr, sel("stopGyroUpdates"));
    msg(pool, sel("drain"));
    _exit(0);
}
