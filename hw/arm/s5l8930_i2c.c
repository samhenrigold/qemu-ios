/*
 * S5L8930 ("A4") I2C controller and the Dialog D1815 PMU that hangs off i2c0.
 *
 * Controller contract: docs/ipad1/research/gap-kernel-platform-mmio.md §4.2,
 * re-checked against the 7B500 AppleS5L8920XI2CController (enable c06377b4,
 * transfer c063792c, interruptFilter c0637888). PMU register traffic: §4.1,
 * re-checked against 7B500 AppleD1815PMU (start c0662a60, IRQ c06622ec,
 * shutdown c0661770, ADC c0663462) and AppleD1815PMURTC (c0667170-c06673c0).
 *
 * One transfer is programmed whole and completes inside the command write,
 * so the FIFO is just two byte arrays; nothing is clocked.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"
#include "system/runstate.h"

/* ---- controller ---- */

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930I2CState, S5L8930_I2C)

#define I2C_ADDR        0x00    /* 7-bit slave address, DT reg value as-is */
#define I2C_CTRL        0x08    /* 0xF0 enable (kext), 0x30 (openiBoot), 0 off */
#define I2C_STATUS      0x0C    /* write-1-to-clear; enable writes 0x37 */
#define I2C_SUBADDR     0x10
#define I2C_LEN         0x18
#define I2C_FIFO        0x20
#define I2C_CMD         0x24    /* bit0 write, bit2 start */

#define STATUS_DONE     (1u << 4)
#define STATUS_ERROR    (1u << 5)   /* NACK / no slave: kIOReturn 0xe00002e9 */
#define CMD_WRITE       (1u << 0)
#define CMD_START       (1u << 2)
#define I2C_FIFO_DEPTH  0x80        /* the kext caps a message at 0x80 bytes */

struct S5L8930I2CState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t addr, ctrl, status, subaddr, len;
    uint8_t tx[I2C_FIFO_DEPTH];
    uint8_t rx[I2C_FIFO_DEPTH];
    uint32_t tx_len, rx_len, rx_pos;
};

static void i2c_update_irq(S5L8930I2CState *s)
{
    qemu_set_irq(s->irq, (s->status & s->ctrl) != 0);
}

/* Slave address, then subaddress, then either the data bytes or a repeated
 * start and the read-back. Any missing ACK ends it with STATUS_ERROR. */
static void i2c_do_transfer(S5L8930I2CState *s, bool write)
{
    uint32_t n = MIN(s->len, I2C_FIFO_DEPTH);
    uint32_t i;
    bool nak;

    s->rx_len = s->rx_pos = 0;
    nak = i2c_start_send(s->bus, s->addr & 0x7f) ||
          i2c_send(s->bus, s->subaddr & 0xff);
    if (!nak && write) {
        for (i = 0; i < MIN(n, s->tx_len) && !nak; i++) {
            nak = i2c_send(s->bus, s->tx[i]);
        }
    } else if (!nak) {
        nak = i2c_start_recv(s->bus, s->addr & 0x7f);
        for (i = 0; i < n && !nak; i++) {
            s->rx[i] = i2c_recv(s->bus);
        }
        s->rx_len = nak ? 0 : n;
    }
    i2c_end_transfer(s->bus);
    s->tx_len = 0;
    s->status |= nak ? STATUS_ERROR : STATUS_DONE;
    i2c_update_irq(s);
}

static uint64_t s5l8930_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8930I2CState *s = opaque;

    switch (offset) {
    case I2C_ADDR:
        return s->addr;
    case I2C_CTRL:
        return s->ctrl;
    case I2C_STATUS:
        return s->status;
    case I2C_SUBADDR:
        return s->subaddr;
    case I2C_LEN:
        return s->len;
    case I2C_FIFO:
        return s->rx_pos < s->rx_len ? s->rx[s->rx_pos++] : 0;
    case I2C_CMD:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled read 0x%04x\n",
                      TYPE_S5L8930_I2C, (unsigned)offset);
        return 0;
    }
}

