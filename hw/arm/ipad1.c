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
#include "hw/usb/hcd-ehci.h"
#include "hw/usb/hcd-ohci.h"
#include "hw/sysbus.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_buttons.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/arm/ipod_touch_amc.h"
#include "hw/arm/ipod_touch_lis302dl.h"
#include "hw/arm/ipod_touch_cs42l58.h"
#include "hw/arm/ipod_touch_cd3272_mikey.h"
#include "chardev/char.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/s5l8930.h"
#include "hw/intc/pl192.h"
#include "hw/qdev-properties.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "hw/arm/guest-services/general.h"
#include "hw/arm/guest-services/gles.h"
#include "hw/arm/guest-pasteboard.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qapi/visitor.h"

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
    DeviceState *pmu;
    DeviceState *ltc;                    /* charger: USB cable level */
    synopsys_usb_state *usb_otg;
    IPodTouchMultitouchState *mt;
    char *kboot_path;
    char *nand_path;
    char *nand_overlay_path;
    char *nor_path;
    char *usb_tcp_addr;                  /* host bridge, empty = no link */
    bool usb_cable;                      /* cable present; runtime qom-set */
    bool wifi;                           /* BCM4329 behind the IOP's SDIO ring */
    bool kbd_cmd, kbd_shift;
    int kbd_btn_held[Q_KEY_CODE__MAX];   /* qcode -> 1 + button pin */
    int mtt_x[MT_MAX_FINGERS], mtt_y[MT_MAX_FINGERS];  /* latched per slot */
    bool mtt_seen[MT_MAX_FINGERS];
    GuestPasteboard pb;                  /* hw/arm/guest-pasteboard.c */
    /* App controls, same property names as the iPod machine. */
    LIS302DLState *accel;
    double accel_pitch, accel_roll;      /* degrees, as the app sends them */
    bool accel_flat;
    double battery_level;                /* % */
    int battery_mode;                    /* 0 auto (follow cable), 1 on, 2 off */
    double battery_drain;                /* accepted for the bridge; unused */
};

static const char *const ipad1_battery_modes[] = { "auto", "on", "off" };

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
/*
 * The guest-services trap (mcr p15,3,Rn,c15,c15,0) the GLES shim uses, as on
 * the iPod machine. GLES and the pasteboard (it_pbd) only; the iPod's
 * keyboard and agent services are replaced by stock USB services and hardware
 * models here (docs/ipad1/guest-services.md).
 */
static void ipad1_qemu_call(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    CPUState *cs = env_cpu(env);
    IPad1MachineState *s = IPAD1_MACHINE(qdev_get_machine());
    qemu_call_t q;
    int32_t err = 0;

    if (cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 0)) {
        return;
    }
    switch (q.call_number) {
    case QC_GLES:
        q.retval = qc_handle_gles(cs, &q.args.gles);
        break;
    case QC_GLES_PING:
        q.retval = QC_GLES_PING_MAGIC;
        break;
    default:
        if (!guest_pb_call(&s->pb, cs, &q, &err)) {
            return;
        }
    }
    q.error = err;
    cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 1);
}

static const ARMCPRegInfo ipad1_cp_reginfo[] = {
    { .name = "QEMU_CALL", .cp = 15, .opc1 = 3, .crn = 15, .crm = 15,
      .opc2 = 0, .access = PL0_RW, .state = ARM_CP_STATE_AA32,
      .type = ARM_CP_IO | ARM_CP_NO_RAW, .readfn = qemu_call_status,
      .writefn = ipad1_qemu_call },
};

