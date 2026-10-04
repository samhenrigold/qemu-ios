/*
 * S5L8900 SPI Emulation
 *
 * by cmw
 */

#include "hw/arm/ipod_touch_spi.h"
#include "migration/vmstate.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "trace.h"

static int apple_spi_word_size(IPodTouchSPIState *s)
{
    switch (R_CFG_WORD_SIZE(REG(s, R_CFG))) {
    case R_CFG_WORD_SIZE_8B:
        return 1;
    case R_CFG_WORD_SIZE_16B:
        return 2;
    case R_CFG_WORD_SIZE_32B:
        return 4;
    default:
        break;
    }
    /*
     * Word size 3 is reserved, but the field is guest-writable, so a stray
     * write used to abort QEMU. Fall back to a byte -- any choice is wrong,
     * but a wrong transfer width beats killing the machine.
     */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "[SPI] reserved word size in CFG; assuming 8 bits\n");
    return 1;
}

static void apple_spi_update_xfer_tx(IPodTouchSPIState *s)
{
    if (fifo8_is_empty(&s->tx_fifo)) {
        REG(s, R_STATUS) |= R_STATUS_TXEMPTY;
    }
}

static void apple_spi_update_xfer_rx(IPodTouchSPIState *s)
{
    if (!fifo8_is_empty(&s->rx_fifo)) {
        REG(s, R_STATUS) |= R_STATUS_RXREADY;
    }
}

static void apple_spi_update_irq(IPodTouchSPIState *s)
{
    uint32_t irq = 0;
    uint32_t mask = 0;

    if (REG(s, R_CFG) & R_CFG_IE_RXREADY) {
        mask |= R_STATUS_RXREADY;
    }
    if (REG(s, R_CFG) & R_CFG_IE_TXEMPTY) {
        mask |= R_STATUS_TXEMPTY;
    }
    if (REG(s, R_CFG) & R_CFG_IE_COMPLETE) {
        mask |= R_STATUS_COMPLETE;
    }

    if (REG(s, R_STATUS) & mask) {
        irq = 1;
    }
    if (irq != s->last_irq) {
        s->last_irq = irq;
        qemu_set_irq(s->irq, irq);
    }
}

static void apple_spi_update_cs(IPodTouchSPIState *s)
{
    trace_ipod_touch_spi_cs(s->base, REG(s, R_PIN));
    /*
     * MEASURED, so that nobody spends another day on this TODO: R_PIN is not a
     * per-transaction chip select on this controller, and wiring it through
     * would not give the peripherals a transaction boundary.
     *
     * Tracing every R_PIN write against the digitizer's command stream over a
     * whole 3.1.3 boot: R_PIN is written once every 16 bytes -- the RX FIFO
     * depth -- always driving the SAME level, in the middle of commands
     * (cur_cmd 0xeb, buf_ind 16, then 32, then 48, then 64 of a 75-byte frame
     * read). It is a FIFO-refill artifact. The R_CTRL FIFO reset fires on the
     * same 16-byte cadence and is no better.
     *
     * NOR chip select is GPIO pad 0 pin 0, wired separately by the machine.
     * The digitizer still has SSI_CS_NONE and frames commands by counting
     * bytes, so a response whose length
     * disagrees with what the guest clocks desynchronises them permanently.
     * See get_empty_frame() in ipod_touch_multitouch.c.
     */
}

static void apple_spi_cs_set(void *opaque, int pin, int level)
{
    IPodTouchSPIState *s = IPOD_TOUCH_SPI(opaque);
    if (level) {
        REG(s, R_PIN) |= R_PIN_CS;
    } else {
        REG(s, R_PIN) &= ~R_PIN_CS;
    }
    apple_spi_update_cs(s);
}

