/*
 * iPad 1 (K48AP): Apple S5L8930 "A4", Cortex-A8, 256 MiB.
 *
 * Milestone 1 of docs/ipad1/PLAN.md: enter the iOS 3.2.2 (7B500) kernel
 * directly, with no bootrom or iBoot, and get its console on UART0. Only the
 * devices the kernel's platform expert needs are modelled. Every other
 * peripheral address falls into an unimplemented-device window that logs the
 * access (-d unimp) instead of faulting, so bring-up can see what the kernel
 * reaches for next.
 *
 * Boot input is a K48KBOOT bundle from imgtools/ipad1_kboot.py: a flat image of
 * physical memory (kernel, filled device tree, boot_args) followed by a 24-byte
 * trailer {char magic[8]; u32 load_pa, entry_pa, bootargs_pa, image_len}. We
 * copy it into DRAM on every reset and start the CPU at entry_pa in ARM state
 * with the MMU off and r0 = bootargs_pa, which is the state the kernel's
 * _start expects from iBoot.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/misc/unimp.h"
#include "hw/sysbus.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_buttons.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/s5l8930.h"
#include "hw/intc/pl192.h"
#include "hw/qdev-properties.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "ui/console.h"
#include "ui/input.h"

#define TYPE_IPAD1_MACHINE MACHINE_TYPE_NAME("ipad1")
OBJECT_DECLARE_SIMPLE_TYPE(IPad1MachineState, IPAD1_MACHINE)

struct IPad1MachineState {
    MachineState parent;
    ARMCPU *cpu;
    MemoryRegion dram;
    MemoryRegion sram;
    MemoryRegion cpu_debug;
    DeviceState *vic[S5L8930_VIC_COUNT];
    DeviceState *gpio;
    IPodTouchMultitouchState *mt;
    char *kboot_path;
    char *nand_path;
    char *nor_path;
    bool kbd_cmd, kbd_shift;
    int kbd_btn_held[Q_KEY_CODE__MAX];   /* qcode -> 1 + button pin */
};

/* GHWCFG1-4 of the DWC OTG core; same synthesis as the S5L8720's. */
static uint32_t s5l8930_usb_hwcfg[] = { 0, 0x7a8f60d0, 0x082000e8, 0x01f08024 };

#define KBOOT_MAGIC "K48KBOOT"
#define KBOOT_TRAILER_LEN 24

static qemu_irq ipad1_irq(IPad1MachineState *s, int irq)
{
    return qdev_get_gpio_in(s->vic[irq / 32], irq % 32);
}

/*
 * Stage the K48KBOOT bundle and point the CPU at it. Done on every reset, like
 * the iPod machine's bootrom staging: iOS writes over the memory the bundle
 * occupies, so a reset that did not re-stage it would resume into junk.
 */
static void ipad1_cpu_reset(void *opaque)
{
    IPad1MachineState *s = IPAD1_MACHINE(opaque);
    CPUState *cs = CPU(s->cpu);
    g_autofree char *data = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize size;
    const uint8_t *trailer;
    uint32_t load_pa, entry_pa, bootargs_pa, image_len;

    cpu_reset(cs);

    if (!g_file_get_contents(s->kboot_path, &data, &size, &gerr)) {
        error_report("ipad1: cannot read kboot bundle '%s': %s",
                     s->kboot_path, gerr->message);
        exit(1);
    }
    if (size < KBOOT_TRAILER_LEN ||
        memcmp(data + size - KBOOT_TRAILER_LEN, KBOOT_MAGIC, 8) != 0) {
        error_report("ipad1: '%s' is not a K48KBOOT bundle", s->kboot_path);
        exit(1);
    }
    trailer = (const uint8_t *)data + size - KBOOT_TRAILER_LEN;
    load_pa = ldl_le_p(trailer + 8);
    entry_pa = ldl_le_p(trailer + 12);
    bootargs_pa = ldl_le_p(trailer + 16);
    image_len = ldl_le_p(trailer + 20);

    if (image_len > size - KBOOT_TRAILER_LEN ||
        load_pa < S5L8930_DRAM_BASE ||
        (uint64_t)load_pa + image_len > S5L8930_DRAM_BASE + S5L8930_DRAM_SIZE) {
        error_report("ipad1: kboot bundle does not fit in DRAM "
                     "(load 0x%x len 0x%x)", load_pa, image_len);
        exit(1);
    }
    if (address_space_write(&address_space_memory, load_pa,
                            MEMTXATTRS_UNSPECIFIED, data, image_len) != MEMTX_OK) {
        error_report("ipad1: cannot stage kboot bundle at 0x%x", load_pa);
        exit(1);
    }

    /* cpu_reset leaves us in SVC mode, IRQ/FIQ masked, MMU and caches off. */
    s->cpu->env.regs[0] = bootargs_pa;
    cpu_set_pc(cs, entry_pa);
}