static void ipad1_cpu_reset(void *opaque)
{
    IPad1MachineState *s = IPAD1_MACHINE(opaque);
    CPUState *cs = CPU(s->cpu);
    g_autofree char *data = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize size;
    const uint8_t *trailer;
    uint32_t load_pa, entry_pa, bootargs_pa, image_len;

    gles_host_reset();
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
 * The panel scans out landscape (1024x768) with the portrait UI rotated; the
 * digitizer is portrait-native. Found by trying all eight axis maps against
 * slide-to-unlock: digitizer x = 1 - panel y, y-from-bottom = 1 - panel x.
 */
static void ipad1_map_touch(int x, int y, float *fx, float *fy)
{
    *fx = 1.0f - y / 32768.0f;
    *fy = 1.0f - x / 32768.0f;
}

static void ipad1_mouse_event(void *opaque, int x, int y, int z, int buttons)
{
    IPodTouchMultitouchState *mt = opaque;

    ipad1_map_touch(x, y, &mt->touch_x, &mt->touch_y);
    if (buttons && !mt->touch_down) {
        ipod_touch_multitouch_on_touch(mt);
    } else if (!buttons && mt->touch_down) {
        ipod_touch_multitouch_on_release(mt);
    } else if (buttons) {
        ipod_touch_multitouch_on_motion(mt);
    }
}

/*
 * Multi-touch from the host: QEMU's "mtt" events (QMP input-send-event), same
 * two-phase protocol as the iPod panel (ipod_touch_lcd_mtt_event): DATA
 * latches a slot's abs X/Y in panel coordinates, BEGIN/UPDATE/END commit it.
 */
static void ipad1_mtt_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    IPad1MachineState *s = IPAD1_MACHINE(qdev_get_machine());
    InputMultiTouchEvent *mtt = evt->u.mtt.data;
    int slot = mtt->slot;
    float fx, fy;

    if (slot < 0 || slot >= MT_MAX_FINGERS) {
        return;
    }
    switch (mtt->type) {
    case INPUT_MULTI_TOUCH_TYPE_DATA:
        if (mtt->axis == INPUT_AXIS_X) {
            s->mtt_x[slot] = mtt->value;
        } else {
            s->mtt_y[slot] = mtt->value;
        }
        s->mtt_seen[slot] = true;
        return;
    case INPUT_MULTI_TOUCH_TYPE_BEGIN:
    case INPUT_MULTI_TOUCH_TYPE_UPDATE:
    case INPUT_MULTI_TOUCH_TYPE_END:
    case INPUT_MULTI_TOUCH_TYPE_CANCEL:
        if (!s->mtt_seen[slot]) {
            return;
        }
        ipad1_map_touch(s->mtt_x[slot], s->mtt_y[slot], &fx, &fy);
        bool down = mtt->type == INPUT_MULTI_TOUCH_TYPE_BEGIN ||
                    mtt->type == INPUT_MULTI_TOUCH_TYPE_UPDATE;
        ipod_touch_multitouch_set_finger(s->mt, slot, fx, fy, down);
        if (!down) {
            s->mtt_seen[slot] = false;
        }
        return;
    default:
        return;
    }
}

static const QemuInputHandler ipad1_mtt_handler = {
    .name  = "iPad Multitouch",
    .mask  = INPUT_EVENT_MASK_MTT,
    .event = ipad1_mtt_event,
};

/* GPIO pin level for the awake path; Home/Hold also go to the PMU, which
 * is the wake source once the kernel has put the AP to sleep. */
static void ipad1_set_button(IPad1MachineState *s, int pin, bool down)
{
    qemu_set_irq(qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(pin)), !down);
    if (pin == S5L8930_GPIO_BTN_HOLD || pin == S5L8930_GPIO_BTN_MENU) {
        s5l8930_d1815_button(s->pmu, pin == S5L8930_GPIO_BTN_HOLD, down);
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
    ipad1_set_button(s, pin, down);
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
    ipad1_set_button(s, pins[button], down);
}

static const QemuInputHandler ipad1_kbd_handler = {
    .name  = "iPad Buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = ipad1_kbd_event,
};

