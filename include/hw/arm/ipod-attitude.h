#ifndef HW_ARM_IPOD_ATTITUDE_H
#define HW_ARM_IPOD_ATTITUDE_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host angles are degrees: right edge down is positive roll; top edge away
 * is positive pitch. The LIS302DL mounting reverses device X exactly once.
 * The vector is in 1/64 g; the sensor model converts it to its own counts. */
/* The same vector in g, unrounded (a device in motion passes between poses). */
static inline bool ipod_attitude_gravity(double pitch, double roll, bool flat, double out[3])
{
    if (!isfinite(pitch) || !isfinite(roll) ||
        pitch < -180 || pitch > 180 || roll < -180 || roll > 180) {
        return false;
    }
    double p = pitch * (3.14159265358979323846 / 180.0);
    double r = roll * (3.14159265358979323846 / 180.0);
    out[0] = -sin(r) * cos(p);
    out[1] = flat ? sin(p) : -cos(r) * cos(p);
    out[2] = flat ? -cos(r) * cos(p) : -sin(p);
    return true;
}

/*
 * A sensor's mount from its DT node's "orientation", as the bytes the guest's driver reads (hex): the driver turns
 * the part's raw axes into the OS's with a matrix D, and the part reads so that D gives what the OS expects.
 *   4 bytes, AppleLIS302DL (the same code in 1.1, 1.1.4, 2.x, 3.1.1 and 4.2.1): bit 3 swaps x and y, then bits 0,
 *     1 and 2 negate x, y and z (1.1 0xc036ca78, N72 2.x 0xc03d3b64, N18 3.1.1 0xc0322b16, N18 4.2.1 0x8060d19e).
 *   12 bytes, AppleLIS331DLH and the L3G4200D's driver (4.x to 7.x): D's rows, three int8 each in a 4-byte slot.
 * A gyro reads D's inverse (its transpose). An accelerometer reads the device's gravity g as D^T C g, so the OS
 * gets C g: C is diag(1, 1, -1) where D is a mirror (det -1: every DT seen from 3.0 on) and its negative where D
 * turns (det +1: the M68's absent 0 and the N45's 3 on 1.x, the iPod touch 2G's 5 on 2.x), as the 1.x OS reads its
 * accelerometer the other way round. Every mount found by hand on hardware-checked rotation (and the iPhone 4's z by
 * its compass) is this rule, except the iPod touch 4G's z: checked with a CoreMotion probe on 6.1.6 (face up, and
 * the gyro's fusion), it reads z, so its board sets the LIS model's flip-z.
 * Fills axis[i] = +/-(j + 1): sensor axis i reads device axis j. False if hex is neither form.
 */
static inline bool ios_dt_mount(const char *hex, bool accelerometer, int8_t axis[3])
{
    int d[3][3] = { { 0 } }, n = hex ? (int)strlen(hex) / 2 : 0, det;
    uint8_t b[12];

    if (n != 4 && n != 12) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        b[i] = v;
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (n == 12) {
                d[i][j] = (int8_t)b[4 * i + j];
            } else {
                int src = (b[0] & 8) && i < 2 ? 1 - i : i;      /* the swap, then the row's sign */
                d[i][j] = j == src ? (b[0] >> i & 1 ? -1 : 1) : 0;
            }
        }
    }
    det = d[0][0] * (d[1][1] * d[2][2] - d[1][2] * d[2][1]) - d[0][1] * (d[1][0] * d[2][2] - d[1][2] * d[2][0]) +
          d[0][2] * (d[1][0] * d[2][1] - d[1][1] * d[2][0]);
    for (int i = 0; i < 3; i++) {
        axis[i] = 0;
        for (int j = 0; j < 3; j++) {
            int c = !accelerometer ? 1 : (j == 2 ? det : -det);   /* C's (or 1's) j-th entry */
            if (d[j][i] == 1 || d[j][i] == -1) {
                axis[i] = d[j][i] * c * (j + 1);
            }
        }
        if (!axis[i] || abs(det) != 1) {
            return false;
        }
    }
    return true;
}

static inline bool ipod_attitude_vector(double pitch, double roll, bool flat,
                                        int8_t out[3])
{
    double g[3];
    if (!ipod_attitude_gravity(pitch, roll, flat, g)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        out[i] = (int8_t)lround(64 * g[i]);
    }
    return true;
}
#endif
