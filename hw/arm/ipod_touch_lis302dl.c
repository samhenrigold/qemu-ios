#include "hw/arm/ipod_touch_lis302dl.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/ipod-attitude.h"

/* 1 g in LIS302DL output counts. The part is ±2 g full-scale over a signed
 * 8-bit register (18 mg/digit), so 1 g ~= 0x38. Any value with the right sign
 * and roughly full-scale magnitude is enough for the OS orientation logic. */
#define ACCEL_1G 0x40

static bool lis302dl_debug(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("IPOD_ACCEL_DEBUG") != NULL;
    }
    return cached;
}

bool lis302dl_apply_attitude(LIS302DLState *s, double pitch, double roll, bool flat)
{
    int8_t vector[3];
    if (!ipod_attitude_vector(pitch, roll, flat, vector)) {
        return false;
    }
    s->pitch_mdeg = lround(pitch * 1000);
    s->roll_mdeg = lround(roll * 1000);
    s->flat_pose = flat;
    s->base_x = vector[0];
    s->base_y = vector[1];
    s->base_z = vector[2];
    return true;
}

/* UIDeviceOrientation shares the same mounted attitude model. */
void lis302dl_apply_orientation(LIS302DLState *s, uint32_t o)
{
    double roll = 0;
    bool flat = o == 5 || o == 6;
    if (o == 2 || o == 6) roll = 180;
    else if (o == 3) roll = 90;
    else if (o == 4) roll = -90;
    lis302dl_apply_attitude(s, 0, roll, flat);
    s->orientation = o;
    it_display_set_orientation(o);
    if (lis302dl_debug()) {
        printf("lis302dl: orientation=%u -> x=%d y=%d z=%d\n",
               o, s->base_x, s->base_y, s->base_z);
    }
}

/* Generate only on a guest sample boundary. Repeated reads within one period
 * see the same triplet, regardless of the host input event rate. Lazy sampling
 * avoids an always-running timer on a part whose interrupt pin is not modeled. */
static void lis302dl_sample(LIS302DLState *s, int64_t now)
{
    uint32_t rate = s->rate_hz ? s->rate_hz : (s->ctrl_reg1 & 0x80 ? 400 : 100);
    int64_t period = 1000000000LL / rate;
    if (s->last_sample_ns >= 0 && now >= s->last_sample_ns &&
        now - s->last_sample_ns < period) return;
    s->last_sample_ns = now;
    int values[3] = { s->base_x, s->base_y, s->base_z };
    int64_t elapsed = now - s->shake_start_ns;
    if (s->shake_start_ns >= 0 && elapsed >= 0 && elapsed < 200000000) {
        int impulse = (elapsed / 20000000) & 1 ? -127 : 127;
        values[0] = impulse;
        values[1] = -impulse;
        values[2] = impulse / 2;
    } else {
        s->shake_start_ns = -1;
        if (values[0] || values[1] || values[2]) {
            for (int i = 0; i < 3; i++) {
                /* Device-local PRNG: migration is reproducible and motion does
                 * not consume guest entropy or depend on host wall time. */
                uint32_t x = s->noise_state ? s->noise_state : 0x302d1;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                s->noise_state = x;
                values[i] += (int)(x % 3) - 1;
            }
        }
    }
    s->out_x = CLAMP(values[0], -128, 127);
    s->out_y = CLAMP(values[1], -128, 127);
    s->out_z = CLAMP(values[2], -128, 127);
}

static void lis302dl_trace_poll(LIS302DLState *s, int64_t now)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("IT_ACCEL_TRACE") != NULL;
    if (!enabled) return;
    if (s->trace_last_poll_ns >= 0 && now > s->trace_last_poll_ns) {
        s->trace_poll_sum_ns += now - s->trace_last_poll_ns;
        s->trace_polls++;
    }
    s->trace_last_poll_ns = now;
    if (now - s->trace_last_report_ns >= 1000000000LL && s->trace_polls) {
        fprintf(stderr, "[IT_ACCEL] OUT_X polls=%u mean_interval_ms=%.3f\n",
                s->trace_polls, s->trace_poll_sum_ns / (s->trace_polls * 1000000.0));
        s->trace_last_report_ns = now;
        s->trace_poll_sum_ns = 0;
        s->trace_polls = 0;
    }
}