static void ipad1_battery_update(IPad1MachineState *s);

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
    define_arm_cp_regs(s->cpu, ipad1_cp_reginfo);
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
        s->pmu = pmu;
        DeviceState *xp = DEVICE(i2c_slave_create_simple(bus, TYPE_S5L8930_TCA6408, 0x20));
        s->ltc = DEVICE(i2c_slave_create_simple(bus, TYPE_S5L8930_LTC4099, 0x09));
        s5l8930_ltc4099_set_usb(s->ltc, s->usb_cable);
        qdev_connect_gpio_out(pmu, 0,
                              qemu_irq_invert(qdev_get_gpio_in(s->gpio, 0x0d)));
        qdev_connect_gpio_out(xp, 0,
                              qemu_irq_invert(qdev_get_gpio_in(s->gpio, 0x11)));
        /*
         * CS42L61 codec (i2c0/audio0). AppleCS42L61Audio treats it as a plain
         * MAP-addressed register file (0x01-0x6f, read back for its register
         * dump) and never checks the chip ID, so the iPod's CS42L58 model
         * fits unchanged. Its MCLK comes from the PWM block, which stays in
         * the unimplemented window.
         */
        i2c_slave_create_simple(bus, TYPE_CS42L58, 0x4a);
        /*
         * CD3282 "Mikey" headset controller (i2c0/mikey). AppleCS42L61Audio
         * resolves the codec's 'mikey' platform function during its power-up
         * (c08213d4 -> c082273c) and waits until AppleCD3282Mikey provides
         * it, so without this slave the codec never registers its "Codec"
         * IOAudio2 device and mediaserverd fails every sound with '!dev'.
         * The iPod's CD3272 model (all registers read 0: nothing plugged in)
         * is enough for the driver to start.
         */
        i2c_slave_create_simple(bus, TYPE_CD3272MIKEY, 0x39);
    }
    /*
     * I2C2: LIS331DLH accelerometer and TSL2581 light sensor. AppleLIS331DLH
     * (7B500 c0895000-c0895c00) probes WHO_AM_I without checking the value,
     * writes CTRL_REG1-4 and INT1_CFG/THS/DURATION, and reads OUT_X/Y/Z as
     * 16-bit bursts (sub-address | 0x80); the iPod LIS302DL model does all of
     * that once told the part's WHO_AM_I. The DT interrupt pins (0x24/0x26,
     * 0x25) are not driven: nothing before userland waits on them.
     */
    dev = qdev_new(TYPE_S5L8930_I2C);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_I2C_BASE(2));
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_I2C(2)));
    {
        I2CBus *bus = I2C_BUS(qdev_get_child_bus(dev, "i2c"));
        I2CSlave *accel = i2c_slave_new(TYPE_LIS302DL, 0x19);

        qdev_prop_set_uint8(DEVICE(accel), "whoami", 0x32);
        i2c_slave_realize_and_unref(accel, bus, &error_fatal);
        s->accel = LIS302DL(accel);
        /* Same names as the iPod machine: UIDeviceOrientation 0-6, e.g.
         * qom-set path=/machine property=accel-orientation value=3; raw
         * counts; a shake. accel-pitch/-roll/-pose are machine properties. */
        object_property_add_alias(OBJECT(machine), "accel-orientation",
                                  OBJECT(accel), "orientation");
        object_property_add_alias(OBJECT(machine), "accel-x", OBJECT(accel), "x");
        object_property_add_alias(OBJECT(machine), "accel-y", OBJECT(accel), "y");
        object_property_add_alias(OBJECT(machine), "accel-z", OBJECT(accel), "z");
        object_property_add_alias(OBJECT(machine), "accel-shake", OBJECT(accel), "shake");
        i2c_slave_create_simple(bus, TYPE_S5L8930_TSL2581, 0x39);
    }

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
     * Wi-Fi: the iPod's Broadcom dongle model, dressed as the unit's BCM4329
     * (K48 USI board: the CIS strings pick AppleBCMWLAN's "K48 USI X17B"
     * personality), behind the SDHC and the IOP's SDIO task. Frames go to
     * -netdev ...,id=wifi0. docs/ipad1/wifi.md.
     */
    DeviceState *sdio = NULL;
    if (s->wifi) {
        static const BCMSDIOChip bcm4329 = {
            .manfid = 0x02d0, .prodid = 0x4329,
            .chipid = 0x00034329,                   /* rev 3 = B1 (c07a61d2) */
            .sdiod_base = 0x18011000,               /* where initDongle polls */
            .vers1 = { "", "", "s=B1", "P=K48 m=u80" },
            .mac = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 },  /* = DT */
        };
        IPodTouchSDIOState *card = IPOD_TOUCH_SDIO(qdev_new(TYPE_IPOD_TOUCH_SDIO));

        ipod_touch_sdio_set_chip(card, &bcm4329);
        card->card_present = true;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(card), &error_fatal);
        ipod_touch_sdio_setup_net(card);

        sdio = qdev_new(TYPE_S5L8930_SDIO);
        object_property_set_link(OBJECT(sdio), "card", OBJECT(card), &error_fatal);
        sbd = SYS_BUS_DEVICE(sdio);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8930_SDIO_BASE);
        sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_SDIO));
        sysbus_connect_irq(SYS_BUS_DEVICE(card), 0, qdev_get_gpio_in(sdio, 0));
    }

    /*
     * IOP: high-level emulation of the second core. No IRQ line: like the real
     * firmware, the model raises the AP by writing VIC0's SOFTINT register.
     */
    dev = qdev_new(TYPE_S5L8930_IOP);
    if (sdio) {
        object_property_set_link(OBJECT(dev), "sdio", OBJECT(sdio), &error_fatal);
    }
    if (s->nand_path) {
        qdev_prop_set_string(dev, "nand", s->nand_path);
    }
    if (s->nand_overlay_path) {
        qdev_prop_set_string(dev, "nand-overlay", s->nand_overlay_path);
    }
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_IOP_BASE);
    sysbus_mmio_map(sbd, 1, S5L8930_IOP_VIC_BASE);

    /* SHA-1 engine; CDMA channel 4 streams the data into its FIFO. */
    dev = qdev_new(TYPE_S5L8930_SHA1);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_SHA1_BASE);

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
     * reused unchanged. The host bridge (usbmuxd-qemu) is dialled from
     * usb-tcp-addr, or IT_USB_TCP=host:port when that is unset. Without a
     * host the guest would never be configured, the power source would see
     * < 500 mA and let the device deep-sleep a few minutes after SpringBoard,
     * so without a bridge the OTG's built-in host enumerates it; either way
     * it behaves like an iPad on a Mac (charging, idle sleep disabled).
     */
    dev = qdev_new(TYPE_IPOD_TOUCH_USB_PHYS);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_USB_PHY_BASE);

    dev = ipod_touch_init_usb_otg(ipad1_irq(s, S5L8930_IRQ_USB_OTG),
                                  s5l8930_usb_hwcfg);
    s->usb_otg = S5L8900USBOTG(dev);
    if (s->usb_tcp_addr && s->usb_tcp_addr[0]) {
        char *colon = strrchr(s->usb_tcp_addr, ':');

        s->usb_otg->server_port = colon ? atoi(colon + 1) : 0;
        if (!s->usb_otg->server_port) {
            s->usb_otg->server_port = 1235;
        }
        s->usb_otg->server_host = colon && colon > s->usb_tcp_addr
            ? g_strndup(s->usb_tcp_addr, colon - s->usb_tcp_addr)
            : g_strdup("127.0.0.1");
    }
    /* No bridge: a built-in host enumerates and configures the device, which
     * is what keeps an iPad on a Mac charging and out of deep sleep. */
    s->usb_otg->builtin_host = !s->usb_otg->server_host && !getenv("IT_USB_TCP");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, S5L8930_USB_OTG_BASE,
                                &S5L8900USBOTG(dev)->iomem);
    /*
     * USB host, next to device mode: the DT we build gives usb-complex
     * "hsic-enabled", which makes AppleS5L8930XUSBArbitrator publish the host
     * nubs (EHCI, OHCI0) at start and never tear them down on cable changes,
     * so usbmux and a USB keyboard coexist (docs/ipad1/usb-keyboard.md).
     * AppleUSBEHCIARM/AppleUSBOHCIARM are plain EHCI/OHCI; capabilities at +0.
     * Its USB_CTL read-modify-writes (0xbf108000 bit 0 host, bit 2 HSIC)
     * stay in the unimp window. Attach with -device usb-kbd,bus=ehci.0.
     */
    dev = qdev_new(TYPE_EXYNOS4210_EHCI);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_USB_EHCI_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_USB_EHCI));

    dev = qdev_new(TYPE_SYSBUS_OHCI);
    qdev_prop_set_uint32(dev, "num-ports", 1);      /* DT rh-ports */
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_USB_OHCI0_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_USB_OHCI0));

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
    s->mt->profile = &mt_profile_k48;
    /* Zephyr2 ATN -> GPIO 0x15; reset (0x204) and download (0x107) are ignored. */
    qdev_connect_gpio_out_named(DEVICE(s->mt), "atn", 0,
        qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(S5L8930_GPIO_MT_ATN)));
    qemu_add_mouse_event_handler(ipad1_mouse_event, s->mt, 1, "iPad Touchscreen");
    qemu_input_handler_register(s->gpio, &ipad1_kbd_handler);
    qemu_input_handler_register(s->gpio, &ipad1_mtt_handler);

    /*
     * SPI2 is the baseband link. The Wi-Fi iPad has the controller but no
     * baseband, and the real unit's IORegistry still shows
     * AppleS5L8920XBasebandSPIController/BasebandSPIDevice loaded on it, so
     * model exactly that: a controller with nothing on the bus (reads return
     * 0). Its DT interrupt is the SRDY GPIO, not a VIC line, so none is wired.
     */
    set_spi_base(2);
    sysbus_create_simple(TYPE_IPOD_TOUCH_SPI, S5L8930_SPI_BASE(2), NULL);

    /* SWI: backlight and DPSM core voltage; only the busy bit matters. */
    sysbus_create_simple("ipodtouch.swi", S5L8930_SWI_BASE, NULL);

    /*
     * I2S0-2. i2s0 carries the CS42L61 codec's PCM from CDMA channel 0x1a to
     * the host audio backend; i2s1 (voice) and i2s2 (baseband) are register
     * files whose FIFO data is dropped. The CDMA model paces all three.
     */
    for (i = 0; i < 3; i++) {
        dev = qdev_new(TYPE_S5L8930_I2S);
        qdev_prop_set_bit(dev, "audio-out", i == 0);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8930_I2S_BASE(i));
    }

    /*
     * AMC (audio media codec, amc,s5l8920x): the same AppleAMC_r2 kext family
     * the iPod's 3.1.3 drives, one hardware revision up ("AMC 2.1"). Reuse the
     * iPod model's register/interrupt handshake; its buffer aperture is the
     * 256 KiB at 0x84000000 that the machine already backs as SRAM. The third
     * DT window (0x84300000, 0x5000) stays unimplemented; UI sounds and PCM
     * playback never reach the AMC (it is the hardware decode transformer).
     */
    dev = qdev_new(TYPE_IPOD_TOUCH_AMC);
    qdev_prop_set_uint64(dev, "buf-base", S5L8930_SRAM_BASE);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8930_AMC_BASE);
    sysbus_connect_irq(sbd, 0, ipad1_irq(s, S5L8930_IRQ_AMC));

    /* Same Samsung UART as the S5L8720, including its interrupt scheme. */
    exynos4210_uart_create(S5L8930_UART_BASE(0), 256, 0, serial_hd(0),
                           ipad1_irq(s, S5L8930_IRQ_UART(0)), true);
    /* UART5 is the bq27545 gas gauge's HDQ line (see s5l8930_hdq.c). */
    exynos4210_uart_create(S5L8930_UART_BASE(5), 256, 5,
                           qemu_chardev_new(NULL, TYPE_CHARDEV_S5L8930_HDQ,
                                            NULL, NULL, &error_abort),
                           ipad1_irq(s, S5L8930_IRQ_UART(5)), true);
    ipad1_battery_update(s);

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

