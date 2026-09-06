#ifndef HW_ARM_IPOD_ATTITUDE_H
#define HW_ARM_IPOD_ATTITUDE_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Host angles are degrees: right edge down is positive roll; top edge away
 * is positive pitch. The LIS302DL mounting reverses device X exactly once.
 * Keep the emulator's established 64 counts/g calibration. */
static inline bool ipod_attitude_vector(double pitch, double roll, bool flat,
                                        int8_t out[3])
{
    if (!isfinite(pitch) || !isfinite(roll) ||
        pitch < -180 || pitch > 180 || roll < -180 || roll > 180) {
        return false;
    }
    double p = pitch * (3.14159265358979323846 / 180.0);
    double r = roll * (3.14159265358979323846 / 180.0);
    out[0] = (int8_t)lround(-64 * sin(r) * cos(p));
    out[1] = (int8_t)lround(64 * (flat ? sin(p) : -cos(r) * cos(p)));
    out[2] = (int8_t)lround(64 * (flat ? -cos(r) * cos(p) : -sin(p)));
    return true;
}
#endif
