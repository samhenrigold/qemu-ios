/*
 * S5L8900 SPI Emulation
 *
 * by cmw
 */

#include "hw/arm/ipod_touch_spi.h"
#include "migration/vmstate.h"

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
    BusState *b = BUS(s->spi);
    BusChild *kid = QTAILQ_FIRST(&b->children);
    if (kid) {
        // TODO GPIO not properly setup yet
        //qemu_set_irq(qdev_get_gpio_in_named(kid->child, SSI_GPIO_CS, 0), (REG(s, R_PIN) & R_PIN_CS) != 0);
    }
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
     * The generic SSI_GPIO_CS path is also unavailable: both peripherals here
     * leave cs_polarity at SSI_CS_NONE, so ssi_peripheral_realize never
     * registers the input GPIO and qdev_get_gpio_in_named would abort.
     *
     * Consequence, which the digitizer model has to live with: the peripherals
     * frame commands purely by counting bytes, so a response whose length
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
    if (REG(s, R_RXCNT) == 0 && REG(s, R_TXCNT) == 0) {
        return;
    }

    apple_spi_update_xfer_tx(s);

    //printf("TX queue: %d, RX queue: %d\n", REG(s, R_TXCNT), REG(s, R_RXCNT));
    //printf("TX buffer size: %d, RX buffer size: %d\n", fifo8_num_used(&s->tx_fifo), fifo8_num_used(&s->rx_fifo));

    while (REG(s, R_TXCNT) && !fifo8_is_empty(&s->tx_fifo)) {
        tx = (uint32_t)fifo8_pop(&s->tx_fifo);
        rx = ssi_transfer(s->spi, tx);
        REG(s, R_TXCNT)--;
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
            val |= (fifo8_num_used(&s->tx_fifo) << R_STATUS_TXFIFO_SHIFT);
            val |= (fifo8_num_used(&s->rx_fifo) << R_STATUS_RXFIFO_SHIFT);
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

    if(addr == R_STATUS) {
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

static uint32_t base_addr = 0;

void set_spi_base(uint32_t base)
{
	base_addr = base;
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
    snprintf(bus_name, sizeof(bus_name), "spi%u.bus", (unsigned)base_addr);
    s->spi = ssi_create_bus(dev, (const char *)bus_name);

    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->cs_line);
    qdev_init_gpio_in_named(dev, apple_spi_cs_set, SSI_GPIO_CS, 1);
    char name[5];
    snprintf(name, 5, "spi%d", base_addr);
    memory_region_init_io(&s->iomem, OBJECT(s), &spi_ops, s, name,
                          SPI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    s->base = base_addr;

    fifo8_create(&s->tx_fifo, R_FIFO_TX_DEPTH);
    fifo8_create(&s->rx_fifo, R_FIFO_RX_DEPTH);

    // create the peripheral
    switch(s->base) {
        case 0:
        {
            DeviceState *dev = ssi_create_peripheral(s->spi, TYPE_IPOD_TOUCH_NOR_SPI);
            IPodTouchNORSPIState *nor = IPOD_TOUCH_NOR_SPI(dev);
            s->nor = nor;
            break;
        }
        case 1:
            break;
        case 4:
        {
            DeviceState *dev = ssi_create_peripheral(s->spi, TYPE_IPOD_TOUCH_MULTITOUCH);
            IPodTouchMultitouchState *mt = IPOD_TOUCH_MULTITOUCH(dev);
            s->mt = mt;
            break;
        }
    }
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

static void ipod_touch_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = ipod_touch_spi_realize;
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