static char *ipad1_get_nand_overlay(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->nand_overlay_path);
}

static void ipad1_set_nand_overlay(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->nand_overlay_path);
    s->nand_overlay_path = g_strdup(value);
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

static char *ipad1_get_usb_tcp_addr(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->usb_tcp_addr);
}

static void ipad1_set_usb_tcp_addr(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    g_free(s->usb_tcp_addr);
    s->usb_tcp_addr = g_strdup(value);
}

static bool ipad1_get_usb_cable(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->usb_cable;
}

/* Plug/unplug at any time (qom-set /machine usb-cable off): the charger's
 * usb_det level flips and the PMU raises the cable event that makes the
 * power source and the USB arbitrator re-evaluate. */
static void ipad1_set_usb_cable(Object *obj, bool value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    if (s->usb_cable == value) {
        return;
    }
    s->usb_cable = value;
    if (s->ltc) {
        s5l8930_ltc4099_set_usb(s->ltc, value);
        synopsys_usb_set_cable(s->usb_otg, value);
        s5l8930_d1815_usb_cable_event(s->pmu);
    }
    ipad1_battery_update(s);
}

/*
 * --- battery -------------------------------------------------------------
 * SpringBoard's level comes from AppleD1815PMUPowerSource's reading of the
 * battery voltage (PMU ADC mux 4), not the gas gauge, which configd never
 * opens on 7B500; charging is the LTC4099's charge-state bits.
 * Measured on 7B500: the level is sampled only at boot (it stayed put through
 * level, charge-state and cable changes), so these are power-on settings
 * (-M ipad1,battery-level=…) until the power source's polling trigger is
 * found; 3.90 V showed 63% and 3.525 V 2%, hence 3.50 V + 6.35 mV per %.
 * The charge-state bits are set but "Not Charging" still shows, so
 * battery-charging has no visible effect yet.
 */

