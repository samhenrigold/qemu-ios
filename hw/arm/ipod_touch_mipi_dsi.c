#include "hw/arm/ipod_touch_mipi_dsi.h"

static uint64_t ipod_touch_mipi_dsi_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr != 0x00000)
	fprintf(stderr, "%s: read from location 0x%08lx\n", __func__, addr);

    IPodTouchMIPIDSIState *s = (IPodTouchMIPIDSIState *)opaque;
    switch(addr)
    {
        case REG_STATUS:
            // TxReadyHsClk has to follow the clock request in CLKCTRL rather
            // than being wired on. Panel bring-up sets CLKCTRL bit 31 and spins
            // until this bit reads set; panel shutdown clears bit 31 and spins
            // until it reads clear. Reporting it permanently set satisfied
            // bring-up but made shutdown spin forever, wedging the kernel
            // mid-power-down -- which is why the display never came back from
            // idle sleep, and why the reboot path never reached the watchdog.
            return 0x103 | ((s->clkctrl & rDSIM_CLKCTRL_TxRequestHsClk)
                                ? rDSIM_STATUS_TxReadyHsClk : 0);
        case REG_INTSRC:
            return rDSIM_INTSRC_RxDatDone;
        case REG_RXFIFO:
            if(!s->return_panel_id) {
                s->return_panel_id = true;
                // TODO this should be rewritten as a proper queue!
                return DSIM_RSP_LONG_READ | (3 << 8); // the latter part indicates the length of the response (the panel ID)
            } else {
                s->return_panel_id = false;
                return 0x00a1d13c;
            }
            
        case REG_FIFOCTRL:
            return rDSIM_FIFOCTRL_EmptyHSfr;
        default:
            printf("%s: read invalid location 0x%08lx.\n", __func__, addr);
            break;
    }
    return 0;
}

static void ipod_touch_mipi_dsi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMIPIDSIState *s = (IPodTouchMIPIDSIState *)opaque;
    fprintf(stderr, "%s: writing 0x%08lx to 0x%08lx\n", __func__, val, addr);

    switch(addr)
    {
        case REG_PKTHDR:
            s->pkthdr_reg = val;
            break;
        case REG_CLKCTRL:
            // Remember the HS clock request; STATUS.TxReadyHsClk mirrors it.
            s->clkctrl = val;
            break;
        default:
            break;
    }
}

static const MemoryRegionOps mipi_dsi_ops = {
    .read = ipod_touch_mipi_dsi_read,
    .write = ipod_touch_mipi_dsi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/*
 * Warm resets have to put the panel link back to its power-on state, or the
 * second boot brings the display up against leftovers from the first.
 *
 * return_panel_id is the worst of them: the panel ID is delivered as a
 * two-part response and this flag says which half comes next. If a reset lands
 * mid-sequence the next boot's first read gets the second half, the ID does not
 * match, and the panel is never brought up -- a headless boot.
 *
 * clkctrl matters too: the guest clears the HS clock request on its way down,
 * so without a reset the link starts the next boot already marked disabled.
 */
static void ipod_touch_mipi_dsi_reset(DeviceState *dev)
{
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(dev);

    s->pkthdr_reg = 0;
    s->clkctrl = 0;
    s->return_panel_id = false;
}

static void ipod_touch_mipi_dsi_realize(DeviceState *dev, Error **errp)
{
    
}

static void ipod_touch_mipi_dsi_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(dev);

    memory_region_init_io(&s->iomem, obj, &mipi_dsi_ops, s, "mipi_dsi", 0x10000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->return_panel_id = 0;
}

static void ipod_touch_mipi_dsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_mipi_dsi_realize;
    dc->reset = ipod_touch_mipi_dsi_reset;
}

static const TypeInfo ipod_touch_mipi_dsi_info = {
    .name          = TYPE_IPOD_TOUCH_MIPI_DSI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMIPIDSIState),
    .instance_init = ipod_touch_mipi_dsi_init,
    .class_init    = ipod_touch_mipi_dsi_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_mipi_dsi_info);
}

type_init(ipod_touch_machine_types)
