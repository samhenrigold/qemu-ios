#include "hw/arm/ipod_touch_clock.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "trace.h"

static void s5l8900_clock_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchClockState *s = (struct IPodTouchClockState *) opaque;

    trace_ipod_touch_clock_write(addr, val);
    switch (addr) {
        case CLOCK_CONFIG0:
            s->config0 = val;
            break;
        case CLOCK_CONFIG1:
            s->config1 = val;
            break;
        case CLOCK_CONFIG2:
            s->config2 = val;
            break;
        case CLOCK_CONFIG3:
            s->config3 = val;
            break;
        case CLOCK_CONFIG4:
            s->config4 = val;
            break;
        case CLOCK_CONFIG5:
            s->config5 = val;
            break;

        case CLOCK_PLL0CON:
            s->pll0con = val;
            break;
        case CLOCK_PLL1CON:
            s->pll1con = val;
            break;
        case CLOCK_PLL2CON:
            s->pll2con = val;
            break;
        case CLOCK_PLL3CON:
            s->pll3con = val;
            break;
        case CLOCK_PLL0LCNT:
            s->pll0lcnt = val;
            break;
        case CLOCK_PLL1LCNT:
            s->pll1lcnt = val;
            break;
        case CLOCK_PLL2LCNT:
            s->pll2lcnt = val;
            break;
        case CLOCK_PLL3LCNT:
            s->pll3lcnt = val;
            break;
        case CLOCK_PLLLOCK:
            /* Read-only status register. A guest write is a driver bug, not a
             * reason to kill the machine. */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write to read-only PLLLOCK register 0x%08x\n",
                          __func__, (uint32_t)addr);
            break;
        case CLOCK_PLLMODE:
            s->pllmode = val;
            break;
        case CLOCK_PWRCON0:
            s->pwrcon0 = val;
            break;
        case CLOCK_PWRCON1:
            s->pwrcon1 = val;
            break;
        case CLOCK_PWRCON2:
            s->pwrcon2 = val;
            break;
        case CLOCK_PWRCON3:
            s->pwrcon3 = val;
            break;
        case CLOCK_PWRCON4:
            s->pwrcon4 = val;
            break;
      default:
            qemu_log_mask(LOG_UNIMP,
                          "%s: write 0x%08x to unknown clock register 0x%08x\n",
                          __func__, (uint32_t)val, (uint32_t)addr);
    }
}

static uint64_t s5l8900_clock_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchClockState *s = (struct IPodTouchClockState *) opaque;

    switch (addr) {
        case CLOCK_CONFIG0:
            return s->config0;
        case CLOCK_CONFIG1:
            return s->config1;
        case CLOCK_CONFIG2:
            return s->config2;
        case CLOCK_CONFIG3:
            return s->config3;
        case CLOCK_CONFIG4:
            return s->config4;
        case CLOCK_CONFIG5:
            return s->config5;

        case CLOCK_PLL0CON:
            return s->pll0con;
        case CLOCK_PLL1CON:
            return s->pll1con;
        case CLOCK_PLL2CON:
            return s->pll2con;
        case CLOCK_PLL3CON:
            return s->pll3con;
        case CLOCK_PLLLOCK:
            return (1 | 2 | 4 | 8); // all PLLs are locked
        case CLOCK_PLLMODE:
            return s->pllmode;
        case CLOCK_PWRCON0:
            return s->pwrcon0;
        case CLOCK_PWRCON1:
            return s->pwrcon1;
        case CLOCK_PWRCON2:
            return s->pwrcon2;
        case CLOCK_PWRCON3:
            return s->pwrcon3;
        case CLOCK_PWRCON4:
            return s->pwrcon4;
      default:
            qemu_log_mask(LOG_UNIMP,
                          "%s: read from unknown clock register 0x%08x\n",
                          __func__, (uint32_t)addr);
    }
    return 0;
}

static const MemoryRegionOps clock_ops = {
    .read = s5l8900_clock_read,
    .write = s5l8900_clock_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_clock_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchClockState *s = IPOD_TOUCH_CLOCK(dev);

    memory_region_init_io(&s->iomem, obj, &clock_ops, s, "clock", 0x80);
}

/* Every field here is a register the guest writes during its own clock setup;
 * none is latched from the image or the machine, so zero is the reset value. */
static void ipod_touch_clock_reset(DeviceState *dev)
{
    IPodTouchClockState *s = IPOD_TOUCH_CLOCK(dev);

    s->config0 = 0; s->config1 = 0; s->config2 = 0;
    s->config3 = 0; s->config4 = 0; s->config5 = 0;
    s->pll0con = 0; s->pll1con = 0; s->pll2con = 0; s->pll3con = 0;
    s->pll0lcnt = 0; s->pll1lcnt = 0; s->pll2lcnt = 0; s->pll3lcnt = 0;
    s->pllmode = 0;
    s->pwrcon0 = 0; s->pwrcon1 = 0; s->pwrcon2 = 0;
    s->pwrcon3 = 0; s->pwrcon4 = 0;
}

#define VMS_CLK(f) VMSTATE_UINT32(f, IPodTouchClockState)

static const VMStateDescription vmstate_ipod_touch_clock = {
    .name = "ipod_touch_clock",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMS_CLK(config0), VMS_CLK(config1), VMS_CLK(config2),
        VMS_CLK(config3), VMS_CLK(config4), VMS_CLK(config5),
        VMS_CLK(pll0con), VMS_CLK(pll1con), VMS_CLK(pll2con), VMS_CLK(pll3con),
        VMS_CLK(pll0lcnt), VMS_CLK(pll1lcnt), VMS_CLK(pll2lcnt), VMS_CLK(pll3lcnt),
        VMS_CLK(pllmode),
        VMS_CLK(pwrcon0), VMS_CLK(pwrcon1), VMS_CLK(pwrcon2),
        VMS_CLK(pwrcon3), VMS_CLK(pwrcon4),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_clock_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_clock;
    device_class_set_legacy_reset(dc, ipod_touch_clock_reset);
}

static const TypeInfo ipod_touch_clock_info = {
    .name          = TYPE_IPOD_TOUCH_CLOCK,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchClockState),
    .instance_init = s5l8900_clock_init,
    .class_init    = s5l8900_clock_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_clock_info);
}

type_init(ipod_touch_machine_types)