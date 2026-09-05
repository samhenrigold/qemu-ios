#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/*
 * Both of these were called on every register access. The trace was
 * unconditional -- one line per read and per write, synchronously inside the
 * MMIO handler with the BQL held -- and IT_DIRECT_IBOOT was a fresh getenv()
 * (a linear scan of environ) up to three times per access. Cached statics, the
 * same pattern as the FMSS/MBX/AMC gates; neither is meant to change mid-run.
 */
static bool dsi_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_DSI_TRACE") != NULL;
    }
    return on;
}

static bool dsi_direct_iboot(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_DIRECT_IBOOT") != NULL;
    }
    return on;
}

static void dsi_panel_read(IPodTouchMIPIDSIState *s, uint32_t header)
{
    /* 7E18 iBoot: generic read, one parameter, panel ID register B1.
     * Reserve both words together so a full FIFO never exposes half a reply. */
    if ((header & 0xffff) != 0xb114 || s->rx_count > 14) {
        return;
    }
    unsigned tail = (s->rx_head + s->rx_count) % 16;
    s->rx_fifo[tail] = DSIM_RSP_LONG_READ | (3 << 8);
    s->rx_fifo[(tail + 1) % 16] = 0x00a1d13c;
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
            uint32_t status = 0x103 | ((s->clkctrl & rDSIM_CLKCTRL_TxRequestHsClk)
                                ? rDSIM_STATUS_TxReadyHsClk : 0);
            /*
             * 3.1.3's iBoot mipi_dsim_init() walks a sequence of "write a DSIM
             * command register, then spin until STATUS shows the command
             * accepted, then spin until it shows the command drained". bit 20
             * (SwRstRelease, after the DSIM_SWRST at 0x04) is a permanent done
             * bit; the escape/FIFO command bits (0x230 = bits 4,5,9) are a
             * request/ack handshake -- they must read set right after the
             * trigger write and then clear, so they are driven by cmd_pending
             * (set on a command write, self-clearing on read) rather than
             * pinned. Gated to the direct 7E18 boot. */
            if (dsi_direct_iboot()) {
                status |= 0x00100000 | s->cmd_pending;
                s->cmd_pending = 0;
            }
            return status;
        }
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
            /* Sending a packet re-arms the command handshake bits. */
            if (dsi_direct_iboot()) {
                s->cmd_pending = 0x230;
            }
            break;
        case REG_INTSRC:
            s->intsrc &= ~val;
            break;
        case 0x04: /* DSIM_SWRST */
            if (val & 1) {
                s->rx_head = s->rx_count = s->intsrc = 0;
            }
            break;
        case 0x14: /* DSIM_ESCMODE: escape-mode command trigger */
            if (dsi_direct_iboot()) {
                s->cmd_pending = 0x230;
            }
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

/* Reset discards partially consumed replies and the old clock handshake. */
static void ipod_touch_mipi_dsi_reset(DeviceState *dev)
{
    IPodTouchMIPIDSIState *s = IPOD_TOUCH_MIPI_DSI(dev);

    s->pkthdr_reg = 0;
    s->clkctrl = 0;
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
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = dsi_post_load,
    .fields = (const VMStateField[]) {
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

static void ipod_touch_mipi_dsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

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