static bool ipad1_battery_charging(IPad1MachineState *s)
{
    return s->battery_mode == 1 || (s->battery_mode == 0 && s->usb_cable);
}

/* Push level and charge state to the PMU and charger. */
static void ipad1_battery_update(IPad1MachineState *s)
{
    if (!s->pmu || !s->ltc) {
        return;
    }
    s5l8930_d1815_set_vbat(s->pmu, 3500 + lround(s->battery_level * 6.35));
    s5l8930_ltc4099_set_charging(s->ltc, ipad1_battery_charging(s));
}

static void ipad1_get_battery_level(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    int64_t value = lround(IPAD1_MACHINE(obj)->battery_level);
    visit_type_int(v, name, &value, errp);
}

static void ipad1_set_battery_level(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 100) {
        error_setg(errp, "battery-level must be between 0 and 100");
        return;
    }
    s->battery_level = value;
    ipad1_battery_update(s);
}

static void ipad1_get_battery_drain(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    visit_type_number(v, name, &IPAD1_MACHINE(obj)->battery_drain, errp);
}

static void ipad1_set_battery_drain(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);
    double value;

    if (!visit_type_number(v, name, &value, errp)) {
        return;
    }
    if (!isfinite(value) || value < 0 || value > 100) {
        error_setg(errp, "battery-drain must be between 0 and 100 percent per minute");
        return;
    }
    s->battery_drain = value;
}

