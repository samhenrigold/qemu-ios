/*
 * S5L8930 GPIO interrupt controller (iOS AppleS5L8930XGPIOIC, 0xBFA00000).
 *
 * One 32-bit config register per pin at 4*pin (pin = port*8 + bit, 176
 * pins); bit 0 is the pin level, bits 1-3 the mode. The interrupt block
 * works in groups of 32 pins: 0x800+4g masks (write 1 = disable), 0x840+4g
 * unmasks (write 1 = enable), 0x880+4g is write-1-to-clear status, and
 * 0xC00 has bit g set while group g has an enabled interrupt pending.
 * All of it ORs into the single IRQ 0x74.
 *
 * Decoded from the 7B500 kernel (AppleS5L8930X kext, text at 0xc0643000):
 * disableVectorHard (vtable neighbour of handleInterrupt) writes 0x800,
 * enableVector writes 0x840, clearing a level pin's status first; start
 * writes all-ones to 0x800 and 0x880. openiBoot's gpio.h agrees on
 * 0x800 = disable / 0x840 = enable.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/arm/s5l8930.h"
#include "hw/irq.h"
#include "migration/vmstate.h"

#define GPIO_GROUPS         DIV_ROUND_UP(S5L8930_GPIO_PINS, 32)

#define GPIO_CFG_DATA       0x0001
#define GPIO_CFG_MODE       0x000e    /* bits 1-3 */
#define GPIO_MODE_OUT       0x2       /* 0xe is output too (setPinMode) */
#define GPIO_MODE_OUT_ALT   0xe
#define GPIO_MODE_LEVEL_HI  0x4
#define GPIO_MODE_LEVEL_LO  0x6
#define GPIO_MODE_RISING    0x8
#define GPIO_MODE_FALLING   0xa
#define GPIO_MODE_BOTH      0xc

#define GPIO_INT_DISABLE    0x800
#define GPIO_INT_ENABLE     0x840
#define GPIO_INT_STATUS     0x880
#define GPIO_INT_SUMMARY    0xc00

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930GPIOState, S5L8930_GPIO)

struct S5L8930GPIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq out[S5L8930_GPIO_PINS];

    uint32_t cfg[S5L8930_GPIO_PINS];
    uint32_t input[GPIO_GROUPS];      /* external pin levels, bit per pin */
    uint32_t enabled[GPIO_GROUPS];
    uint32_t status[GPIO_GROUPS];
};

static bool pin_input(S5L8930GPIOState *s, unsigned pin)
{
    return (s->input[pin / 32] >> (pin % 32)) & 1;
}

static unsigned pin_mode(S5L8930GPIOState *s, unsigned pin)
{
    return s->cfg[pin] & GPIO_CFG_MODE;
}

/* Level interrupts stay pending for as long as the level holds. */
static void s5l8930_gpio_latch_level(S5L8930GPIOState *s, unsigned pin)
{
    unsigned mode = pin_mode(s, pin);

    if ((mode == GPIO_MODE_LEVEL_HI && pin_input(s, pin)) ||
        (mode == GPIO_MODE_LEVEL_LO && !pin_input(s, pin))) {
        s->status[pin / 32] |= 1u << (pin % 32);
    }
}

static void s5l8930_gpio_update(S5L8930GPIOState *s)
{
    bool pending = false;

    for (unsigned g = 0; g < GPIO_GROUPS; g++) {
        pending |= (s->status[g] & s->enabled[g]) != 0;
    }
    qemu_set_irq(s->irq, pending);
}

static void s5l8930_gpio_set_input(void *opaque, int pin, int level)
{
    S5L8930GPIOState *s = opaque;
    uint32_t bit = 1u << (pin % 32);
    bool old = pin_input(s, pin);
    unsigned mode = pin_mode(s, pin);

    level = !!level;
    if (level) {
        s->input[pin / 32] |= bit;
    } else {
        s->input[pin / 32] &= ~bit;
    }
    if (level != old &&
        ((mode == GPIO_MODE_RISING && level) ||
         (mode == GPIO_MODE_FALLING && !level) ||
         mode == GPIO_MODE_BOTH)) {
        s->status[pin / 32] |= bit;
    }
    s5l8930_gpio_latch_level(s, pin);
    s5l8930_gpio_update(s);
}

