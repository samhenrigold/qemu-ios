#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"

/* Cache opt-in tracing; lane/reset state belongs to the physical DSIM. */
static bool dsi_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_DSI_TRACE") != NULL;
    }
    return on;
}

static uint32_t dsi_lane_mask(IPodTouchMIPIDSIState *s)
{
    return (1u << MIN(s->lanes, 4)) - 1;
}

static void dsi_panel_read(IPodTouchMIPIDSIState *s, uint32_t header)
{
    /* 7E18 iBoot: generic read, one parameter, panel ID register B1.
     * Reserve both words together so a full FIFO never exposes half a reply. */
    if ((header & 0xffff) != 0xb114 || s->rx_count > 14) {
        return;
    }
    unsigned tail = (s->rx_head + s->rx_count) % 16;
    s->rx_fifo[tail] = DSIM_RSP_LONG_READ | (s->panel_id_len << 8);
    s->rx_fifo[(tail + 1) % 16] = s->panel_id;
    s->rx_count += 2;
    s->intsrc |= rDSIM_INTSRC_RxDatDone;
}

static uint64_t ipod_touch_mipi_dsi_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr != 0x00000 && dsi_trace()) {
        fprintf(stderr, "%s: read from location 0x%08" PRIx64 "\n", __func__, addr);
    }

    IPodTouchMIPIDSIState *s = (IPodTouchMIPIDSIState *)opaque;
    switch(addr)
    {
        case REG_STATUS: {
            // TxReadyHsClk has to follow the clock request in CLKCTRL rather
            // than being wired on. Panel bring-up sets CLKCTRL bit 31 and spins
            // until this bit reads set; panel shutdown clears bit 31 and spins
            // until it reads clear. Reporting it permanently set satisfied
            // bring-up but made shutdown spin forever, wedging the kernel
            // mid-power-down -- which is why the display never came back from
            // idle sleep, and why the reboot path never reached the watchdog.
            /* bits 0-3: per-lane stop state, one per configured lane. Bit 8
             * (StopStateClk) is the clock lane idling in LP mode, the
             * complement of the HS clock request: iOS 4.3's DSI driver
             * (AppleS5L8720X-91) snapshots STATUS at start and takes
             * StopStateClk set as "interface not enabled", so pinning it made
             * the panel's first display-off a "redundant disable request"
             * panic. */
            uint32_t status = s->ulps_data ? dsi_lane_mask(s) << 4 : dsi_lane_mask(s);
            status |= s->ulps_clock ? 0x200 :
                ((s->clkctrl & rDSIM_CLKCTRL_TxRequestHsClk) ?
                 rDSIM_STATUS_TxReadyHsClk : rDSIM_STATUS_StopStateClk);
            /* S5L8720 STATUS_SWRST follows an actual SWRST request, independent
             * of the board's boot strategy. Reset execution is synchronous;
             * analog completion latency remains unmodeled. Reads are inert. */
            if (s->swrst_released) {
                status |= rDSIM_STATUS_SwRstRelease;
            }
            return status;
        }
        case REG_ESCMODE:
            return s->escmode;
        case REG_INTSRC:
            return s->intsrc;
        case REG_RXFIFO: {
            if (!s->rx_count) {
                return 0;
            }
            uint32_t word = s->rx_fifo[s->rx_head];
            s->rx_head = (s->rx_head + 1) % 16;
            s->rx_count--;
            return word;
        }
        case REG_FIFOCTRL:
            return rDSIM_FIFOCTRL_EmptyHSfr;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: read invalid location 0x%08" PRIx64 ".\n",
                          __func__, addr);
            break;
    }
    return 0;
}

/*
 * Panel power as the guest last commanded it over DCS: display off / sleep in
 * (0x28/0x10) vs display on / sleep out (0x29/0x11), as short writes with no
 * or one parameter (data types 0x05/0x15). The iPad's ApplePinotLCD turns the
 * panel off this way when it sleeps; the app bridge reads it as "sleeping".
 */
static int dsi_panel_off;

bool ipod_touch_mipi_dsi_panel_off(void)
{
    return qatomic_read(&dsi_panel_off);
}

static void dsi_note_dcs(uint32_t header)
{
    unsigned type = header & 0x3f, cmd = (header >> 8) & 0xff;

    if (type != 0x05 && type != 0x15) {
        return;
    }
    if (cmd == 0x28 || cmd == 0x10) {
        qatomic_set(&dsi_panel_off, 1);
    } else if (cmd == 0x29 || cmd == 0x11) {
        qatomic_set(&dsi_panel_off, 0);
    }
}