static char *ipad1_get_battery_charging(Object *obj, Error **errp)
{
    return g_strdup(ipad1_battery_modes[IPAD1_MACHINE(obj)->battery_mode]);
}

static void ipad1_set_battery_charging(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    for (int i = 0; i < ARRAY_SIZE(ipad1_battery_modes); i++) {
        if (!strcmp(value, ipad1_battery_modes[i])) {
            s->battery_mode = i;
            ipad1_battery_update(s);
            return;
        }
    }
    error_setg(errp, "battery-charging must be auto, on or off");
}

/* --- tilt --------------------------------------------------------------- */

/*
 * The app's attitude (see hw/arm/ipod-attitude.h). The iPad's accelerometer
 * reads roll the opposite way round to the iPod's for the same turn of the
 * device: turned clockwise it must report UIDeviceOrientation 3 (roll +90),
 * where the app sends -90 (found by frame dumps, as for qemu_ios_ui_rotate).
 * ponytail: pitch keeps the iPod's sign, unverified on the iPad.
 */
static void ipad1_apply_attitude(IPad1MachineState *s)
{
    lis302dl_apply_attitude(s->accel, s->accel_pitch, -s->accel_roll, s->accel_flat);
}

static void ipad1_get_accel_angle(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);
    double value = !strcmp(name, "accel-pitch") ? s->accel_pitch : s->accel_roll;

    visit_type_number(v, name, &value, errp);
}

static void ipad1_set_accel_angle(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);
    double value;

    if (!visit_type_number(v, name, &value, errp)) {
        return;
    }
    if (!isfinite(value) || value < -180 || value > 180) {
        error_setg(errp, "%s must be finite and between -180 and 180 degrees", name);
        return;
    }
    *(!strcmp(name, "accel-pitch") ? &s->accel_pitch : &s->accel_roll) = value;
    ipad1_apply_attitude(s);
}

static char *ipad1_get_accel_pose(Object *obj, Error **errp)
{
    return g_strdup(IPAD1_MACHINE(obj)->accel_flat ? "flat" : "upright");
}