static uint64_t s5l8930_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8930GPIOState *s = opaque;
    unsigned g = (addr & 0x3f) / 4;

    if (addr < S5L8930_GPIO_PINS * 4) {
        unsigned pin = addr / 4;
        unsigned mode = pin_mode(s, pin);
        bool level = (mode == GPIO_MODE_OUT || mode == GPIO_MODE_OUT_ALT) ?
                     s->cfg[pin] & GPIO_CFG_DATA : pin_input(s, pin);
        return (s->cfg[pin] & ~GPIO_CFG_DATA) | level;
    }
    if (addr == GPIO_INT_SUMMARY) {
        uint32_t summary = 0;
        for (g = 0; g < GPIO_GROUPS; g++) {
            summary |= (uint32_t)((s->status[g] & s->enabled[g]) != 0) << g;
        }
        return summary;
    }
    if (g < GPIO_GROUPS) {
        switch (addr & ~0x3f) {
        /*
         * Only sleep reads the mask: it saves [0x800+4g], and wake writes the
         * saved word to 0x800 and its complement to 0x840. That round-trips
         * only if 0x800 reads back the disabled bits.
         */
        case GPIO_INT_DISABLE:
            return ~s->enabled[g];
        case GPIO_INT_ENABLE:
            return s->enabled[g];
        /*
         * Masked so the handler (which picks the highest status bit without
         * checking the mask) never spins on a disabled level pin that
         * re-latches as fast as it is cleared.
         */
        case GPIO_INT_STATUS:
            return s->status[g] & s->enabled[g];
        }
    }
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

static void s5l8930_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    S5L8930GPIOState *s = opaque;
    unsigned g = (addr & 0x3f) / 4;

    if (addr < S5L8930_GPIO_PINS * 4) {
        unsigned pin = addr / 4, mode;

        s->cfg[pin] = value & 0xffff;
        mode = pin_mode(s, pin);
        if (mode == GPIO_MODE_OUT || mode == GPIO_MODE_OUT_ALT) {
            qemu_set_irq(s->out[pin], s->cfg[pin] & GPIO_CFG_DATA);
        }
        s5l8930_gpio_latch_level(s, pin);
        s5l8930_gpio_update(s);
        return;
    }
    if (g < GPIO_GROUPS) {
        switch (addr & ~0x3f) {
        case GPIO_INT_DISABLE:
            s->enabled[g] &= ~value;
            s5l8930_gpio_update(s);
            return;
        case GPIO_INT_ENABLE:
            s->enabled[g] |= value;
            s5l8930_gpio_update(s);
            return;
        case GPIO_INT_STATUS:
            s->status[g] &= ~value;
            for (unsigned pin = g * 32;
                 pin < MIN(g * 32 + 32, S5L8930_GPIO_PINS); pin++) {
                s5l8930_gpio_latch_level(s, pin);
            }
            s5l8930_gpio_update(s);
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx
                  " value 0x%" PRIx64 "\n", __func__, addr, value);
}

static const MemoryRegionOps s5l8930_gpio_ops = {
    .read = s5l8930_gpio_read,
    .write = s5l8930_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void s5l8930_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    S5L8930GPIOState *s = S5L8930_GPIO(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8930_gpio_ops, s, "gpio",
                          S5L8930_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in(DEVICE(obj), s5l8930_gpio_set_input, S5L8930_GPIO_PINS);
    qdev_init_gpio_out(DEVICE(obj), s->out, S5L8930_GPIO_PINS);
}

/*
 * Interrupt inputs idle high. The DT's interrupt users all want that: buttons
 * (pins 0-4, polarity 0 = active low, both edges), the PMU (0x0d) and
 * TCA6408 (0x11) as level-low, multitouch (0x15) as falling edge. Configs
 * reset to plain input (setPinMode mode 0 = 0x200) with no interrupt mode;
 * iBoot/the kernel program the rest.
 */
static void s5l8930_gpio_reset(DeviceState *dev)
{
    S5L8930GPIOState *s = S5L8930_GPIO(dev);
    unsigned radio = S5L8930_GPIO_PIN(0x607);

    for (unsigned pin = 0; pin < S5L8930_GPIO_PINS; pin++) {
        s->cfg[pin] = 0x200;
    }
    memset(s->input, 0xff, sizeof(s->input));
    /* K48 Wi-Fi: radio-presence input GPIO 0x607 is low. iBoot probes
     * this before pinging the radio and marking its DT node AAPL,ignore. */
    s->input[radio / 32] &= ~(1u << (radio % 32));
    memset(s->enabled, 0, sizeof(s->enabled));
    memset(s->status, 0, sizeof(s->status));
    qemu_irq_lower(s->irq);
}

static const VMStateDescription vmstate_s5l8930_gpio = {
    .name = "s5l8930_gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(cfg, S5L8930GPIOState, S5L8930_GPIO_PINS),
        VMSTATE_UINT32_ARRAY(input, S5L8930GPIOState, GPIO_GROUPS),
        VMSTATE_UINT32_ARRAY(enabled, S5L8930GPIOState, GPIO_GROUPS),
        VMSTATE_UINT32_ARRAY(status, S5L8930GPIOState, GPIO_GROUPS),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8930_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8930_gpio_reset);
    dc->vmsd = &vmstate_s5l8930_gpio;
}

static const TypeInfo s5l8930_gpio_info = {
    .name          = TYPE_S5L8930_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930GPIOState),
    .instance_init = s5l8930_gpio_init,
    .class_init    = s5l8930_gpio_class_init,
};

static void s5l8930_gpio_register_types(void)
{
    type_register_static(&s5l8930_gpio_info);
}

type_init(s5l8930_gpio_register_types)
