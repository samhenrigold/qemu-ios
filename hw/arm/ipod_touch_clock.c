#include "hw/arm/ipod_touch_clock.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "trace.h"

/* IT_CLOCK_TRACE=1: every clock-controller access with a host timestamp. */
static bool clock_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_CLOCK_TRACE") != NULL;
    }
    return on;
}

/* S5L8720 register encoding is corroborated by OpeniBoot's hardware/clock.h
 * and clock_setup(), and by the stock SecureROM/LLB writes. This is the root
 * controller only: the S5L8900 and the secondary block have different layouts.
 * No firmware address, build profile or default running frequency is needed.
 * PLL lock settles immediately for now; analog lock latency remains unmodeled.
 */
static uint32_t s5l8720_pll_locks(const IPodTouchClockState *s)
{
    const uint32_t con[] = { s->pll0con, s->pll1con, s->pll2con };
    uint32_t locks = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(con); i++) {
        if ((s->pllmode & (1U << i)) && ((con[i] >> 24) & 0x3f) &&
            ((con[i] >> 8) & 0xff)) {
            locks |= 1U << i;
        }
    }
    return locks;
}

static void ipod_touch_clock_update(IPodTouchClockState *s)
{
    uint64_t hz = 0;
    unsigned select = (s->config0 >> 12) & 3;
    unsigned div = (s->config1 & (1U << 14)) ?
                   (((s->config1 >> 9) & 0x1f) + 1) * 2 : 1;

    if (s->s5l8720 && s->chipid) {
        /* CHIPID_INFO bit 0 selects the physical oscillator. CONFIG0 source
         * zero bypasses the PLLs; it is not a stopped clock. Stock iBoot's
         * frequency reader corroborates both paths and each PLL's SDIV. */
        uint64_t base = (s->chipid->word2 & 1) ? 24000000 : 12000000;
        if (!select) {
            hz = base / div;
        } else if (s5l8720_pll_locks(s) & (1U << (select - 1))) {
            const uint32_t con[] = { s->pll0con, s->pll1con, s->pll2con };
            unsigned pll = select - 1;
            uint32_t value = con[pll];
            uint64_t ref = (s->pllmode & (1U << (pll + 4))) ? 27000000 : base;
            unsigned shift = (value & 7) + (pll != 0);
            hz = ref * ((value >> 8) & 0xff) / ((value >> 24) & 0x3f) /
                 (1U << shift) / div;
        }
    }
    clock_update_hz(s->pclk, hz);
}

static void s5l8900_clock_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    if (clock_trace()) {
        fprintf(stderr, "[CLOCK %.3f] W %p 0x%03x <- 0x%08x\n",
                g_get_monotonic_time() / 1e6, opaque, (unsigned)addr, (unsigned)val);
    }
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
    ipod_touch_clock_update(s);
}

static uint64_t s5l8900_clock_read_reg(void *opaque, hwaddr addr, unsigned size);

static uint64_t s5l8900_clock_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t v = s5l8900_clock_read_reg(opaque, addr, size);
    if (clock_trace()) {
        fprintf(stderr, "[CLOCK %.3f] R %p 0x%03x -> 0x%08x\n",
                g_get_monotonic_time() / 1e6, opaque, (unsigned)addr, (unsigned)v);
    }
    return v;
}

static uint64_t s5l8900_clock_read_reg(void *opaque, hwaddr addr, unsigned size)
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
            return s->s5l8720 ? s5l8720_pll_locks(s) : 0xf;
        case CLOCK_PLL0LCNT:
            return s->pll0lcnt;
        case CLOCK_PLL1LCNT:
            return s->pll1lcnt;
        case CLOCK_PLL2LCNT:
            return s->pll2lcnt;
        case CLOCK_PLL3LCNT:
            return s->pll3lcnt;
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

    s->pclk = qdev_init_clock_out(dev, "pclk");
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

    if (s->s5l8900) {
        /*
         * The S5L8900's iBoot-204 derives its bus/peripheral clocks from the
         * dividers and PLL settings it finds here (its LLB/bootrom programmed
         * them), so the block comes up with the values a running iPod touch
         * 1G reads back, as devos50's model had them: PLL index/divisor
         * fields in CONFIG0-2, MDIV/PDIV/SDIV per PLL, PLLMODE 0x000a003a.
         */
        s->config0 = (1 << 12) | (1 << 24) | (2 << 16);
        s->config1 = (1 << 12) | (1 << 24) | (3 << 16) | (1 << 8) | 3 |
                     (1 << 20) | (1 << 14) | (1 << 28) | (1 << 30);
        s->config2 = (3 << 28) | (1 << 24) | (1 << 16);
        s->pll0con = (80 << 8) | (8 << 24) | 0;
        s->pll1con = (103 << 8) | (6 << 24) | 0;
        s->pll2con = (156 << 8) | (53 << 24) | 2;
        s->pll3con = (72 << 8) | (8 << 24) | 1;
        s->pllmode = 0x000a003a;
    }
    ipod_touch_clock_update(s);
}

static int ipod_touch_clock_post_load(void *opaque, int version_id)
{
    ipod_touch_clock_update(opaque);
    return 0;
}

#define VMS_CLK(f) VMSTATE_UINT32(f, IPodTouchClockState)

static const VMStateDescription vmstate_ipod_touch_clock = {
    .name = "ipod_touch_clock",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ipod_touch_clock_post_load,
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

static const Property ipod_touch_clock_properties[] = {
    DEFINE_PROP_BOOL("s5l8900", IPodTouchClockState, s5l8900, false),
    DEFINE_PROP_BOOL("s5l8720", IPodTouchClockState, s5l8720, false),
    DEFINE_PROP_LINK("chipid", IPodTouchClockState, chipid,
                     TYPE_IPOD_TOUCH_CHIPID, IPodTouchChipIDState *),
};

static void ipod_touch_clock_realize(DeviceState *dev, Error **errp)
{
    IPodTouchClockState *s = IPOD_TOUCH_CLOCK(dev);
    if (s->s5l8720 && !s->chipid) {
        error_setg(errp, "S5L8720 root clock requires its physical ChipID link");
    }
}

static void s5l8900_clock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_clock_realize;
    dc->vmsd = &vmstate_ipod_touch_clock;
    device_class_set_props(dc, ipod_touch_clock_properties);
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
