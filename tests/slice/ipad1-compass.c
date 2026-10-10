/* The iPhone 4's AK8975B as its driver (8C148 AppleAKM8975B) reads it: WIA, the fuse ROM sensitivities, then per
 * reading CNTL = 1 (single measurement), ST1 DRDY, and HXL..HZH, signed little endian at 0.3 uT/LSB, DRDY cleared
 * by the read and the part back in power-down. It reads the field the AK8973 model reads for the same heading
 * (unmounted). Compiles both models from hw/arm/s5l8930_i2c.c.
 *
 * SLICE hw/arm/s5l8930_i2c.c range #define AK_ST | void s5l8930_ak8973_set_accel(
 * SLICE hw/arm/s5l8930_i2c.c fn ak8973_event ak8973_recv ak8973_send ak8973_reset
 * SLICE hw/arm/s5l8930_i2c.c range #define AK75_WIA | static void ak8975_class_init(
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct { int address; } I2CSlave;
typedef void DeviceState;
typedef struct { int pitch_mdeg, roll_mdeg; bool flat_pose; } LIS302DLState;
enum i2c_event { I2C_START_SEND, I2C_START_RECV, I2C_FINISH };
static bool ipod_attitude_vector(double p, double r, bool flat, int8_t g[3]) { return false; }
typedef struct S5L8930AK8973State S5L8930AK8973State;
#define S5L8930_AK8973(o) ((S5L8930AK8973State *)(o))
#include "slice.h"

static void addr(S5L8930AK8973State *s, uint8_t reg) { ak8973_event(&s->i2c, I2C_START_SEND); ak8975_send(&s->i2c, reg); }

int main(void)
{
    static S5L8930AK8973State s, t;
    int axis, heading;

    for (heading = 0; heading < 360; heading += 70) {
        ak8973_reset(&t);                               /* orientation 0: the field as the driver hands it up */
        t.heading = heading;
        ak8973_event(&t.i2c, I2C_START_SEND); ak8973_send(&t.i2c, 0xe0); ak8973_send(&t.i2c, 0);

        ak8975_reset(&s);
        s.heading = heading;
        addr(&s, 0x00);
        assert(ak8975_recv(&s.i2c) == 0x48);            /* WIA */
        addr(&s, 0x10);
        for (axis = 0; axis < 3; axis++) {
            assert(ak8975_recv(&s.i2c) == 128);         /* ASA: nominal sensitivity */
        }
        assert(s.regs[0x02] == 0);
        addr(&s, 0x0a); ak8975_send(&s.i2c, 1);         /* single measurement */
        assert(s.regs[0x02] == 1 && s.regs[0x0a] == 0); /* DRDY, back to power-down */
        addr(&s, 0x03);
        for (axis = 0; axis < 3; axis++) {
            int16_t raw = ak8975_recv(&s.i2c);
            raw |= ak8975_recv(&s.i2c) << 8;
            assert(fabs(raw * 0.3 - (t.regs[0xc2 + axis] - 128)) < 1.0);
        }
        assert(s.regs[0x02] == 0);                      /* DRDY cleared by the read */
    }
    return 0;
}