static void ipod_touch_mipi_dsi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMIPIDSIState *s = (IPodTouchMIPIDSIState *)opaque;
    if (dsi_trace()) {
        fprintf(stderr, "%s: writing 0x%08" PRIx64 " to 0x%08" PRIx64 "\n", __func__, val, addr);
    }

    switch(addr)
    {
        case REG_PKTHDR:
            s->pkthdr_reg = val;
            dsi_panel_read(s, val);
            dsi_note_dcs(val);
            break;
        case REG_INTSRC:
            s->intsrc &= ~val;
            break;
        case REG_SWRST: /* DSIM_SWRST */
            if (val & 1) {
                s->rx_head = s->rx_count = s->intsrc = 0;
                s->escmode = 0;
                s->ulps_clock = s->ulps_data = false;
                s->swrst_released = true;
            }
            break;
        case REG_ESCMODE: {
            /* Enter requests act on assertion; exit has priority. Leaving an
             * enter bit asserted while clearing exit must not re-enter ULPS:
             * stock firmware clears exit before finally clearing enter. */
            uint32_t rising = val & ~s->escmode;
            if (val & 1) {
                s->ulps_clock = false;
            } else if (rising & 2) {
                s->ulps_clock = true;
            }
            if (val & 4) {
                s->ulps_data = false;
            } else if (rising & 8) {
                s->ulps_data = true;
            }
            s->escmode = val;
            break;
        }
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

/* Reset discards partially consumed replies and the old clock handshake. */
static void ipod_touch_mipi_dsi_reset(DeviceState *dev)
{
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(dev);

    s->swrst_released = false;
    s->escmode = 0;
    s->ulps_clock = s->ulps_data = false;
    s->pkthdr_reg = 0;
    /* kboot= skips iBoot, whose pinot_init leaves the panel lit with the HS
     * clock running; the kernel's boot_args says the framebuffer is up, and
     * 4.3's DSI driver reads the clock lane to decide whether the link is. */
    s->clkctrl = s->hs_clock_at_reset ? rDSIM_CLKCTRL_TxRequestHsClk : 0;
    s->cmd_pending = 0;
    s->return_panel_id = false;
    s->rx_head = s->rx_count = s->intsrc = 0;
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

static int dsi_post_load(void *opaque, int version_id)
{
    IPodTouchMIPIDSIState *s = opaque;
    if (version_id < 4) {
        s->escmode = 0;
        s->ulps_clock = s->ulps_data = false;
        s->cmd_pending = 0;
    }
    if (version_id < 3) {
        /* Old streams did not record completion of a software-reset request. */
        s->swrst_released = false;
    }
    if (version_id == 1) {
        /* The old model had an implicit reply on every other FIFO read. */
        s->rx_head = s->rx_count = s->intsrc = 0;
        if (s->return_panel_id) {
            s->rx_fifo[0] = 0x00a1d13c;
            s->rx_count = 1;
            s->intsrc = rDSIM_INTSRC_RxDatDone;
        }
    }
    return s->rx_head < 16 && s->rx_count <= 16 ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_ipod_touch_mipi_dsi = {
    .name = "ipod_touch_mipi_dsi",
    .version_id = 4,
    .minimum_version_id = 1,
    .post_load = dsi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_V(escmode, IPodTouchMIPIDSIState, 4),
        VMSTATE_BOOL_V(ulps_clock, IPodTouchMIPIDSIState, 4),
        VMSTATE_BOOL_V(ulps_data, IPodTouchMIPIDSIState, 4),
        VMSTATE_BOOL_V(swrst_released, IPodTouchMIPIDSIState, 3),
        VMSTATE_UINT32(pkthdr_reg, IPodTouchMIPIDSIState),
        VMSTATE_UINT32(clkctrl, IPodTouchMIPIDSIState),
        VMSTATE_UINT32(cmd_pending, IPodTouchMIPIDSIState),
        VMSTATE_BOOL(return_panel_id, IPodTouchMIPIDSIState),
        VMSTATE_UINT32_ARRAY_V(rx_fifo, IPodTouchMIPIDSIState, 16, 2),
        VMSTATE_UINT32_V(rx_head, IPodTouchMIPIDSIState, 2),
        VMSTATE_UINT32_V(rx_count, IPodTouchMIPIDSIState, 2),
        VMSTATE_UINT32_V(intsrc, IPodTouchMIPIDSIState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ipod_touch_mipi_dsi_properties[] = {
    DEFINE_PROP_UINT32("lanes", IPodTouchMIPIDSIState, lanes, 2),
    /* the iPod touch 2G panel's ID, three bytes */
    DEFINE_PROP_UINT32("panel-id", IPodTouchMIPIDSIState, panel_id, 0x00a1d13c),
    DEFINE_PROP_UINT32("panel-id-len", IPodTouchMIPIDSIState, panel_id_len, 3),
};

static void ipod_touch_mipi_dsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, ipod_touch_mipi_dsi_properties);

    dc->realize = ipod_touch_mipi_dsi_realize;
    device_class_set_legacy_reset(dc, ipod_touch_mipi_dsi_reset);
    dc->vmsd = &vmstate_ipod_touch_mipi_dsi;
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
