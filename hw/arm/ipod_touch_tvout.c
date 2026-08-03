#include "hw/arm/ipod_touch_tvout.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

static bool tvout_trace(void)
{
    static int on = -1;
    if (on < 0) { on = getenv("IT_TVOUT_DEBUG") != NULL; }
    return on;
}
#define TVT(fmt, ...) do { if (tvout_trace()) { \
    printf("[TVOUT] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

/* Any register touch retires the vblank pulse, so the line cannot stick. */
static void tvout_ack_vblank(IPodTouchTVOutState *s)
{
    if (s->irq2_pending) {
        s->irq2_pending = false;
        if (s->irq2) {
            qemu_irq_lower(s->irq2);
        }
    }
}

static void tvout_vblank(void *opaque)
{
    IPodTouchTVOutState *s = (IPodTouchTVOutState *)opaque;

    /*
     * Post a frame interrupt on the SDO line. AppleM2TVOut's SDO filter
     * requires SDO_IRQ bit 0, and its handler is what retires queued swaps;
     * without a periodic one, IOMobileFramebuffer's close path sleeps forever
     * waiting for the swap queue to drain. Only raise while nothing is pending
     * - the driver acknowledges by writing SDO_IRQ, which lowers the line.
     */
    if (!s->sdo_irq && (s->sdo_irq_mask & 1) == 0) {
        s->sdo_irq = 0x1;
        if (s->irq) {
            qemu_irq_raise(s->irq);
        }
        TVT("vblank -> raise SDO irq");
    }
    timer_mod(s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16 * 1000 * 1000);
}

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
        case SDO_FIELDCTL:
            /*
             * Bit 1 selects the interlaced-field path in both AppleM2TVOut SDO
             * handlers: with it set they additionally require MXR_CFG bit 2 to
             * match the current field, and a mismatch makes them return without
             * retiring the swap. The IT_TVOUT_READY blanket returned all-ones
             * here, so that check could never pass and the framebuffer's
             * in-flight swap was never completed - which is what left
             * SpringBoard asleep in IOServiceClose. Report progressive.
             */
            return 0;
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

    TVT("sdo   wr %#06x <- %#010x", (unsigned)offset, (unsigned)value);

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
            tvout_ack_vblank(s);
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
        case MXR_INTSTAT:
            /*
             * Mixer interrupt status. Bit 0 is "mixer underrun": AppleM2TVOut's
             * second interrupt filter reads it, and if it is set logs
             * "mixer UF: %08x" and reports a MixerUnderrun to the framebuffer.
             * The IT_TVOUT_READY blanket used to return all-ones here, so the
             * driver saw a permanent underrun and did nothing else.
             */
            return s->mixer1_intstat;
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

    TVT("mix1  wr %#06x <- %#010x (irqmask=%#x pending=%#x)", (unsigned)offset,
        (unsigned)value, s->sdo_irq_mask, s->sdo_irq);

    switch(offset) {
        case MXR_INTSTAT:
            s->mixer1_intstat &= ~(uint32_t)value;   /* write-1-to-clear */
            break;
        case MXR_STATUS:
            /*
             * Starting the mixer completes a swap, which the driver learns
             * about through the SDO interrupt.
             *
             * This used to be capped at two interrupts ever ("ugly hack for
             * now"). 3.1.3 swaps continuously, and AppleM2TVOut sleeps on the
             * swap-complete event on SpringBoard's own thread, so once the cap
             * was reached SpringBoard blocked forever and the watchdog killed
             * it. The cap is unnecessary: raising only while no interrupt is
             * already pending is enough to keep the line from sticking high,
             * because the driver acknowledges by writing SDO_IRQ, which lowers
             * it and re-arms us.
             */
            if((value & 0x1) && (s->sdo_irq_mask & 1) == 0 && !s->sdo_irq) {
                s->sdo_irq = 0x1;
                qemu_irq_raise(s->irq);
                TVT("mix1  -> RAISE sdo irq");
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
    sysbus_init_irq(sbd, &s->irq2);

    s->vblank_shim = getenv("IT_TVOUT_VBLANK") != NULL;
    if (s->vblank_shim) {
        s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tvout_vblank, s);
        timer_mod(s->vblank_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 16 * 1000 * 1000);
    }
}

/* vblank_shim is an environment switch, not guest state, and vblank_timer only
 * exists when it is on -- a NULL timer pointer in a VMStateField aborts the
 * save. It is a free-running periodic timer that the destination arms for
 * itself at realize, so there is nothing to carry across. */
static const VMStateDescription vmstate_ipod_touch_tvout = {
    .name = "ipod_touch_tvout",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(irq2_pending, IPodTouchTVOutState),
        VMSTATE_UINT32(mixer1_intstat, IPodTouchTVOutState),
        VMSTATE_UINT32(mixer1_status, IPodTouchTVOutState),
        VMSTATE_UINT32(mixer1_cfg, IPodTouchTVOutState),
        VMSTATE_UINT32(mixer2_status, IPodTouchTVOutState),
        VMSTATE_UINT32(mixer2_cfg, IPodTouchTVOutState),
        VMSTATE_UINT32(sdo_clkcon, IPodTouchTVOutState),
        VMSTATE_UINT32(sdo_config, IPodTouchTVOutState),
        VMSTATE_UINT32(sdo_irq, IPodTouchTVOutState),
        VMSTATE_UINT32(sdo_irq_mask, IPodTouchTVOutState),
        VMSTATE_UINT32(irq_count, IPodTouchTVOutState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_tvout_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_tvout;

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