void lis302dl_shake(LIS302DLState *s)
{
    s->shake_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

void lis302dl_set_axis_value(LIS302DLState *s, char axis, int v)
{
    if (v < -128) v = -128;
    if (v > 127) v = 127;
    switch (axis) {
        case 'x': s->base_x = (int8_t)v; break;
        case 'y': s->base_y = (int8_t)v; break;
        case 'z': s->base_z = (int8_t)v; break;
        default: break;
    }
}

static int lis302dl_event(I2CSlave *i2c, enum i2c_event event)
{
    LIS302DLState *s = LIS302DL(i2c);
    if (event == I2C_START_SEND) {
        /* the first byte of a write is the sub-address / register pointer */
        s->pointer_set = false;
    }
    return 0;
}

static uint8_t lis302dl_recv(I2CSlave *i2c)
{
    LIS302DLState *s = LIS302DL(i2c);
    uint8_t ret;

    if (s->cmd == ACCEL_OUT_X || s->cmd == ACCEL_OUT_Y || s->cmd == ACCEL_OUT_Z) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        lis302dl_sample(s, now);
        if (s->cmd == ACCEL_OUT_X) lis302dl_trace_poll(s, now);
    }
    switch(s->cmd) {
        case ACCEL_WHOAMI:
            ret = ACCEL_WHOAMI_VALUE;
            break;
        case ACCEL_STATUS:
            /* All axes have fresh data available (ZYXDA + per-axis DA), no
             * overrun. A driver that polls STATUS for data-ready before
             * reading OUT_X/Y/Z needs this to be non-zero. */
            ret = 0x0F;
            break;
        /*
         * The iOS AppleLIS302DL driver only sets the PD (power, 0x40) bit in
         * CTRL_REG1 and then polls OUT_X/Y/Z -- it never sets the per-axis
         * enable bits, and CTRL_REG1 even reads back 0 between poll cycles.
         * Gating the outputs on per-axis (or even PD) bits therefore starves
         * the OS of samples and rotation never happens. Report the injected
         * acceleration whenever the sensor is not held in explicit power-down,
         * i.e. as long as CTRL_REG1 has ever been programmed non-zero.
         */
        case ACCEL_OUT_X:
            ret = (uint8_t)s->out_x;
            break;
        case ACCEL_OUT_Y:
            ret = (uint8_t)s->out_y;
            break;
        case ACCEL_OUT_Z:
            ret = (uint8_t)s->out_z;
            break;
        case ACCEL_CTRL_REG1:
            ret = s->ctrl_reg1;
            break;
        case ACCEL_CTRL_REG2:
            ret = s->ctrl_reg2;
            break;
        case ACCEL_CTRL_REG3:
            ret = s->ctrl_reg3;
            break;
        default:
            if (lis302dl_debug()) {
                printf("%s: unknown register 0x%02x\n", __func__, s->cmd);
            }
            ret = 0;
            break;
    }
    if (lis302dl_debug()) {
        printf("lis302dl: read reg 0x%02x -> 0x%02x\n", s->cmd, ret);
    }
    /* honor the multi-byte / auto-increment read the driver uses to slurp
     * OUT_X/Y/Z in one burst */
    s->cmd++;
    return ret;
}

static int lis302dl_send(I2CSlave *i2c, uint8_t data)
{
    LIS302DLState *s = LIS302DL(i2c);

    if (!s->pointer_set) {
        s->cmd = data;
        s->pointer_set = true;
        return 0;
    }

    /* a data byte written to the current register */
    switch (s->cmd) {
        case ACCEL_CTRL_REG1: s->ctrl_reg1 = data; break;
        case ACCEL_CTRL_REG2:
            /*
             * BOOT (bit 6) reloads the part's trimming registers and the
             * hardware clears it when that finishes. AppleLIS302DL sets it in
             * enableAccelerometer and polls for it to return to zero, and
             * panics the kernel outright if it has not cleared within 500 ms:
             *   "AppleLIS302DL::enableAccelerometer - Boot bit did not return
             *    to zero in 500 msecs. That is wrong."
             * Echoing the write back kept the bit set forever, so 3.1.3 panicked
             * as soon as SpringBoard brought the accelerometer up. Complete the
             * reboot immediately and clear the bit.
             */
            s->ctrl_reg2 = data & ~ACCEL_CTRL_REG2_BOOT;
            break;
        case ACCEL_CTRL_REG3: s->ctrl_reg3 = data; break;
        default:
            if (lis302dl_debug()) {
                printf("%s: write 0x%02x to reg 0x%02x\n", __func__, data, s->cmd);
            }
            break;
    }
    s->cmd++;
    return 0;
}

/* --- QMP-drivable orientation / shake, exposed as QOM properties --- */

static void lis302dl_get_orientation(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    int64_t val = s->orientation;
    visit_type_int(v, name, &val, errp);
}