static void s5l8930_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    S5L8930I2CState *s = opaque;

    switch (offset) {
    case I2C_ADDR:
        s->addr = value;
        break;
    case I2C_CTRL:
        s->ctrl = value;
        i2c_update_irq(s);
        break;
    case I2C_STATUS:
        s->status &= ~value;
        i2c_update_irq(s);
        break;
    case I2C_SUBADDR:
        s->subaddr = value;
        break;
    case I2C_LEN:
        s->len = value;
        break;
    case I2C_FIFO:
        if (s->tx_len < I2C_FIFO_DEPTH) {
            s->tx[s->tx_len++] = value;
        }
        break;
    case I2C_CMD:
        if (value & CMD_START) {
            i2c_do_transfer(s, value & CMD_WRITE);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled write 0x%04x <- 0x%08x\n",
                      TYPE_S5L8930_I2C, (unsigned)offset, (unsigned)value);
    }
}

static const MemoryRegionOps s5l8930_i2c_ops = {
    .read = s5l8930_i2c_read,
    .write = s5l8930_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void s5l8930_i2c_reset(DeviceState *dev)
{
    S5L8930I2CState *s = S5L8930_I2C(dev);

    s->addr = s->ctrl = s->status = s->subaddr = s->len = 0;
    s->tx_len = s->rx_len = s->rx_pos = 0;
    qemu_irq_lower(s->irq);
}

static void s5l8930_i2c_init(Object *obj)
{
    S5L8930I2CState *s = S5L8930_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8930_i2c_ops, s,
                          TYPE_S5L8930_I2C, S5L8930_I2C_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static const VMStateDescription vmstate_s5l8930_i2c = {
    .name = TYPE_S5L8930_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(addr, S5L8930I2CState),
        VMSTATE_UINT32(ctrl, S5L8930I2CState),
        VMSTATE_UINT32(status, S5L8930I2CState),
        VMSTATE_UINT32(subaddr, S5L8930I2CState),
        VMSTATE_UINT32(len, S5L8930I2CState),
        VMSTATE_UINT8_ARRAY(tx, S5L8930I2CState, I2C_FIFO_DEPTH),
        VMSTATE_UINT8_ARRAY(rx, S5L8930I2CState, I2C_FIFO_DEPTH),
        VMSTATE_UINT32(tx_len, S5L8930I2CState),
        VMSTATE_UINT32(rx_len, S5L8930I2CState),
        VMSTATE_UINT32(rx_pos, S5L8930I2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8930_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8930_i2c;
    device_class_set_legacy_reset(dc, s5l8930_i2c_reset);
}

static const TypeInfo s5l8930_i2c_info = {
    .name          = TYPE_S5L8930_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930I2CState),
    .instance_init = s5l8930_i2c_init,
    .class_init    = s5l8930_i2c_class_init,
};

/* ---- Dialog D1815 PMU ---- */

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930D1815State, S5L8930_D1815)

#define D1815_ADDR          0x74
#define PMU_EVENT           0x01    /* A-F, clear-on-read */
#define PMU_EVENT_COUNT     6
#define PMU_EVENT_B_ADC     (1u << 5)   /* IRQ handler c066236a -> ADC done */
#define PMU_STATUS          0x07    /* A-E, power sources; STAT function */
#define PMU_IRQ_MASK        0x0C    /* A-F; start writes FF 5F FF FF FF FF */
#define PMU_OOC             0x12    /* bit0 = shutdown/standby, spin after */
#define PMU_ADC_CTRL        0x30    /* mux | 0x10 start (mux 3 also 0x20) */
#define PMU_ADC_START       (1u << 4)
#define PMU_ADC_RES         0x31    /* 12-bit: (r[0] & 0xF) | r[1] << 4 */
#define PMU_RTC_PRELOAD     0x46    /* 4 bytes, latched into the counter... */
#define PMU_RTC_CTRL        0x4A    /* ...by writing 0x41 here */
#define PMU_RTC_CTRL_LOAD   (1u << 6)
#define PMU_RTC_COUNT       0x4C    /* live seconds counter, LE, read twice */
#define PMU_RTC_OFFSET      0x84    /* scratch: PMURTC adds it to the count */

struct S5L8930D1815State {
    I2CSlave i2c;
    qemu_irq irq;
    QEMUTimer *adc_timer;

    uint8_t regs[256];
    uint8_t reg;            /* current register, auto-incrementing */
    bool addressing;        /* next byte received is the register number */
    int64_t rtc_base;       /* counter = host epoch + rtc_base */
    uint32_t rtc_latch;
};

static void d1815_update_irq(S5L8930D1815State *s)
{
    int i, pending = 0;

    for (i = 0; i < PMU_EVENT_COUNT; i++) {
        pending |= s->regs[PMU_EVENT + i] & ~s->regs[PMU_IRQ_MASK + i];
    }
    qemu_set_irq(s->irq, pending != 0);
}

/* Completion arrives as an event, not inline: the driver arms its own timeout
 * before waiting, and an IRQ from inside the register write would beat it. */
static void d1815_adc_done(void *opaque)
{
    S5L8930D1815State *s = opaque;
    /* ponytail: one mid-scale value for every mux; per-channel table when a
     * client (battery voltage, thermistor, accessory ID) proves to care. */
    uint16_t v = 0x800;

    s->regs[PMU_ADC_RES] = v & 0xf;
    s->regs[PMU_ADC_RES + 1] = v >> 4;
    s->regs[PMU_EVENT + 1] |= PMU_EVENT_B_ADC;
    d1815_update_irq(s);
}

static uint32_t d1815_rtc_count(S5L8930D1815State *s)
{
    return (uint32_t)(time(NULL) + s->rtc_base);
}

static int d1815_event(I2CSlave *i2c, enum i2c_event event)
{
    S5L8930D1815State *s = S5L8930_D1815(i2c);

    if (event == I2C_START_SEND) {
        s->addressing = true;
    }
    return 0;
}

static uint8_t d1815_recv(I2CSlave *i2c)
{
    S5L8930D1815State *s = S5L8930_D1815(i2c);
    uint8_t reg = s->reg++;
    uint8_t v = s->regs[reg];

    switch (reg) {
    case PMU_EVENT ... PMU_EVENT + PMU_EVENT_COUNT - 1:
        s->regs[reg] = 0;
        d1815_update_irq(s);
        break;
    case PMU_RTC_COUNT:
        /* Snapshot on the low byte so the 4 bytes describe one instant. */
        s->rtc_latch = d1815_rtc_count(s);
        v = s->rtc_latch;
        break;
    case PMU_RTC_COUNT + 1 ... PMU_RTC_COUNT + 3:
        v = s->rtc_latch >> (8 * (reg - PMU_RTC_COUNT));
        break;
    }
    return v;
}

static int d1815_send(I2CSlave *i2c, uint8_t data)
{
    S5L8930D1815State *s = S5L8930_D1815(i2c);
    uint8_t reg;

    if (s->addressing) {
        s->addressing = false;
        s->reg = data;
        return 0;
    }
    reg = s->reg++;
    switch (reg) {
    case PMU_EVENT ... PMU_EVENT + PMU_EVENT_COUNT - 1:
    case PMU_RTC_COUNT ... PMU_RTC_COUNT + 3:
        return 0;                       /* read-only */
    case PMU_IRQ_MASK ... PMU_IRQ_MASK + PMU_EVENT_COUNT - 1:
        s->regs[reg] = data;
        d1815_update_irq(s);
        return 0;
    case PMU_OOC:
        s->regs[reg] = data;
        if (data & 1) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        return 0;
    case PMU_ADC_CTRL:
        s->regs[reg] = data;
        if (data & PMU_ADC_START) {
            timer_mod(s->adc_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
        }
        return 0;
    case PMU_RTC_CTRL:
        s->regs[reg] = data;
        if (data & PMU_RTC_CTRL_LOAD) {
            s->rtc_base = (int64_t)ldl_le_p(&s->regs[PMU_RTC_PRELOAD]) -
                          time(NULL);
        }
        return 0;
    default:
        s->regs[reg] = data;
        return 0;
    }
}

static void d1815_reset(DeviceState *dev)
{
    S5L8930D1815State *s = S5L8930_D1815(dev);

    timer_del(s->adc_timer);
    memset(s->regs, 0, sizeof(s->regs));
    /* Everything masked until the driver programs 0x0C-0x11; no events
     * pending; no external power (status 0x07-0x0B = 0); the RTC offset at
     * 0x84 is 0 so the counter alone is the wall clock. */
    memset(&s->regs[PMU_IRQ_MASK], 0xff, PMU_EVENT_COUNT);
    s->reg = 0;
    s->addressing = true;
    s->rtc_base = 0;
    s->rtc_latch = 0;
    d1815_update_irq(s);
}

static void d1815_init(Object *obj)
{
    S5L8930D1815State *s = S5L8930_D1815(obj);

    I2C_SLAVE(obj)->address = D1815_ADDR;
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    s->adc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, d1815_adc_done, s);
}

static void d1815_finalize(Object *obj)
{
    timer_free(S5L8930_D1815(obj)->adc_timer);
}

static int d1815_post_load(void *opaque, int version_id)
{
    d1815_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_s5l8930_d1815 = {
    .name = TYPE_S5L8930_D1815,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = d1815_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, S5L8930D1815State),
        VMSTATE_TIMER_PTR(adc_timer, S5L8930D1815State),
        VMSTATE_UINT8_ARRAY(regs, S5L8930D1815State, 256),
        VMSTATE_UINT8(reg, S5L8930D1815State),
        VMSTATE_BOOL(addressing, S5L8930D1815State),
        VMSTATE_INT64(rtc_base, S5L8930D1815State),
        VMSTATE_UINT32(rtc_latch, S5L8930D1815State),
        VMSTATE_END_OF_LIST()
    }
};