static void ipad1_set_accel_pose(Object *obj, const char *value, Error **errp)
{
    IPad1MachineState *s = IPAD1_MACHINE(obj);

    if (strcmp(value, "flat") && strcmp(value, "upright")) {
        error_setg(errp, "accel-pose must be upright or flat");
        return;
    }
    s->accel_flat = !strcmp(value, "flat");
    ipad1_apply_attitude(s);
}

static bool ipad1_get_wifi(Object *obj, Error **errp)
{
    return IPAD1_MACHINE(obj)->wifi;
}

static void ipad1_set_wifi(Object *obj, bool value, Error **errp)
{
    IPAD1_MACHINE(obj)->wifi = value;
}

static void ipad1_instance_init(Object *obj)
{
    IPAD1_MACHINE(obj)->usb_cable = true;
    guest_pb_init(&IPAD1_MACHINE(obj)->pb, obj, "ipad1");
    IPAD1_MACHINE(obj)->battery_level = 80;
}

static void ipad1_instance_finalize(Object *obj)
{
    g_free(IPAD1_MACHINE(obj)->usb_tcp_addr);
    g_free(IPAD1_MACHINE(obj)->kboot_path);
    g_free(IPAD1_MACHINE(obj)->nand_path);
    g_free(IPAD1_MACHINE(obj)->nand_overlay_path);
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
    object_class_property_add_str(klass, "nand-overlay", ipad1_get_nand_overlay,
                                  ipad1_set_nand_overlay);
    object_class_property_set_description(klass, "nand-overlay",
        "Copy-on-write directory for guest NAND writes; the nand store is then read-only");
    object_class_property_add_str(klass, "nor", ipad1_get_nor, ipad1_set_nor);
    object_class_property_set_description(klass, "nor",
        "1 MiB SPI NOR image (nvram, syscfg); erased flash if unset");
    object_class_property_add_str(klass, "usb-tcp-addr", ipad1_get_usb_tcp_addr,
                                  ipad1_set_usb_tcp_addr);
    object_class_property_set_description(klass, "usb-tcp-addr",
        "usbmuxd-qemu host bridge host:port (default port 1235); unset = IT_USB_TCP or no link");
    object_class_property_add_bool(klass, "wifi", ipad1_get_wifi, ipad1_set_wifi);
    object_class_property_set_description(klass, "wifi",
        "Model the BCM4329 Wi-Fi card (frames to -netdev id=wifi0); off = no card");
    object_class_property_add_bool(klass, "usb-cable", ipad1_get_usb_cable,
                                   ipad1_set_usb_cable);
    object_class_property_set_description(klass, "usb-cable",
        "USB cable present (default on); settable at runtime to plug/unplug");
    /* The iPod machine's names, so the app bridge drives both unchanged. */
    object_class_property_add_bool(klass, "usb-attached", ipad1_get_usb_cable,
                                   ipad1_set_usb_cable);
    object_class_property_add(klass, "battery-level", "int", ipad1_get_battery_level,
                              ipad1_set_battery_level, NULL, NULL);
    object_class_property_add(klass, "battery-drain", "number", ipad1_get_battery_drain,
                              ipad1_set_battery_drain, NULL, NULL);
    object_class_property_add_str(klass, "battery-charging", ipad1_get_battery_charging,
                                  ipad1_set_battery_charging);
    object_class_property_add(klass, "accel-pitch", "number", ipad1_get_accel_angle,
                              ipad1_set_accel_angle, NULL, NULL);
    object_class_property_add(klass, "accel-roll", "number", ipad1_get_accel_angle,
                              ipad1_set_accel_angle, NULL, NULL);
    object_class_property_add_str(klass, "accel-pose", ipad1_get_accel_pose,
                                  ipad1_set_accel_pose);
}

static const TypeInfo ipad1_machine_info = {
    .name = TYPE_IPAD1_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(IPad1MachineState),
    .instance_init = ipad1_instance_init,
    .instance_finalize = ipad1_instance_finalize,
    .class_init = ipad1_class_init,
};

static void ipad1_machine_types(void)
{
    type_register_static(&ipad1_machine_info);
}

type_init(ipad1_machine_types)