/*
 * Host mouse -> digitizer slot 0. QEMU's absolute coordinates are 0..0x7fff;
 * the digitizer wants 0..1 with y from the bottom (see set_finger()).
 * ponytail: same axis mapping as the iPod's portrait panel. If touches land
 * rotated once SpringBoard is up, this is the one place to swap/flip them.
 */
static void ipad1_mouse_event(void *opaque, int x, int y, int z, int buttons)
{
    IPodTouchMultitouchState *mt = opaque;

    mt->touch_x = x / 32768.0f;
    mt->touch_y = 1.0f - y / 32768.0f;
    if (buttons && !mt->touch_down) {
        ipod_touch_multitouch_on_touch(mt);
    } else if (!buttons && mt->touch_down) {
        ipod_touch_multitouch_on_release(mt);
    } else if (buttons) {
        ipod_touch_multitouch_on_motion(mt);
    }
}

/*
 * Buttons are GPIO port 0 pins 0-4, active low, idle high in the GPIO model.
 * Same host chords as the iPod machine: Cmd+L hold/power, Cmd+Shift+H
 * home/menu, Cmd+- volume down, Cmd+= volume up. A key that pressed a
 * button always releases it, even if Cmd went up first.
 */
static void ipad1_kbd_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    IPad1MachineState *s = IPAD1_MACHINE(qdev_get_machine());
    int q = qemu_input_key_value_to_qcode(evt->u.key.data->key);
    bool down = evt->u.key.data->down;
    int pin = -1;

    switch (q) {
    case Q_KEY_CODE_META_L:
    case Q_KEY_CODE_META_R:
        s->kbd_cmd = down;
        return;
    case Q_KEY_CODE_SHIFT:
    case Q_KEY_CODE_SHIFT_R:
        s->kbd_shift = down;
        return;
    default:
        break;
    }
    if (q < 0 || q >= Q_KEY_CODE__MAX) {
        return;
    }
    if (!down && s->kbd_btn_held[q]) {
        pin = s->kbd_btn_held[q] - 1;
    } else if (down && s->kbd_cmd) {
        switch (q) {
        case Q_KEY_CODE_L:     pin = S5L8930_GPIO_BTN_HOLD; break;
        case Q_KEY_CODE_H:     if (s->kbd_shift) pin = S5L8930_GPIO_BTN_MENU; break;
        case Q_KEY_CODE_MINUS: pin = S5L8930_GPIO_BTN_VOLDOWN; break;
        case Q_KEY_CODE_EQUAL: pin = S5L8930_GPIO_BTN_VOLUP; break;
        default: break;
        }
    }
    if (pin < 0) {
        return;
    }
    s->kbd_btn_held[q] = down ? pin + 1 : 0;
    qemu_set_irq(qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(pin)), !down);
}

