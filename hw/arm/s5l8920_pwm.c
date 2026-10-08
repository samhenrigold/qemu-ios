/*
 * S5L8920X PWM ('pwm,s5l8920x', also the A4's 'pwm,s5l8930x') at 0x83500000:
 * three channels, each a child node of the DT's pwm (its reg = the channel).
 * The 3GS puts the vibrator on channel 0 and the codec MCLK on 2; the iPhone
 * 4 the vibrator on 1 and the camera strobe on 2.
 *
 * Registers, from the N90 8C148 AppleS5L8920XPWM (start 80791b04, stop
 * 80791a9c, register write 80791a7c):
 *   +0x00 + 8n, +0x04 + 8n  channel n's two counts (the caller's period and
 *                           duty arguments, in that order)
 *   +0x18 + 4n              channel n's control: 0x4003 (or 0x4207 inverted)
 *                           starts it, 0 stops it
 * The driver gates the block's clock on with the first running channel and
 * off with the last. Each channel's output is GPIO out n: high while its
 * control has bit 0 (run) set. The waveform itself is not modeled; nothing on
 * these boards is fed by it but the motor, which only cares that it runs.
 * IOS_PWM_TRACE=1 prints every register write.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define TYPE_S5L8920_PWM "s5l8920.pwm"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8920PWMState, S5L8920_PWM)

#define PWM_CHANNELS    3
#define PWM_CTRL(n)     (0x18 + 4 * (n))
#define PWM_REGS        (PWM_CTRL(PWM_CHANNELS) / 4)
#define PWM_CTRL_RUN    1u

struct S5L8920PWMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq out[PWM_CHANNELS];
    uint32_t regs[PWM_REGS];
};

static void pwm_update(S5L8920PWMState *s)
{
    for (int n = 0; n < PWM_CHANNELS; n++) {
        qemu_set_irq(s->out[n], !!(s->regs[PWM_CTRL(n) / 4] & PWM_CTRL_RUN));
    }
}

static uint64_t pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8920PWMState *s = opaque;

    if (offset < sizeof(s->regs)) {
        return s->regs[offset / 4];
    }
    qemu_log_mask(LOG_UNIMP, "s5l8920.pwm: read 0x%" HWADDR_PRIx "\n", offset);
    return 0;
}

static void pwm_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    S5L8920PWMState *s = opaque;

    if (getenv("IOS_PWM_TRACE")) {
        fprintf(stderr, "%.3f s5l8920.pwm: [0x%02" HWADDR_PRIx "] = 0x%" PRIx64 "\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6, offset, value);
    }
    if (offset >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "s5l8920.pwm: write 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n",
                      offset, value);
        return;
    }
    s->regs[offset / 4] = value;
    pwm_update(s);
}

static const MemoryRegionOps pwm_ops = {
    .read = pwm_read,
    .write = pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void pwm_reset(DeviceState *dev)
{
    S5L8920PWMState *s = S5L8920_PWM(dev);

    memset(s->regs, 0, sizeof(s->regs));
    pwm_update(s);
}

static int pwm_post_load(void *opaque, int version_id)
{
    pwm_update(opaque);
    return 0;
}

static const VMStateDescription pwm_vmstate = {
    .name = TYPE_S5L8920_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pwm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8920PWMState, PWM_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void pwm_init(Object *obj)
{
    S5L8920PWMState *s = S5L8920_PWM(obj);

    memory_region_init_io(&s->iomem, obj, &pwm_ops, s, TYPE_S5L8920_PWM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out(DEVICE(obj), s->out, PWM_CHANNELS);
}

static void pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, pwm_reset);
    dc->vmsd = &pwm_vmstate;
}

static const TypeInfo pwm_info = {
    .name          = TYPE_S5L8920_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8920PWMState),
    .instance_init = pwm_init,
    .class_init    = pwm_class_init,
};

static void pwm_register_types(void)
{
    type_register_static(&pwm_info);
}

type_init(pwm_register_types)
