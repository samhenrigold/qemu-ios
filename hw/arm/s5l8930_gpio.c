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
#include "hw/qdev-properties.h"
#include "qapi/error.h"

/*
 * Sized per SoC by properties: "ports" config ports of 8 pins (K48 22, the
 * S5L8920's DT #gpio-ports 46) and "int-groups" groups of 32 pins that can
 * interrupt (#interrupt-groups: K48 6, S5L8920 7; pins past those are
 * plain I/O). The arrays hold the largest.
 */
#define GPIO_MAX_PINS       S5L8930_GPIO_MAX_PINS
#define GPIO_GROUPS         DIV_ROUND_UP(GPIO_MAX_PINS, 32)
#define K48_PINS            S5L8930_GPIO_PINS
#define K48_GROUPS          DIV_ROUND_UP(K48_PINS, 32)

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
    qemu_irq out[GPIO_MAX_PINS];
    uint32_t ports, int_groups;
    bool pin_int_enable;

    uint32_t cfg[GPIO_MAX_PINS];
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

    for (unsigned g = 0; g < s->int_groups; g++) {
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

    if (addr < s->ports * 8 * 4) {
        unsigned pin = addr / 4;
        unsigned mode = pin_mode(s, pin);
        bool level = (mode == GPIO_MODE_OUT || mode == GPIO_MODE_OUT_ALT) ?
                     s->cfg[pin] & GPIO_CFG_DATA : pin_input(s, pin);
        return (s->cfg[pin] & ~GPIO_CFG_DATA) | level;
    }
    if (addr == GPIO_INT_SUMMARY) {
        uint32_t summary = 0;
        for (g = 0; g < s->int_groups; g++) {
            summary |= (uint32_t)((s->status[g] & s->enabled[g]) != 0) << g;
        }
        return summary;
    }
    if (s->pin_int_enable && g < s->int_groups && (addr & ~0x3f) == GPIO_INT_DISABLE) {
        /* S5L8920: 0x800+4g is the pending word the handler scans (W1C). */
        return s->status[g] & s->enabled[g];
    }
    if (g < s->int_groups) {
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

    if (addr < s->ports * 8 * 4) {
        unsigned pin = addr / 4, mode;

        if (s->pin_int_enable) {
            /*
             * S5L8920: the pin's own config word masks its interrupt (bit
             * 4); 0x800+4g is the pending word (all-ones cleared at start,
             * scanned by the handler, W1C) and the 0x840/0x880 blocks are
             * unused. AppleS5L8920XGPIOIC as N18 8C148 drives it: PMU 0x9d
             * mode 0x217 then unmasked 0x207; its handler masks a level
             * source (0x217) and clears 0x810 bit 29 before the PMU driver
             * runs. Multi-touch 0xb4 0x21b/0x20b, buttons 0x21d/0x20d.
             */
            uint32_t bit = 1u << (pin % 32);

            mode = value & GPIO_CFG_MODE;
            if (!(value & 0x10) && mode >= GPIO_MODE_LEVEL_HI && mode <= GPIO_MODE_BOTH) {
                s->enabled[pin / 32] |= bit;
            } else {
                s->enabled[pin / 32] &= ~bit;
            }
            s->cfg[pin] = value & 0xffff;
            s5l8930_gpio_latch_level(s, pin);
            s5l8930_gpio_update(s);
            return;
        }
        s->cfg[pin] = value & 0xffff;
        mode = pin_mode(s, pin);
        if (mode == GPIO_MODE_OUT || mode == GPIO_MODE_OUT_ALT) {
            qemu_set_irq(s->out[pin], s->cfg[pin] & GPIO_CFG_DATA);
        }
        s5l8930_gpio_latch_level(s, pin);
        s5l8930_gpio_update(s);
        return;
    }
    if (s->pin_int_enable && g < s->int_groups && (addr & ~0x3f) == GPIO_INT_DISABLE) {
        s->status[g] &= ~value;
        for (unsigned pin = g * 32; pin < MIN(g * 32 + 32, s->ports * 8); pin++) {
            s5l8930_gpio_latch_level(s, pin);
        }
        s5l8930_gpio_update(s);
        return;
    }
    if (g < s->int_groups) {
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
                 pin < MIN(g * 32 + 32, s->ports * 8); pin++) {
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
}

static void s5l8930_gpio_realize(DeviceState *dev, Error **errp)
{
    S5L8930GPIOState *s = S5L8930_GPIO(dev);

    if (s->ports * 8 > GPIO_MAX_PINS || s->int_groups * 32 > GPIO_MAX_PINS ||
        s->ports < 22 || s->int_groups < K48_GROUPS) {
        error_setg(errp, "gpio: %u ports / %u interrupt groups out of range",
                   s->ports, s->int_groups);
        return;
    }
    qdev_init_gpio_in(dev, s5l8930_gpio_set_input, s->ports * 8);
    qdev_init_gpio_out(dev, s->out, s->ports * 8);
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
    static const unsigned low_inputs[] = {
        0x607, /* K48 Wi-Fi: no radio */
        0x502, 0x504, /* board straps: 0x503 high -> board ID 2 */
        0x202, 0x301, 0x304, 0x305, /* board revision 0 */
    };

    for (unsigned pin = 0; pin < s->ports * 8; pin++) {
        s->cfg[pin] = 0x200;
    }
    memset(s->input, 0xff, sizeof(s->input));
    /* SecureROM samples these straps to construct POWER_ID. The board/revision
     * match the real K48's 0x01020001; all-high inputs selected an unsupported
     * board and restarted the ROM before DFU could enumerate. */
    for (unsigned i = 0; i < ARRAY_SIZE(low_inputs); i++) {
        unsigned pin = S5L8930_GPIO_PIN(low_inputs[i]);
        s->input[pin / 32] &= ~(1u << (pin % 32));
    }
    memset(s->enabled, 0, sizeof(s->enabled));
    memset(s->status, 0, sizeof(s->status));
    qemu_irq_lower(s->irq);
}

/* The K48's 176 pins / 6 groups in the main section, as before; the
 * rest only for a larger instance. */
static bool gpio_wide_needed(void *opaque)
{
    S5L8930GPIOState *s = opaque;

    return s->ports * 8 > K48_PINS || s->int_groups > K48_GROUPS;
}

static const VMStateDescription vmstate_s5l8930_gpio_wide = {
    .name = "s5l8930_gpio/wide",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = gpio_wide_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_SUB_ARRAY(cfg, S5L8930GPIOState, K48_PINS, GPIO_MAX_PINS - K48_PINS),
        VMSTATE_UINT32_SUB_ARRAY(input, S5L8930GPIOState, K48_GROUPS, GPIO_GROUPS - K48_GROUPS),
        VMSTATE_UINT32_SUB_ARRAY(enabled, S5L8930GPIOState, K48_GROUPS, GPIO_GROUPS - K48_GROUPS),
        VMSTATE_UINT32_SUB_ARRAY(status, S5L8930GPIOState, K48_GROUPS, GPIO_GROUPS - K48_GROUPS),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_s5l8930_gpio = {
    .name = "s5l8930_gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_SUB_ARRAY(cfg, S5L8930GPIOState, 0, K48_PINS),
        VMSTATE_UINT32_SUB_ARRAY(input, S5L8930GPIOState, 0, K48_GROUPS),
        VMSTATE_UINT32_SUB_ARRAY(enabled, S5L8930GPIOState, 0, K48_GROUPS),
        VMSTATE_UINT32_SUB_ARRAY(status, S5L8930GPIOState, 0, K48_GROUPS),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_s5l8930_gpio_wide,
        NULL
    }
};

static const Property s5l8930_gpio_props[] = {
    DEFINE_PROP_UINT32("ports", S5L8930GPIOState, ports, 22),
    DEFINE_PROP_UINT32("int-groups", S5L8930GPIOState, int_groups, K48_GROUPS),
    DEFINE_PROP_BOOL("pin-int-enable", S5L8930GPIOState, pin_int_enable, false),
};

static void s5l8930_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, s5l8930_gpio_reset);
    dc->realize = s5l8930_gpio_realize;
    dc->vmsd = &vmstate_s5l8930_gpio;
    device_class_set_props(dc, s5l8930_gpio_props);
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