static void d1815_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8930_d1815;
    device_class_set_legacy_reset(dc, d1815_reset);
    k->event = d1815_event;
    k->recv = d1815_recv;
    k->send = d1815_send;
}

static const TypeInfo s5l8930_d1815_info = {
    .name              = TYPE_S5L8930_D1815,
    .parent            = TYPE_I2C_SLAVE,
    .instance_size     = sizeof(S5L8930D1815State),
    .instance_init     = d1815_init,
    .instance_finalize = d1815_finalize,
    .class_init        = d1815_class_init,
};

/* ---- TI TCA6408 8-bit GPIO expander (I2C0 0x20) ----
 *
 * 7B500 AppleTCA6408GPIOIC (kext at c063c000): readReg c063d5c8 / writeReg
 * c063d610; start() reads 0x03 and writes it (configuration), writes 0x01
 * (output) on pin changes, and interruptAction reads 0x00 (input), XORs it
 * against the last value and dispatches per pin. Nothing is validated.
 * Pins (ref-a4-board.md §3): 0 bt_reset, 1 wlan/sdio reset, 2 bluetooth,
 * 3 wlan, 4 baseband, 5 firewire. INT: asserted (level 1 here) from an
 * input change until the input port is read, as the datasheet has it.
 */

#ifndef TYPE_S5L8930_TCA6408   /* until it moves into s5l8930.h */
#define TYPE_S5L8930_TCA6408 "s5l8930.tca6408"
#endif

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930TCA6408State, S5L8930_TCA6408)