static void apple_spi_run(IPodTouchSPIState *s)
{
    uint32_t tx;
    uint32_t rx;

    if (!(REG(s, R_CTRL) & R_CTRL_RUN)) {
        return;
    }
    /*
     * The S5L8900 block has no TX count: iBoot-204's spi_write pushes up to
     * eight bytes into TXDATA, sets RUN and waits for the interrupt (its
     * driver at 0x1800ba48 never touches R_TXCNT), so the FIFO drains on its
     * own. The S5L8720/S5L8930 drivers always program R_TXCNT first.
     */
    bool drain_tx = s->s5l8900 && !fifo8_is_empty(&s->tx_fifo);
    if (REG(s, R_RXCNT) == 0 && REG(s, R_TXCNT) == 0 && !drain_tx) {
        return;
    }

    apple_spi_update_xfer_tx(s);

    while ((REG(s, R_TXCNT) || drain_tx) && !fifo8_is_empty(&s->tx_fifo)) {
        tx = (uint32_t)fifo8_pop(&s->tx_fifo);
        rx = ssi_transfer(s->spi, tx);
        if (REG(s, R_TXCNT)) {
            REG(s, R_TXCNT)--;
        }
        apple_spi_update_xfer_tx(s);
        if (REG(s, R_RXCNT) > 0) {
            if (fifo8_is_full(&s->rx_fifo)) {
                /* A full RX FIFO is a guest pacing error, not a QEMU bug: set
                 * the overflow status bit and drop the byte instead of
                 * aborting the whole process (the abort used to shadow this
                 * recovery, which was already written). */
                qemu_log_mask(LOG_GUEST_ERROR, "%s: rx overflow\n", __func__);
                REG(s, R_STATUS) |= R_STATUS_RXOVERFLOW;
            } else {
                fifo8_push(&s->rx_fifo, (uint8_t)rx);
                REG(s, R_RXCNT)--;
                apple_spi_update_xfer_rx(s);
            }
        }
    }

    // fetch the remaining bytes by sending sentinel bytes.
    while (!fifo8_is_full(&s->rx_fifo) && (REG(s, R_RXCNT) > 0) && (REG(s, R_CFG) & R_CFG_AGD)) {
        rx = ssi_transfer(s->spi, 0xff);
        /* The loop condition already guarantees the FIFO is not full here. */
        fifo8_push(&s->rx_fifo, (uint8_t)rx);
        REG(s, R_RXCNT)--;
        apple_spi_update_xfer_rx(s);
    }
    if (REG(s, R_RXCNT) == 0 && REG(s, R_TXCNT) == 0) {
        REG(s, R_STATUS) |= R_STATUS_COMPLETE;
    }

    //printf("<after> TX buffer size: %d, RX buffer size: %d\n", fifo8_num_used(&s->tx_fifo), fifo8_num_used(&s->rx_fifo));
}

static uint64_t ipod_touch_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchSPIState *s = IPOD_TOUCH_SPI(opaque);
    //printf("%s (base %d): read from location 0x%08x\n", __func__, s->base, addr);

    uint32_t r;
    bool run = false;

    r = s->regs[addr >> 2];
    switch (addr) {
        case R_RXDATA: {
            const uint8_t *buf = NULL;
            int word_size = apple_spi_word_size(s);
            uint32_t num = 0;
            if (fifo8_is_empty(&s->rx_fifo)) {
                /*
                 * Reading RXDATA with an empty FIFO returns 0 rather than
                 * aborting QEMU (a bare guest MMIO read could reach here).
                 *
                 * `run` mirrors what the normal path below does when a read
                 * drains the FIFO: kick apple_spi_run() so the controller can
                 * refill it, rather than leaving it un-kicked. Consistency
                 * only -- this was investigated as a suspect for the Doodle
                 * Jump 100%-CPU hang and is NOT the cause of it (that was an
                 * unmodelled MBX status bit; see ipod_touch_mbx.c 0x12c).
                 */
                qemu_log_mask(LOG_GUEST_ERROR, "%s: rx underflow\n", __func__);
                r = 0;
                run = true;
                break;
            }
            buf = fifo8_pop_bufptr(&s->rx_fifo, word_size, &num);
            memcpy(&r, buf, num);

            if (fifo8_is_empty(&s->rx_fifo)) {
                run = true;
            }
            break;
        }
        case R_STATUS: {
            int val = 0;
            if (s->s5l8900) {
                /* 8-deep FIFOs, counts at [7:4] and [11:8] (iBoot-204's SPI ISR at 0x1800bc4c/0x1800bc96). */
                val |= (fifo8_num_used(&s->tx_fifo) << 4);
                val |= (fifo8_num_used(&s->rx_fifo) << 8);
            } else {
                val |= (fifo8_num_used(&s->tx_fifo) << R_STATUS_TXFIFO_SHIFT);
                val |= (fifo8_num_used(&s->rx_fifo) << R_STATUS_RXFIFO_SHIFT);
            }
            r |= val;
            break;
        }
        default:
            break;
    }

    if (run) {
        apple_spi_run(s);
    }
    apple_spi_update_irq(s);
    return r;
}

