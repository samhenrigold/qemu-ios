#include "hw/arm/ipod_touch_tvout.h"
#include "qapi/error.h"

static uint64_t ipod_touch_tvout_sdo_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: offset = 0x%08x\n", __func__, offset);

    switch(offset) {
        case SDO_CLKCON:
            /*
             * Bit 1 is a hardware status bit ("SDO clock stable/ready"), not
             * part of what the driver writes. 3.1.3's AppleM2TVOut shutdown
             * path spins on `readSDOReg(SDO_CLKCON) & 0x2` and logs
             * "TVOUT SHUT DOWN PROBLEM: !(readSDOReg(SDO_CLKCON) & 0x2)" when
             * it never asserts. That wait runs on the SpringBoard thread, so
             * the UI never comes up. Echoing only the written value can never
             * satisfy it -- report the clock as ready.
             */
            return s->sdo_clkcon | (getenv("IT_TVOUT_READY") ? 0x2 : 0);
        case SDO_CONFIG:
            return s->sdo_config;
        case SDO_IRQ:
            return s->sdo_irq;
        case SDO_IRQMASK:
            return s->sdo_irq_mask;
        default:
            /*
             * The other two shutdown gates (enable.reg.clkgating_rdy and
             * mix_ctrl.reg.reg_mixer_ready_clk_down) live at offsets this model
             * does not implement yet. Report ready so the close path completes;
             * bring-up probe, to be replaced once the bits are pinned down.
             */
            if (getenv("IT_TVOUT_READY")) {
                return 0xffffffff;
            }
            break;
    }

    return 0;
}

static void ipod_touch_tvout_sdo_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: writing 0x%08x to 0x%08x\n", __func__, value, offset);

    switch(offset) {
        case SDO_CLKCON:
            s->sdo_clkcon = value;
            return;
        case SDO_CONFIG:
            s->sdo_config = value;
            return;
        case SDO_IRQ:
            s->sdo_irq = 0x0;
            qemu_irq_lower(s->irq);
            return;
        case SDO_IRQMASK:
            s->sdo_irq_mask = value;
            return;
    }
}

static uint64_t ipod_touch_tvout_mixer1_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: offset = 0x%08x\n", __func__, offset);

    switch(offset) {
        case MXR_STATUS:
            /* reg_mixer_ready_clk_down lives in the mixer status/ctrl block;
             * report every ready bit while probing the shutdown gates. */
            return getenv("IT_TVOUT_READY") ? 0xffffffff : 0x4;
        case MXR_CFG:
            return getenv("IT_TVOUT_READY") ? (s->mixer1_cfg | 0xffff0000)
                                            : s->mixer1_cfg;
        default:
            if (getenv("IT_TVOUT_READY")) {
                return 0xffffffff;   /* unknown ready gates -- bring-up probe */
            }
            break;
    }

    return 0;
}

static void ipod_touch_tvout_mixer1_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: writing 0x%08x to 0x%08x\n", __func__, value, offset);

    switch(offset) {
        case MXR_STATUS:
            if((value & 0x1) && (s->sdo_irq_mask & 1) == 0 && s->irq_count < 2) {
                s->sdo_irq = 0x1;
                s->irq_count += 1; // ugly hack for now
                qemu_irq_raise(s->irq);
            }
            break;
        case MXR_CFG:
            s->mixer1_cfg = value;
            break;
    }
}

static uint64_t ipod_touch_tvout_mixer2_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: offset = 0x%08x\n", __func__, offset);

    switch(offset) {
        case MXR_STATUS:
            return getenv("IT_TVOUT_READY") ? 0xffffffff : s->mixer2_status;
        case MXR_CFG:
            return getenv("IT_TVOUT_READY") ? (s->mixer2_cfg | 0xffff0000)
                                            : s->mixer2_cfg;
        default:
            if (getenv("IT_TVOUT_READY")) {
                return 0xffffffff;   /* unknown ready gates -- bring-up probe */
            }
            break;
    }

    return 0;
}

static void ipod_touch_tvout_mixer2_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    //printf("%s: writing 0x%08x to 0x%08x\n", __func__, value, offset);

    switch(offset) {
        case MXR_STATUS:
            s->mixer2_status = value;
            break;
        case MXR_CFG:
            s->mixer2_cfg = value;
            break;
    }
}

static const MemoryRegionOps ipod_touch_tvout_sdo_ops = {
    .read = ipod_touch_tvout_sdo_read,
    .write = ipod_touch_tvout_sdo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps ipod_touch_tvout_mixer1_ops = {
    .read = ipod_touch_tvout_mixer1_read,
    .write = ipod_touch_tvout_mixer1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps ipod_touch_tvout_mixer2_ops = {
    .read = ipod_touch_tvout_mixer2_read,
    .write = ipod_touch_tvout_mixer2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_tvout_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    IPodTouchTVOutState *s = IPOD_TOUCH_TVOUT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    // mixer 1
    memory_region_init_io(&s->mixer1_iomem, obj, &ipod_touch_tvout_mixer1_ops, s, "tvout_mixer1", 4096);
    sysbus_init_mmio(sbd, &s->mixer1_iomem);

    // mixer 2
    memory_region_init_io(&s->mixer2_iomem, obj, &ipod_touch_tvout_mixer2_ops, s, "tvout_mixer2", 4096);
    sysbus_init_mmio(sbd, &s->mixer2_iomem);

    // SDO
    memory_region_init_io(&s->sdo_iomem, obj, &ipod_touch_tvout_sdo_ops, s, "tvout_sdo", 4096);
    sysbus_init_mmio(sbd, &s->sdo_iomem);

    sysbus_init_irq(sbd, &s->irq);
}

static void ipod_touch_tvout_class_init(ObjectClass *klass, void *data)
{

}

static const TypeInfo ipod_touch_tvout_type_info = {
    .name = TYPE_IPOD_TOUCH_TVOUT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchTVOutState),
    .instance_init = ipod_touch_tvout_init,
    .class_init = ipod_touch_tvout_class_init,
};

static void ipod_touch_tvout_register_types(void)
{
    type_register_static(&ipod_touch_tvout_type_info);
}

type_init(ipod_touch_tvout_register_types)