#define TCA6408_ADDR    0x20
#define TCA_INPUT       0
#define TCA_OUTPUT      1
#define TCA_POLARITY    2
#define TCA_CONFIG      3   /* 1 = input */

struct S5L8930TCA6408State {
    I2CSlave i2c;
    qemu_irq irq;

    uint8_t regs[4];
    uint8_t pins;           /* external levels driven on the qdev inputs */
    uint8_t reg;
    bool addressing;
    bool pending;
};

/* Input port: external level on input pins, the output latch on outputs,
 * then polarity inversion. */
static uint8_t tca6408_input(S5L8930TCA6408State *s)
{
    uint8_t cfg = s->regs[TCA_CONFIG];

    return ((s->pins & cfg) | (s->regs[TCA_OUTPUT] & ~cfg)) ^
           s->regs[TCA_POLARITY];
}

static void tca6408_set_pin(void *opaque, int n, int level)
{
    S5L8930TCA6408State *s = opaque;
    uint8_t before = tca6408_input(s);

    s->pins = deposit32(s->pins, n, 1, level != 0);
    if (tca6408_input(s) != before) {
        s->pending = true;
        qemu_set_irq(s->irq, 1);
    }
}

static int tca6408_event(I2CSlave *i2c, enum i2c_event event)
{
    S5L8930TCA6408State *s = S5L8930_TCA6408(i2c);

    if (event == I2C_START_SEND) {
        s->addressing = true;
    }
    return 0;
}

