/*
 * The vibration motor (include/hw/misc/ios_vibrator.h). One per machine; the
 * state is global so the app-facing API needs no QOM lookup from its thread.
 * It runs while any of its drives is high (the iPhone 4 has two: iOS 4's SoC
 * PWM channel and iOS 6's motor-driver enable).
 * IOS_VIBRATOR_TRACE=1 prints each change with the guest's clock.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/misc/ios_vibrator.h"

static int vibrator_on;
static uint64_t vibrator_pulses;
static unsigned vibrator_drives;    /* bit n: drive n is high (QEMU thread) */
static int vibrator_ndrives;

bool ios_vibrator_get(uint64_t *pulses)
{
    /* pulses first: an edge seen as a count is never newer than the state read after it. */
    if (pulses) {
        *pulses = qatomic_read(&vibrator_pulses);
    }
    return qatomic_read(&vibrator_on);
}

static void vibrator_set(void *opaque, int n, int level)
{
    bool on;

    vibrator_drives = level ? vibrator_drives | 1u << n : vibrator_drives & ~(1u << n);
    on = vibrator_drives != 0;
    if (on == qatomic_read(&vibrator_on)) {
        return;
    }
    if (on) {
        qatomic_inc(&vibrator_pulses);
    }
    qatomic_set(&vibrator_on, on);
    if (getenv("IOS_VIBRATOR_TRACE")) {
        fprintf(stderr, "%.3f ios-vibrator: %s (drive %d)\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6, on ? "on" : "off", n);
    }
}

static bool vibrator_get_prop(Object *obj, Error **errp)
{
    return ios_vibrator_get(NULL);
}

qemu_irq ios_vibrator_line(Object *machine)
{
    assert(vibrator_ndrives < 8);
    if (!vibrator_ndrives) {
        object_property_add_bool(machine, "vibrator", vibrator_get_prop, NULL);
        object_property_set_description(machine, "vibrator", "Whether the vibration motor runs");
    }
    return qemu_allocate_irq(vibrator_set, NULL, vibrator_ndrives++);
}