/* The app bridge's buttons (contrib/ios-app), on the same pins as the chords. */
void ipad1_press_button(IPodTouchButton button, bool down)
{
    static const int pins[] = {
        [IPOD_TOUCH_BUTTON_HOME] = S5L8930_GPIO_BTN_MENU,
        [IPOD_TOUCH_BUTTON_POWER] = S5L8930_GPIO_BTN_HOLD,
        [IPOD_TOUCH_BUTTON_VOLUP] = S5L8930_GPIO_BTN_VOLUP,
        [IPOD_TOUCH_BUTTON_VOLDOWN] = S5L8930_GPIO_BTN_VOLDOWN,
    };
    IPad1MachineState *s = (IPad1MachineState *)
        object_dynamic_cast(OBJECT(qdev_get_machine()), TYPE_IPAD1_MACHINE);

    if (!s || (unsigned)button >= ARRAY_SIZE(pins)) {
        return;
    }
    qemu_set_irq(qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(pins[button])), !down);
}

static const QemuInputHandler ipad1_kbd_handler = {
    .name  = "iPad Buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = ipad1_kbd_event,
};

static void ipad1_init(MachineState *machine)
{
    IPad1MachineState *s = IPAD1_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    DeviceState *dev;
    SysBusDevice *sbd;
    int i;

    if (!s->kboot_path) {
        error_report("ipad1: the 'kboot' machine property is required "
                     "(-machine ipad1,kboot=/path/to/k48-kboot.bin)");
        exit(1);
    }

    cpuobj = object_new(machine->cpu_type);
    s->cpu = ARM_CPU(cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(sysmem), &error_abort);
    object_property_set_bool(cpuobj, "has_el3", false, NULL);
    object_property_set_bool(cpuobj, "has_el2", false, NULL);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    object_unref(cpuobj);

    memory_region_init_ram(&s->dram, NULL, "ipad1.dram", S5L8930_DRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_DRAM_BASE, &s->dram);
    memory_region_init_ram(&s->sram, NULL, "ipad1.sram", S5L8930_SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_SRAM_BASE, &s->sram);

    /*
     * Everything in the peripheral window that has no model yet. Real devices
     * added below sit on top of this at higher priority.
     */
    create_unimplemented_device("s5l8930.periph", 0x80000000, 0x40000000);

    /* The kernel maps the cpu-debug-interface but never writes it at boot. */
    memory_region_init_ram(&s->cpu_debug, NULL, "ipad1.cpu-debug", 0x1000,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_CPU_DEBUG_BASE, &s->cpu_debug);

    /*
     * Four PL192s, daisy-chained VIC3 -> VIC2 -> VIC1 -> VIC0 -> CPU. The kernel
     * reads VIC0's VECTADDR and expects the chained vectors to come through it.
     */
    s->vic[0] = pl192_manual_init((char *)"vic0",
                                  qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ),
                                  qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ),
                                  NULL);
    memory_region_add_subregion(sysmem, S5L8930_VIC_BASE(0),
                                &PL192(s->vic[0])->iomem);
    for (i = 1; i < S5L8930_VIC_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("vic%d", i);
        s->vic[i] = pl192_manual_init(name, NULL);
        memory_region_add_subregion(sysmem, S5L8930_VIC_BASE(i),
                                    &PL192(s->vic[i])->iomem);
        PL192(s->vic[i])->daisy = PL192(s->vic[i - 1]);
    }

    dev = qdev_new(TYPE_S5L8930_PMGR);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_PMGR_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_TIMER0));
    sysbus_connect_irq(sbd, 1, ipad1_irq(s, S5L8930_IRQ_TIMER1));

    dev = qdev_new(TYPE_S5L8930_GPIO);
    s->gpio = dev;
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_GPIO_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_GPIO));

    /* I2C0 carries the D1815 PMU; its interrupt is GPIO pin 0x0D, active low. */
    dev = qdev_new(TYPE_S5L8930_I2C);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_I2C_BASE(0));
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_I2C(0)));
    {
        I2CBus *bus = I2C_BUS(qdev_get_child_bus(dev, "i2c"));
        DeviceState *pmu = DEVICE(i2c_slave_create_simple(bus, TYPE_S5L8930_D1815, 0x74));
        DeviceState *xp = DEVICE(i2c_slave_create_simple(bus, TYPE_S5L8930_TCA6408, 0x20));
        qdev_connect_gpio_out(pmu, 0,
                              qemu_irq_invert(qdev_get_gpio_in(s->gpio, 0x0d)));
        qdev_connect_gpio_out(xp, 0,
                              qemu_irq_invert(qdev_get_gpio_in(s->gpio, 0x11)));
    }
    /* I2C2 (accelerometer, light sensor) is empty for now: transfers NACK. */
    dev = qdev_new(TYPE_S5L8930_I2C);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_I2C_BASE(2));
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_I2C(2)));

    /* Display pipe, CLCD, DART2, RGBOUT, TV-out; scanout starts at iBoot's FB. */
    dev = qdev_new(TYPE_S5L8930_DISPLAY);
    qdev_prop_set_uint64(dev, "fb-base", 0x4f700000);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_DISP_PIPE0_BASE);
    sysbus_mmio_map(sbd, 1, S5L8930_CLCD_BASE);
    sysbus_mmio_map(sbd, 2, S5L8930_DART2_BASE);
    sysbus_mmio_map(sbd, 3, S5L8930_RGBOUT_BASE);
    sysbus_mmio_map(sbd, 4, S5L8930_TVOUT_BASE);
    sysbus_mmio_map(sbd, 5, S5L8930_RGBOUT2_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_DISP_PIPE0));
    sysbus_connect_irq(sbd, 1, ipad1_irq(s, S5L8930_IRQ_CLCD));

    /* MIPI-DSIM: the same Samsung IP as the iPod's; reuse that model. */
    dev = qdev_new(TYPE_IPOD_TOUCH_MIPI_DSI);
    IPOD_TOUCH_MIPI_DSI(dev)->direct_boot = true;
    qdev_prop_set_uint32(dev, "lanes", 4);      /* K48 DT #lanes */
    memory_region_add_subregion(sysmem, S5L8930_DSIM_BASE,
                                &IPOD_TOUCH_MIPI_DSI(dev)->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    /*
     * IOP: high-level emulation of the second core. No IRQ line: like the real
     * firmware, the model raises the AP by writing VIC0's SOFTINT register.
     */
    dev = qdev_new(TYPE_S5L8930_IOP);
    if (s->nand_path) {
        qdev_prop_set_string(dev, "nand", s->nand_path);
    }
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_IOP_BASE);
    sysbus_mmio_map(sbd, 1, S5L8930_IOP_VIC_BASE);

    /* CDMA + AES filter; one interrupt line per channel. */
    dev = qdev_new(TYPE_S5L8930_CDMA);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_CDMA_BASE);
    sysbus_mmio_map(sbd, 1, S5L8930_AES_BASE);
    for (i = 0; i < S5L8930_CDMA_CHANNELS; i++) {
        sysbus_connect_irq(sbd, i, ipad1_irq(s, S5L8930_IRQ_CDMA(i)));
    }
    /*
     * USB device mode: the same Synopsys DWC OTG core and PHY register layout
     * as the S5L8720 (gap-kernel-platform-mmio.md §6), so both iPod models are
     * reused unchanged. The host bridge dials IT_USB_TCP=host:port when set.
     * The USB arbitrator's USB_CTL block (0xbf108000) is mapped but never
     * touched on K48 (no hsic-enabled), so it stays in the unimp window.
     */
    dev = qdev_new(TYPE_IPOD_TOUCH_USB_PHYS);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_USB_PHY_BASE);

    dev = ipod_touch_init_usb_otg(ipad1_irq(s, S5L8930_IRQ_USB_OTG),
                                  s5l8930_usb_hwcfg);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_USB_OTG_BASE,
                                &S5L8900USBOTG(dev)->iomem);
    /*
     * SPI: the K48 kernel drives these with the same AppleS5L8900X SPI kext
     * as the iPod, so the iPod controller model is reused. It picks its
     * peripheral from the global set_spi_base() index: 0 = NOR, 4 =
     * multitouch. So the iPad's SPI1 is created as "spi4"; only the bus
     * name and the peripheral choice come from that number.
     */
    set_spi_base(0);
    dev = sysbus_create_simple(TYPE_IPOD_TOUCH_SPI, S5L8930_SPI_BASE(0),
                               ipad1_irq(s, S5L8930_IRQ_SPI(0)));
    IPOD_TOUCH_SPI(dev)->nor->nor_path = s->nor_path;
    /* NOR chip select is GPIO 0x505 (function-spi_cs0), driven by the kernel. */
    qdev_connect_gpio_out(s->gpio, S5L8930_GPIO_PIN(S5L8930_GPIO_NOR_CS),
        qdev_get_gpio_in_named(DEVICE(IPOD_TOUCH_SPI(dev)->nor), SSI_GPIO_CS, 0));

    set_spi_base(4);
    dev = sysbus_create_simple(TYPE_IPOD_TOUCH_SPI, S5L8930_SPI_BASE(1),
                               ipad1_irq(s, S5L8930_IRQ_SPI(1)));
    s->mt = IPOD_TOUCH_SPI(dev)->mt;
    /* Zephyr2 ATN -> GPIO 0x15; reset (0x204) and download (0x107) are ignored. */
    qdev_connect_gpio_out_named(DEVICE(s->mt), "atn", 0,
        qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(S5L8930_GPIO_MT_ATN)));
    qemu_add_mouse_event_handler(ipad1_mouse_event, s->mt, 1, "iPad Touchscreen");
    qemu_input_handler_register(s->gpio, &ipad1_kbd_handler);

    /* SWI: backlight and DPSM core voltage; only the busy bit matters. */
    sysbus_create_simple("ipodtouch.swi", S5L8930_SWI_BASE, NULL);

    /* Same Samsung UART as the S5L8720, including its interrupt scheme. */
    exynos4210_uart_create(S5L8930_UART_BASE(0), 256, 0, serial_hd(0),
                           ipad1_irq(s, S5L8930_IRQ_UART(0)), true);

    qemu_register_reset(ipad1_cpu_reset, s);
}

