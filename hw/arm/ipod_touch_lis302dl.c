#include "hw/arm/ipod_touch_lis302dl.h"
#include "qapi/error.h"
#include "qapi/visitor.h"

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

/* Map an orientation index to a steady-state gravity vector in the raw
 * accelerometer frame. Indices follow UIDeviceOrientation ordering. */
void lis302dl_apply_orientation(LIS302DLState *s, uint32_t o)
{
    int8_t x = 0, y = 0, z = 0;
    switch (o) {
        case 1: /* portrait, home button at bottom */          x = 0;         y = -ACCEL_1G; z = 0;        break;
        case 2: /* portrait upside down, home button at top */  x = 0;         y = ACCEL_1G;  z = 0;        break;
        case 3: /* landscape, home button on the right */       x = ACCEL_1G;  y = 0;         z = 0;        break;
        case 4: /* landscape, home button on the left */        x = -ACCEL_1G; y = 0;         z = 0;        break;
        case 5: /* face up */                                   x = 0;         y = 0;         z = -ACCEL_1G; break;
        case 6: /* face down */                                 x = 0;         y = 0;         z = ACCEL_1G;  break;
        default: /* 0 / unknown: leave flat-portrait default */ x = 0;         y = -ACCEL_1G; z = 0;        break;
    }
    s->orientation = o;
    s->base_x = x; s->base_y = y; s->base_z = z;
    s->out_x = x; s->out_y = y; s->out_z = z;
    if (lis302dl_debug()) {
        printf("lis302dl: orientation=%u -> x=%d y=%d z=%d\n", o, x, y, z);
    }
}

static void lis302dl_shake_tick(void *opaque)
{
    LIS302DLState *s = opaque;
    if (s->shake_ticks <= 0) {
        /* settle back to the steady-state vector */
        s->out_x = s->base_x;
        s->out_y = s->base_y;
        s->out_z = s->base_z;
        return;
    }
    /* alternating large impulse on top of the resting vector */
    int8_t imp = (s->shake_ticks & 1) ? 0x7F : -0x7F;
    s->out_x = imp;
    s->out_y = (int8_t)(s->base_y + (imp >> 1));
    s->out_z = s->base_z;
    s->shake_ticks--;
    timer_mod(s->shake_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 40);
}

void lis302dl_shake(LIS302DLState *s)
{
    s->shake_ticks = 12;
    timer_mod(s->shake_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 40);
    if (lis302dl_debug()) {
        printf("lis302dl: shake\n");
    }
}

void lis302dl_set_axis_value(LIS302DLState *s, char axis, int v)
{
    if (v < -128) v = -128;
    if (v > 127) v = 127;
    switch (axis) {
        case 'x': s->out_x = s->base_x = (int8_t)v; break;
        case 'y': s->out_y = s->base_y = (int8_t)v; break;
        case 'z': s->out_z = s->base_z = (int8_t)v; break;
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
    int8_t *axis = (name[0] == 'x') ? &s->out_x : (name[0] == 'y') ? &s->out_y : &s->out_z;
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
    lis302dl_set_axis_value(s, name[0], (int)val);
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

    /* power-on default: resting flat in portrait so an OS that reads before
     * a host sets orientation still sees a sane gravity vector */
    lis302dl_apply_orientation(s, 1);
    s->shake_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, lis302dl_shake_tick, s);

    object_property_add(obj, "orientation", "int",
                        lis302dl_get_orientation, lis302dl_set_orientation, NULL, NULL);
    object_property_add(obj, "x", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "y", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "z", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "shake", "bool", NULL, lis302dl_set_shake, NULL, NULL);
}

static void lis302dl_class_init(ObjectClass *klass, void *data)
{
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
