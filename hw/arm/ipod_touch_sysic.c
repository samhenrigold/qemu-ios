#include "hw/arm/ipod_touch_sysic.h"
#include "migration/vmstate.h"

/*
 * Cached: consulted on every GPIO interrupt-status access, and the guest polls
 * those. getenv() scans environ each call, on the vCPU thread.
 */
static bool sysic_gpio_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_GPIO_TRACE") != NULL;
    }
    return on;
}


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
            return group < GPIO_NUMINTGROUPS ? s->gpio_int_level[group] : 0;
        }
        case GPIO_INTSTAT ... (GPIO_INTSTAT + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTSTAT) / 4;
            uint32_t v = group < GPIO_NUMINTGROUPS ? s->gpio_int_status[group] : 0;
            if (v && sysic_gpio_trace()) {
                fprintf(stderr, "[gpio] STAT  group %u -> %08x\n", group, v);
            }
            return v;
        }
        case GPIO_INTEN ... (GPIO_INTEN + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTEN) / 4;
            return group < GPIO_NUMINTGROUPS ? s->gpio_int_enabled[group] : 0;
        }
        case GPIO_INTTYPE ... (GPIO_INTTYPE + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTTYPE) / 4;
            return group < GPIO_NUMINTGROUPS ? s->gpio_int_type[group] : 0;
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

            /*
             * The register block has EIGHT slots per range (they are 0x20
             * apart) but only GPIO_NUMINTGROUPS arrays behind them, and fewer
             * qemu_irqs than that are actually connected. Indexing group 7 read
             * past gpio_int_level into gpio_int_status, and lowered an irq off
             * the end of gpio_irqs[] - calling through whatever followed it,
             * interpreted as an IRQState*.
             *
             * Claim the address (so it does not fall through to default and
             * silently read as 0) but ignore an out-of-range group.
             */
            if (group < GPIO_NUMINTGROUPS) {
                if (sysic_gpio_trace()) {
                    fprintf(stderr, "[gpio] ACK   group %u <- %08x "
                            "(stat %08x -> %08x)\n", group, (uint32_t)val,
                            s->gpio_int_status[group],
                            s->gpio_int_status[group] & ~(uint32_t)val);
                }
                // acknowledge the interrupts and clear the corresponding bits
                s->gpio_int_status[group] = s->gpio_int_status[group] & ~val;
                /*
                 * KNOWN GAP, left alone on purpose. Lowering unconditionally
                 * discards any OTHER source still pending in the same group --
                 * group 3 carries the digitizer's attention line alongside all
                 * four buttons -- and no model re-raises a level that is
                 * already latched, so that interrupt is simply lost. The
                 * obvious repair (re-assert while status & enabled) was tried
                 * on 2026-08-06 together with a matching i2c change and the
                 * pair hung the boot at the logo; reverted rather than shipped
                 * on a theory. Fix it WITH a boot test, one change at a time.
                 */
                if (s->gpio_irqs[group]) {
                    qemu_irq_lower(s->gpio_irqs[group]);
                }
            }
            break;
        }
        case GPIO_INTEN ... (GPIO_INTEN + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTEN) / 4;
            if (group < GPIO_NUMINTGROUPS) {
                /*
                 * IT_GPIO_TRACE=1: which GPIO interrupt sources the guest arms,
                 * and when. Answering "does the driver actually enable the
                 * source it then sleeps on" needs this; a source that is never
                 * armed and one that is armed but never asserted look identical
                 * from the driver's side.
                 */
                if (sysic_gpio_trace()) {
                    fprintf(stderr, "[gpio] INTEN group %u <- %08x "
                            "(was %08x, stat %08x)\n", group, (uint32_t)val,
                            s->gpio_int_enabled[group],
                            s->gpio_int_status[group]);
                }
                s->gpio_int_enabled[group] = val;
            }
            break;
        }
        case GPIO_INTTYPE ... (GPIO_INTTYPE + GPIO_NUMINTGROUPS * 4):
        {
            uint8_t group = (addr - GPIO_INTTYPE) / 4;
            if (group < GPIO_NUMINTGROUPS) {
                s->gpio_int_type[group] = val;
            }
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

static const VMStateDescription vmstate_ipod_touch_sysic = {
    .name = "ipod_touch_sysic",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(power_id, IPodTouchSYSICState),
        VMSTATE_UINT32(power_state, IPodTouchSYSICState),
        VMSTATE_UINT32_ARRAY(gpio_int_level, IPodTouchSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_UINT32_ARRAY(gpio_int_status, IPodTouchSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_UINT32_ARRAY(gpio_int_enabled, IPodTouchSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_UINT32_ARRAY(gpio_int_type, IPodTouchSYSICState, GPIO_NUMINTGROUPS),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_sysic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_sysic;

    device_class_set_legacy_reset(dc, ipod_touch_sysic_reset);
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