static void ipod_touch_spi_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    IPodTouchSPIState *s = IPOD_TOUCH_SPI(opaque);
    //printf("%s (base %d): writing 0x%08x to 0x%08x\n", __func__, s->base, data, addr);

    uint32_t r = data;
    uint32_t *mmio = &REG(s, addr);
    uint32_t old = *mmio;
    bool cs_flg = false;
    bool run = false;

    switch (addr) {
    case R_CTRL:
        if (r & R_CTRL_TX_RESET) {
            fifo8_reset(&s->tx_fifo);
        }
        if (r & R_CTRL_RX_RESET) {
            fifo8_reset(&s->rx_fifo);
        }
        if (r & R_CTRL_RUN && !fifo8_is_empty(&s->tx_fifo)) {
            run = true;
        }
        break;
    case R_STATUS:
        run = true;
        r = old & (~r);
        break;
    case R_PIN:
        cs_flg = true;
        break;
    case R_TXDATA ... R_TXDATA + 3: {
        int word_size = apple_spi_word_size(s);
        if (fifo8_is_full(&s->tx_fifo) || fifo8_num_free(&s->tx_fifo) < word_size) {
            /* Drop the write instead of aborting when the guest overruns TX. */
            qemu_log_mask(LOG_GUEST_ERROR, "%s: tx overflow\n", __func__);
            break;
        }
        fifo8_push_all(&s->tx_fifo, (uint8_t *)&r, word_size);
        break;
    case R_CFG:
        run = true;
        break;
    }
    default:
        break;
    }

    *mmio = r;
    if (cs_flg) {
        apple_spi_update_cs(s);
    }
    if (run) {
        apple_spi_run(s);
    }

    /*
     * On the S5L8900 TXEMPTY/RXREADY are latched events the ISR acknowledges
     * by writing STATUS back (iBoot-204 at 0x1800bcd4); re-asserting them as
     * FIFO levels here left the line high and the CPU in the ISR forever.
     */
    if (addr == R_STATUS && !s->s5l8900) {
        apple_spi_update_xfer_tx(s);
        apple_spi_update_xfer_rx(s);
    }

    apple_spi_update_irq(s);
}

