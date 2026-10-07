#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Temporary diagnostic for the 3.1.3 USB bring-up; gated by IT_USB_TRACE. */
static bool usb_phys_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_USB_TRACE") != NULL;
    }
    return on;
}

static uint64_t ipod_touch_usb_phys_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchUSBPhysState *s = (IPodTouchUSBPhysState *) opaque;

    if (usb_phys_trace()) {
        fprintf(stderr, "[USBPHY] R 0x%03x\n", (unsigned)addr);
    }

    switch(addr)
    {
    case REG_OPHYPWR:
        return s->usb_ophypwr;
    case REG_OPHYCLK:
        return s->usb_ophyclk;
    case REG_ORSTCON:
        return s->usb_orstcon;
    case REG_UNKNOWN1:
        return s->usb_unknown1;
    case REG_OPHYTUNE:
        return s->usb_ophytune;
    default:
        return s->regs[(addr & 0xfff) >> 2];
    }
}

static void ipod_touch_usb_phys_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchUSBPhysState *s = (IPodTouchUSBPhysState *) opaque;

    if (usb_phys_trace()) {
        fprintf(stderr, "[USBPHY] W 0x%03x = 0x%08x\n", (unsigned)addr, (unsigned)val);
    }

    switch(addr)
    {
    case REG_OPHYPWR:
        s->usb_ophypwr = val;
        return;
    case REG_OPHYCLK:
        s->usb_ophyclk = val;
        return;
    case REG_ORSTCON:
        s->usb_orstcon = val;
        qemu_set_irq(s->phy_reset, !!(val & ORSTCON_PHY_RESET));
        return;
    case REG_UNKNOWN1:
        s->usb_unknown1 = val;
        return;
    case REG_OPHYTUNE:
        s->usb_ophytune = val;
        return;

    default:
        s->regs[(addr & 0xfff) >> 2] = val;
        return;
    }
}

static const MemoryRegionOps ipod_touch_usb_phys_ops = {
    .read = ipod_touch_usb_phys_read,
    .write = ipod_touch_usb_phys_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_usb_phys_init(Object *obj)
{
    IPodTouchUSBPhysState *s = IPOD_TOUCH_USB_PHYS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_usb_phys_ops, s, TYPE_IPOD_TOUCH_USB_PHYS, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), &s->phy_reset, "phy-reset", 1);
}

/* Preserve the existing zeroed power-on register state. Physical reset is
 * asserted by ORSTCON bit 0; power and clock semantics remain separate. */
static void ipod_touch_usb_phys_reset(DeviceState *dev)
{
    IPodTouchUSBPhysState *s = IPOD_TOUCH_USB_PHYS(dev);

    s->usb_ophypwr = 0;
    s->usb_ophyclk = 0;
    s->usb_orstcon = 0;
    s->usb_unknown1 = 0;
    s->usb_ophytune = 0;
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->phy_reset, 0);
}

static int ipod_touch_usb_phys_post_load(void *opaque, int version_id)
{
    IPodTouchUSBPhysState *s = opaque;
    qemu_set_irq(s->phy_reset, !!(s->usb_orstcon & ORSTCON_PHY_RESET));
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_usb_phys = {
    .name = "ipod_touch_usb_phys",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ipod_touch_usb_phys_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(usb_ophypwr, IPodTouchUSBPhysState),
        VMSTATE_UINT32(usb_ophyclk, IPodTouchUSBPhysState),
        VMSTATE_UINT32(usb_orstcon, IPodTouchUSBPhysState),
        VMSTATE_UINT32(usb_unknown1, IPodTouchUSBPhysState),
        VMSTATE_UINT32(usb_ophytune, IPodTouchUSBPhysState),
        VMSTATE_UINT32_ARRAY(regs, IPodTouchUSBPhysState, 0x400),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_usb_phys_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_usb_phys;
    device_class_set_legacy_reset(dc, ipod_touch_usb_phys_reset);
    
}

static const TypeInfo ipod_touch_usb_phys_type_info = {
    .name = TYPE_IPOD_TOUCH_USB_PHYS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchUSBPhysState),
    .instance_init = ipod_touch_usb_phys_init,
    .class_init = ipod_touch_usb_phys_class_init,
};

static void ipod_touch_usb_phys_register_types(void)
{
    type_register_static(&ipod_touch_usb_phys_type_info);
}

type_init(ipod_touch_usb_phys_register_types)