static uint8_t tca6408_recv(I2CSlave *i2c)
{
    S5L8930TCA6408State *s = S5L8930_TCA6408(i2c);
    uint8_t reg = s->reg++ & 3;

    if (reg == TCA_INPUT) {
        s->pending = false;
        qemu_set_irq(s->irq, 0);
        return tca6408_input(s);
    }
    return s->regs[reg];
}

static int tca6408_send(I2CSlave *i2c, uint8_t data)
{
    S5L8930TCA6408State *s = S5L8930_TCA6408(i2c);
    uint8_t reg;

    if (s->addressing) {
        s->addressing = false;
        s->reg = data;
        return 0;
    }
    reg = s->reg++ & 3;
    if (reg != TCA_INPUT) {
        s->regs[reg] = data;
    }
    return 0;
}

static void tca6408_reset(DeviceState *dev)
{
    S5L8930TCA6408State *s = S5L8930_TCA6408(dev);

    s->regs[TCA_INPUT] = 0;
    s->regs[TCA_OUTPUT] = 0xff;
    s->regs[TCA_POLARITY] = 0;
    s->regs[TCA_CONFIG] = 0xff;
    s->pins = 0xff;
    s->reg = 0;
    s->addressing = true;
    s->pending = false;
    qemu_set_irq(s->irq, 0);
}

static void tca6408_init(Object *obj)
{
    S5L8930TCA6408State *s = S5L8930_TCA6408(obj);

    I2C_SLAVE(obj)->address = TCA6408_ADDR;
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    qdev_init_gpio_in(DEVICE(obj), tca6408_set_pin, 8);
}

static int tca6408_post_load(void *opaque, int version_id)
{
    S5L8930TCA6408State *s = opaque;

    qemu_set_irq(s->irq, s->pending);
    return 0;
}

static const VMStateDescription vmstate_s5l8930_tca6408 = {
    .name = TYPE_S5L8930_TCA6408,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = tca6408_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, S5L8930TCA6408State),
        VMSTATE_UINT8_ARRAY(regs, S5L8930TCA6408State, 4),
        VMSTATE_UINT8(pins, S5L8930TCA6408State),
        VMSTATE_UINT8(reg, S5L8930TCA6408State),
        VMSTATE_BOOL(addressing, S5L8930TCA6408State),
        VMSTATE_BOOL(pending, S5L8930TCA6408State),
        VMSTATE_END_OF_LIST()
    }
};

static void tca6408_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_s5l8930_tca6408;
    device_class_set_legacy_reset(dc, tca6408_reset);
    k->event = tca6408_event;
    k->recv = tca6408_recv;
    k->send = tca6408_send;
}

static const TypeInfo s5l8930_tca6408_info = {
    .name          = TYPE_S5L8930_TCA6408,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(S5L8930TCA6408State),
    .instance_init = tca6408_init,
    .class_init    = tca6408_class_init,
};

static void s5l8930_i2c_register_types(void)
{
    type_register_static(&s5l8930_i2c_info);
    type_register_static(&s5l8930_d1815_info);
    type_register_static(&s5l8930_tca6408_info);
}

type_init(s5l8930_i2c_register_types)
