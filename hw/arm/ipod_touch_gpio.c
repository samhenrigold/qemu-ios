#include "hw/arm/ipod_touch_gpio.h"
#include "migration/vmstate.h"

/*
 * IT_GPIO_READ_TRACE=1 logs every GPIO pad read. It was unconditional, one line
 * per read, on the vCPU thread with the BQL held -- guest stall, and enough
 * volume to bury anything else on the console. Named apart from sysic.c's
 * IT_GPIO_TRACE, which traces the GPIO *interrupt* block. Cached the same way
 * as the FMSS, MBX and PMU gates; these are not meant to be togglable mid-run.
 */
static bool gpio_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_GPIO_READ_TRACE") != NULL;
    }
    return on;
}

static void s5l8900_gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, addr);
    IPodTouchGPIOState *s = (struct IPodTouchGPIOState *) opaque;

    switch(addr) {
      default:
        break;
    }
}

static uint64_t s5l8900_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    if (gpio_trace()) {
        fprintf(stderr, "%s: read from location 0x%08x\n", __func__, (unsigned)addr);
    }
    IPodTouchGPIOState *s = (struct IPodTouchGPIOState *) opaque;

    switch(addr) {
        case 0x4:
            return 0;
        case 0x24 ... 0x184:
            return s->gpio_state[GPIOADDR2PAD(addr)];
        default:
            break;
    }

    return 0;
}

bool gpio_is_on(uint32_t *state, uint32_t gpio)
{
    return (state[GPIO2PAD(gpio)] & (1 << GPIO2PIN(gpio)));
}

bool gpio_is_off(uint32_t *state, uint32_t gpio)
{
    return !gpio_is_on(state, gpio);
}

void gpio_set_on(uint32_t *state, uint32_t gpio)
{
    state[GPIO2PAD(gpio)] |= (1 << GPIO2PIN(gpio));
}

void gpio_set_off(uint32_t *state, uint32_t gpio)
{
    state[GPIO2PAD(gpio)] &= ~(1 << GPIO2PIN(gpio));
}

static const MemoryRegionOps gpio_ops = {
    .read = s5l8900_gpio_read,
    .write = s5l8900_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchGPIOState *s = IPOD_TOUCH_GPIO(dev);

    memory_region_init_io(&s->iomem, obj, &gpio_ops, s, "gpio", 0x1000);
}

/* Pad state is all driven by the guest; clear it so the second boot does not
 * read back the first boot's pin levels while it is probing. */
static void s5l8900_gpio_reset(DeviceState *dev)
{
    IPodTouchGPIOState *s = IPOD_TOUCH_GPIO(dev);

    memset(s->gpio_state, 0, sizeof(s->gpio_state));

    /*
     * The volume pads rest HIGH, because they are active low.
     *
     * The real device tree flags these two differently from the buttons that
     * work: function-button_hold <gpio 0x0c02 0x100> and button_menu
     * <gpio 0x0c01 0x100> carry 0x100, while button_volup <gpio 0x0902 0x000>
     * and button_voldown <gpio 0x0c00 0x000> carry 0. If 0 means active-low,
     * then a pad we leave at 0 reads to iOS as a button HELD DOWN - from boot,
     * with no input - which would make it emit volume changes continuously and
     * re-show the HUD forever. Both held at once would also make the level
     * wander rather than sit still, which is what the user sees.
     *
     * Confirmed by the user: with these pads parked high the HUD no longer
     * appears or flickers on its own. The matching half of the fix is in
     * ipod_touch_2g.c, where a press now pulls the pad DOWN rather than up.
     * IT_VOLBTN_LEGACY restores the old resting level if it is ever needed for
     * a bisect.
     */
    if (!getenv("IT_VOLBTN_LEGACY")) {
        gpio_set_on(s->gpio_state, GPIO_BUTTON_VOLUP);
        gpio_set_on(s->gpio_state, GPIO_BUTTON_VOLDOWN);
    }
}

static const VMStateDescription vmstate_ipod_touch_gpio = {
    .name = "ipod_touch_gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gpio_state, IPodTouchGPIOState, NUM_GPIO_PADS),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8900_gpio_reset);
    dc->vmsd = &vmstate_ipod_touch_gpio;
}

static const TypeInfo ipod_touch_gpio_info = {
    .name          = TYPE_IPOD_TOUCH_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchGPIOState),
    .instance_init = s5l8900_gpio_init,
    .class_init    = s5l8900_gpio_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_gpio_info);
}

type_init(ipod_touch_machine_types)