static const MemoryRegionOps spi_ops = {
    .read = ipod_touch_spi_read,
    .write = ipod_touch_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_spi_reset(DeviceState *d)
{
    IPodTouchSPIState *s = (IPodTouchSPIState *)d;
	memset(s->regs, 0, sizeof(s->regs));
    fifo8_reset(&s->tx_fifo);
    fifo8_reset(&s->rx_fifo);
    /*
     * last_irq is the level we last drove, and apple_spi_update_irq only
     * touches the line when the level CHANGES. Leaving it set across a reset
     * meant the line stayed asserted with no status bits to justify it -- a
     * phantom SPI interrupt inherited from the previous boot, arriving exactly
     * as iBoot runs spi_init(). Drive it low here rather than only clearing
     * the shadow, so the line and our idea of it agree.
     */
    s->last_irq = 0;
    qemu_set_irq(s->irq, 0);
}

static void ipod_touch_spi_realize(DeviceState *dev, struct Error **errp)
{
    IPodTouchSPIState *s = IPOD_TOUCH_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /*
     * Every SPI controller is created with sysbus_create_simple(), which leaves
     * dev->id NULL, so this produced five buses all named "(null).bus". The
     * controller index is the only thing that distinguishes them.
     */
    char bus_name[32] = { 0 };
    snprintf(bus_name, sizeof(bus_name), "spi%u.bus", (unsigned)s->base);
    s->spi = ssi_create_bus(dev, (const char *)bus_name);

    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->cs_line);
    qdev_init_gpio_in_named(dev, apple_spi_cs_set, SSI_GPIO_CS, 1);
    char name[5];
    snprintf(name, 5, "spi%d", s->base);
    memory_region_init_io(&s->iomem, OBJECT(s), &spi_ops, s, name,
                          SPI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    fifo8_create(&s->tx_fifo, R_FIFO_TX_DEPTH);
    fifo8_create(&s->rx_fifo, R_FIFO_RX_DEPTH);

    /*
     * The peripheral on the bus is a property, not a process global: the
     * S5L8720 hangs the NOR on SPI0 and the digitizer on SPI4, the S5L8930 the
     * same two on SPI0/SPI1, the S5L8900 the LCD panel on SPI1 and the
     * digitizer on SPI2. Anything not listed here is a controller with nothing
     * attached (reads return 0).
     */
    const char *periph = s->peripheral ? s->peripheral : "none";
    if (!strcmp(periph, "nor")) {
        s->nor = IPOD_TOUCH_NOR_SPI(ssi_create_peripheral(s->spi, TYPE_IPOD_TOUCH_NOR_SPI));
    } else if (!strcmp(periph, "multitouch")) {
        s->mt = IPOD_TOUCH_MULTITOUCH(ssi_create_peripheral(s->spi, TYPE_IPOD_TOUCH_MULTITOUCH));
    } else if (!strcmp(periph, "none")) {
        /* nothing on the bus */
    } else {
        /* Any other SSI peripheral type registered in this binary; a digitizer
         * of another protocol (the S5L8900's Zephyr1) is still the touch input. */
        DeviceState *p = ssi_create_peripheral(s->spi, periph);
        s->mt = (IPodTouchMultitouchState *)object_dynamic_cast(OBJECT(p), TYPE_IPOD_TOUCH_MULTITOUCH);
    }
}

DeviceState *ipod_touch_spi_create(hwaddr addr, qemu_irq irq, unsigned index,
                                   const char *peripheral, bool s5l8900)
{
    DeviceState *dev = qdev_new(TYPE_IPOD_TOUCH_SPI);
    qdev_prop_set_uint8(dev, "index", index);
    qdev_prop_set_string(dev, "peripheral", peripheral);
    qdev_prop_set_bit(dev, "s5l8900", s5l8900);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    return dev;
}

/*
 * last_irq is our shadow of the level we drove; apple_spi_update_irq only
 * touches the line when that shadow changes, so restoring last_irq = 1 without
 * driving the line would leave the guest waiting for an interrupt that is
 * already "delivered" as far as this model is concerned. Drive the line to
 * match the restored shadow.
 */
static int ipod_touch_spi_post_load(void *opaque, int version_id)
{
    IPodTouchSPIState *s = opaque;

    qemu_set_irq(s->irq, s->last_irq);
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_spi = {
    .name = "ipod_touch_spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ipod_touch_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(last_irq, IPodTouchSPIState),
        VMSTATE_UINT32_ARRAY(regs, IPodTouchSPIState, SPI_MMIO_SIZE >> 2),
        VMSTATE_UINT8(base, IPodTouchSPIState),
        VMSTATE_FIFO8(rx_fifo, IPodTouchSPIState),
        VMSTATE_FIFO8(tx_fifo, IPodTouchSPIState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ipod_touch_spi_properties[] = {
    DEFINE_PROP_UINT8("index", IPodTouchSPIState, base, 0),
    DEFINE_PROP_STRING("peripheral", IPodTouchSPIState, peripheral),
    DEFINE_PROP_BOOL("s5l8900", IPodTouchSPIState, s5l8900, false),
};

static void ipod_touch_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = ipod_touch_spi_realize;
    device_class_set_props(dc, ipod_touch_spi_properties);
    device_class_set_legacy_reset(dc, ipod_touch_spi_reset);
    dc->vmsd = &vmstate_ipod_touch_spi;
}

static const TypeInfo ipod_touch_spi_info = {
    .name          = TYPE_IPOD_TOUCH_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchSPIState),
    .class_init    = ipod_touch_spi_class_init,
};

static void ipod_touch_spi_register_types(void)
{
    type_register_static(&ipod_touch_spi_info);
}

type_init(ipod_touch_spi_register_types)