static void lis302dl_set_orientation(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    int64_t val;
    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    lis302dl_apply_orientation(s, (uint32_t)val);
}

static void lis302dl_get_axis(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    /* QOM reports the requested raw vector; I2C reports sampled sensor data. */
    int8_t *axis = (name[0] == 'x') ? &s->base_x : (name[0] == 'y') ? &s->base_y : &s->base_z;
    int64_t val = *axis;
    visit_type_int(v, name, &val, errp);
}

static void lis302dl_set_axis(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    int64_t val;
    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    lis302dl_set_axis_value(s, name[0], (int)CLAMP(val, -128, 127));
}

static void lis302dl_set_shake(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    bool val;
    if (!visit_type_bool(v, name, &val, errp)) {
        return;
    }
    if (val) {
        lis302dl_shake(s);
    }
}

static void lis302dl_init(Object *obj)
{
    LIS302DLState *s = LIS302DL(obj);

    /* Power-on default: upright in portrait. */
    lis302dl_apply_orientation(s, 1);
    s->last_sample_ns = s->shake_start_ns = s->trace_last_poll_ns = -1;
    s->noise_state = 0x302d1;
    s->out_x = s->base_x; s->out_y = s->base_y; s->out_z = s->base_z;

    object_property_add(obj, "orientation", "int",
                        lis302dl_get_orientation, lis302dl_set_orientation, NULL, NULL);
    object_property_add(obj, "x", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "y", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "z", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "shake", "bool", NULL, lis302dl_set_shake, NULL, NULL);
}

static int lis302dl_post_load(void *opaque, int version)
{
    LIS302DLState *s = opaque;
    if (version < 2) {
        s->pitch_mdeg = 0;
        s->flat_pose = s->orientation == 5 || s->orientation == 6;
        s->roll_mdeg = s->orientation == 2 || s->orientation == 6 ? 180000 :
                      s->orientation == 3 ? 90000 : s->orientation == 4 ? -90000 : 0;
    }
    if (s->pitch_mdeg < -180000 || s->pitch_mdeg > 180000 ||
        s->roll_mdeg < -180000 || s->roll_mdeg > 180000) return -EINVAL;
    if (version < 3) { s->rate_hz = 0; s->noise_state = 0x302d1; }
    if (s->rate_hz > 400) return -EINVAL;
    s->last_sample_ns = s->shake_start_ns = s->trace_last_poll_ns = -1;
    s->trace_last_report_ns = s->trace_poll_sum_ns = s->trace_polls = 0;
    s->out_x = s->base_x;
    s->out_y = s->base_y;
    s->out_z = s->base_z;
    return 0;
}

/* Shake is transient host input, not retained after restore: a restore
 * lands with the accelerometer at rest at its steady-state vector. */
static const VMStateDescription vmstate_lis302dl = {
    .name = "lis302dl",
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = lis302dl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, LIS302DLState),
        VMSTATE_UINT32(cmd, LIS302DLState),
        VMSTATE_BOOL(pointer_set, LIS302DLState),
        VMSTATE_INT8(out_x, LIS302DLState),
        VMSTATE_INT8(out_y, LIS302DLState),
        VMSTATE_INT8(out_z, LIS302DLState),
        VMSTATE_INT8(base_x, LIS302DLState),
        VMSTATE_INT8(base_y, LIS302DLState),
        VMSTATE_INT8(base_z, LIS302DLState),
        VMSTATE_UINT32(orientation, LIS302DLState),
        VMSTATE_UINT16(ctrl_reg1, LIS302DLState),
        VMSTATE_UINT16(ctrl_reg2, LIS302DLState),
        VMSTATE_UINT16(ctrl_reg3, LIS302DLState),
        VMSTATE_INT32_V(pitch_mdeg, LIS302DLState, 2),
        VMSTATE_INT32_V(roll_mdeg, LIS302DLState, 2),
        VMSTATE_BOOL_V(flat_pose, LIS302DLState, 2),
        VMSTATE_UINT32_V(rate_hz, LIS302DLState, 3),
        VMSTATE_UINT32_V(noise_state, LIS302DLState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void lis302dl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_lis302dl;
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = lis302dl_event;
    k->recv = lis302dl_recv;
    k->send = lis302dl_send;
}

static const TypeInfo lis302dl_info = {
    .name          = TYPE_LIS302DL,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = lis302dl_init,
    .instance_size = sizeof(LIS302DLState),
    .class_init    = lis302dl_class_init,
};

static void lis302dl_register_types(void)
{
    type_register_static(&lis302dl_info);
}

type_init(lis302dl_register_types)
