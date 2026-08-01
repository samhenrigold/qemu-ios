#include "hw/arm/ipod_touch_sysic.h"

static uint64_t ipod_touch_sysic_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchSYSICState *s = (IPodTouchSYSICState *) opaque;

    //fprintf(stderr, "%s: offset = 0x%08x\n", __func__, addr);

    switch (addr) {
        case POWER_ID:
            /*
             * 0x39700044: low bits are the POWER_ID power-control scratch, but
             * bits[31:24] are the read-only fused boot security epoch. iBoot's
             * miu_init reads this word and panics ("Epoch Mismatch", which trips
             * the watchdog) unless the top byte equals the image epoch (4 on the
             * S5L8720 / iPod touch 2G). A normal boot gets that top byte latched
             * by the SecureROM before iBoot runs; when we substitute the boot
             * chain (IT_DIRECT_IBOOT) the ROM never runs and iBoot's own
             * power-control writes here would clobber it, so synthesise the epoch
             * top byte on read. Env-gated: a normal 2.1.1 boot is untouched.
             */
            if (getenv("IT_DIRECT_IBOOT")) {
                uint32_t epoch = 4;
                if (getenv("IT_DIRECT_EPOCH")) {
                    epoch = (uint32_t)strtoul(getenv("IT_DIRECT_EPOCH"), NULL, 0);
                }
                return (s->power_id & 0x00FFFFFFu) | (epoch << 24);
            }
            return s->power_id;
        case POWER_SETSTATE:
        case POWER_STATE:
            return s->power_state;
        case 0x7a:
        case 0x7c:
            return 1;
        case GPIO_INTLEVEL ... (GPIO_INTLEVEL + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTLEVEL) / 4;
            return s->gpio_int_level[group];
        }
        case GPIO_INTSTAT ... (GPIO_INTSTAT + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTSTAT) / 4;
            return s->gpio_int_status[group];
        }
        case GPIO_INTEN ... (GPIO_INTEN + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTEN) / 4;
            return s->gpio_int_enabled[group];
        }
        case GPIO_INTTYPE ... (GPIO_INTTYPE + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTTYPE) / 4;
            return s->gpio_int_type[group];
        }
      default:
        break;
    }
    return 0;
}

static void ipod_touch_sysic_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchSYSICState *s = (IPodTouchSYSICState *) opaque;

    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, val, addr);

    switch (addr) {
        case POWER_ID:
            s->power_id = val;
            break;
        case POWER_ONCTRL:
            if((val & 0x20) != 0 || (val & 0x4) != 0 || (val & POWER_ID_ADM) != 0) { break; } // make sure that we do not record the 'on' state of some devices so it appears like they are turned on immediately.
            s->power_state = val;
            break;
        case POWER_OFFCTRL:
            s->power_state = val;
            break;
        case GPIO_INTLEVEL ... (GPIO_INTLEVEL + GPIO_NUMINTGROUPS * 4):
        {
            break;
        }
        case GPIO_INTSTAT ... (GPIO_INTSTAT + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTSTAT) / 4;

            // acknowledge the interrupts and clear the corresponding bits
            s->gpio_int_status[group] = s->gpio_int_status[group] & ~val;

            qemu_irq_lower(s->gpio_irqs[group]);

            break;
        }
        case GPIO_INTEN ... (GPIO_INTEN + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTEN) / 4;
            s->gpio_int_enabled[group] = val;
            break;
        }
        case GPIO_INTTYPE ... (GPIO_INTTYPE + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTTYPE) / 4;
            s->gpio_int_type[group] = val;
            break;
        }
        default:
            break;
    }
}

static const MemoryRegionOps ipod_touch_sysic_ops = {
    .read = ipod_touch_sysic_read,
    .write = ipod_touch_sysic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_sysic_init(Object *obj)
{
    IPodTouchSYSICState *s = IPOD_TOUCH_SYSIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_sysic_ops, s, TYPE_IPOD_TOUCH_SYSIC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    for(int grp = 0; grp < GPIO_NUMINTGROUPS; grp++) {
        sysbus_init_irq(sbd, &s->gpio_irqs[grp]);
    }
}

/*
 * The GPIO interrupt fabric has to go back to power-on state on a warm reset.
 *
 * gpio_int_status is the pending latch and gpio_int_enabled the mask; devices
 * signal through them (the digitizer raises its attention line by setting
 * gpio_int_status[3] bit 13 and pulsing gpio_irqs[3]). Carrying either across a
 * reset means the next boot's driver either sees a stale pending interrupt it
 * never armed, or arms a line that already reads asserted -- and the edge it is
 * actually waiting for never arrives.
 *
 * The output lines are dropped too, so nothing is left asserted into the VIC
 * across the reset.
 */
static void ipod_touch_sysic_reset(DeviceState *dev)
{
    IPodTouchSYSICState *s = IPOD_TOUCH_SYSIC(dev);

    s->power_id = 0;
    s->power_state = 0;

    for (int grp = 0; grp < GPIO_NUMINTGROUPS; grp++) {
        s->gpio_int_level[grp] = 0;
        s->gpio_int_status[grp] = 0;
        s->gpio_int_enabled[grp] = 0;
        s->gpio_int_type[grp] = 0;
        if (s->gpio_irqs[grp]) {
            qemu_irq_lower(s->gpio_irqs[grp]);
        }
    }
}

static void ipod_touch_sysic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_touch_sysic_reset;
}

static const TypeInfo ipod_touch_sysic_type_info = {
    .name = TYPE_IPOD_TOUCH_SYSIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchSYSICState),
    .instance_init = ipod_touch_sysic_init,
    .class_init = ipod_touch_sysic_class_init,
};

static void ipod_touch_sysic_register_types(void)
{
    type_register_static(&ipod_touch_sysic_type_info);
}

type_init(ipod_touch_sysic_register_types)