static char *ipad1_get_kboot(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->kboot_path);
}

static void ipad1_set_kboot(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->kboot_path);
    s->kboot_path = g_strdup(value);
}

static char *ipad1_get_nand(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->nand_path);
}

static void ipad1_set_nand(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->nand_path);
    s->nand_path = g_strdup(value);
}

static char *ipad1_get_nor(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->nor_path);
}

static void ipad1_set_nor(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->nor_path);
    s->nor_path = g_strdup(value);
}

static void ipad1_instance_finalize(Object *obj)
{
    g_free(IPAD1_MACHINE(obj)->kboot_path);
    g_free(IPAD1_MACHINE(obj)->nand_path);
    g_free(IPAD1_MACHINE(obj)->nor_path);
}

static void ipad1_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "iPad 1 (K48AP, S5L8930)";
    mc->init = ipad1_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a8");
    mc->default_ram_size = S5L8930_DRAM_SIZE;

    object_class_property_add_str(klass, "kboot", ipad1_get_kboot,
                                  ipad1_set_kboot);
    object_class_property_set_description(klass, "kboot",
        "K48KBOOT bundle from imgtools/ipad1_kboot.py (required)");
    object_class_property_add_str(klass, "nand", ipad1_get_nand, ipad1_set_nand);
    object_class_property_set_description(klass, "nand",
        "NAND page-store directory (geometry.json + bus<b>-ce<c>.pages); blank chips if unset");
    object_class_property_add_str(klass, "nor", ipad1_get_nor, ipad1_set_nor);
    object_class_property_set_description(klass, "nor",
        "1 MiB SPI NOR image (nvram, syscfg); erased flash if unset");
}

static const TypeInfo ipad1_machine_info = {
    .name = TYPE_IPAD1_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(IPad1MachineState),
    .instance_finalize = ipad1_instance_finalize,
    .class_init = ipad1_class_init,
};

static void ipad1_machine_types(void)
{
    type_register_static(&ipad1_machine_info);
}

type_init(ipad1_machine_types)
