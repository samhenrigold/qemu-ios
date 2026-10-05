/*
 * Apple S5L8920/S5L8922 boards, Cortex-A8: iPod touch 3G (N18AP, -M n18).
 * What differs per board is an S5L8920Board below; the rest is the SoC.
 *
 * The S5L8920 is the A4's predecessor and shares most of its blocks: the
 * same peripheral window at 0x80000000, the IOP, CDMA/AES, SHA-1, PKE, SPI,
 * UART and I2C, so those are the s5l8930_* and ipod_touch_* models, placed
 * at this SoC's addresses and interrupt numbers (DT arm-io, docs/n18/). The
 * display is not the A4's pipe but the S5L8720's M2 CLCD.
 *
 * Boot input is a K48KBOOT bundle (imgtools/s5l8920_kboot.py), loaded as
 * the ipad1 machine loads it: kernel, filled device tree and boot_args in
 * DRAM, the CPU entered at the kernel with r0 = boot_args.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/misc/unimp.h"
#include "hw/core/split-irq.h"
#include "hw/arm/ipod_touch_pke.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "hw/arm/ipod_touch_tvout.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "hw/arm/ipod_touch_lis302dl.h"
#include "hw/arm/ipod_touch_cs42l58.h"
#include "hw/arm/ipod_touch_cd3272_mikey.h"
#include "hw/i2c/i2c.h"
#include "chardev/char.h"
#include "hw/sysbus.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/s5l8930.h"
#include "hw/intc/pl192.h"
#include "hw/qdev-properties.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "hw/arm/guest-services/general.h"
#include "hw/arm/guest-services/gles.h"
#include "hw/arm/guest-package.h"

/* Addresses and interrupts: N18AP 8C148 DT (arm-io maps child offsets at 0x80000000). */
#define S5L8920_DRAM_BASE       0x40000000
#define S5L8920_SRAM_BASE       0x84000000
#define S5L8920_SRAM_SIZE       0x00040000
#define S5L8920_PMGR_BASE       0xbf100000
#define S5L8920_VIC_BASE(n)     (0xbf200000 + (n) * 0x10000)
#define S5L8920_VIC_COUNT       3
#define S5L8920_CHIPID_BASE     0xbf500000
#define S5L8920_CPU_DEBUG_BASE  0xbf700000
#define S5L8920_GPIO_BASE       0x83000000
#define S5L8920_UART_BASE(n)    (0x82500000 + (n) * 0x100000)
#define S5L8920_IOP_BASE        0x86300000
#define S5L8920_IOP_VIC_BASE    0xbf300000
#define S5L8920_SHA1_BASE       0x80100000
#define S5L8920_PKE_BASE        0x83100000
#define S5L8920_CDMA_BASE       0x87000000
#define S5L8920_AES_BASE        0x87800000
#define S5L8920_CDMA_CHANNELS   28          /* reg 0x1c000 */
#define S5L8920_H2FMI_BASE(n)   (0x81200000 + (n) * 0x100000)
#define S5L8920_SPI_BASE(n)     (0x82000000 + (n) * 0x100000)
#define S5L8920_I2C_BASE(n)     (0x83200000 + (n) * 0x100000)
#define S5L8920_CLCD_BASE       0x85400000      /* M2 CLCD, clcd,s5l8720x */
#define S5L8920_SCALER_BASE     0x85500000
#define S5L8920_USB_PHY_BASE    0x86000000
#define S5L8920_USB_OTG_BASE    0x86100000
#define S5L8920_DSIM_BASE       0x89000000
#define S5L8920_SWI_BASE        0x89100000
#define S5L8920_DART_BASE(n)    (0xbfe00000 + (n) * 0x100000)
#define S5L8920_I2S0_FIFO       0x84500000      /* DT i2s0 reg; what the CDMA writes */
#define S5L8920_I2S0_BASE       0x84500400      /* its registers, the A4's i2s layout */

#define S5L8920_IRQ_TIMER0      0x06
#define S5L8920_IRQ_TIMER1      0x05
#define S5L8920_IRQ_UART(n)     (0x18 - (n))    /* they count down from uart0 */
#define S5L8920_IRQ_GPIO        0x5e
#define S5L8920_IRQ_FMI(n)      (0x1f - (n))
#define S5L8920_IRQ_SPI(n)      (0x1d - (n))
#define S5L8920_IRQ_I2C(n)      (0x13 - (n))
#define S5L8920_IRQ_USB_OTG     0x0e
#define S5L8920_IRQ_SCALER      0x0c
#define S5L8920_IRQ_CLCD        0x25
#define S5L8920_IRQ_DART(n)     (0x5a - (n))
#define S5L8920_IRQ_CDMA(ch)    (0x2a + (ch))   /* DT lists channels 1.. from 0x2b */

typedef struct S5L8920I2CDevice {
    uint8_t bus;                         /* i2c0 or i2c2 */
    uint8_t addr;
    const char *type;
    int16_t irq_pin;                     /* GPIO interrupt its gpio-out 0 drives, active low; 0 = none */
} S5L8920I2CDevice;

/* DT buttons interrupts: GPIO interrupt numbers, all active low. */
typedef struct S5L8920Buttons {
    uint16_t hold, menu, volup, voldown;
} S5L8920Buttons;

/* Where SpringBoard's "slide to power off" knob sits (portrait), and how far to drag it. */
typedef struct S5L8920PowerKnob {
    int x, y, drag;
} S5L8920PowerKnob;

typedef struct S5L8920Board {
    const char *desc;
    uint64_t dram_size;
    uint32_t chipid[2];                  /* ChipID fuse words 0-1 */
    uint8_t board_id;                    /* PMGR POWER_ID[23:16] */
    int nuarts;
    int8_t gauge_uart;                   /* bq27540 HDQ gas gauge; -1 = none */
    uint16_t gauge_mah;
    bool nor;                            /* a SPI NOR on spi0 (the N88's; the N18 boots from NAND) */
    bool baseband;                       /* spi2, the baseband link */
    uint32_t fmc_off;                    /* FMC within each FMI window, as its IOP firmware addresses it */
    uint16_t mt_atn;                     /* multi-touch ATN, a GPIO interrupt */
    S5L8920Buttons buttons;
    S5L8920I2CDevice i2c[8];             /* in creation order (the snapshot's) */
    S5L8920PowerKnob pwroff_knob;
} S5L8920Board;

/* iPod touch 3G (N18AP, S5L8922). */
static const S5L8920Board s5l8920_n18 = {
    .desc = "iPod touch 3G (N18AP, S5L8922)",
    .dram_size = 0x10000000,
    /* ponytail: the K48's fuse words; the 8922's are unmeasured. */
    .chipid = { 0x31800387, 0x80758000 },
    .board_id = 0x02,
    .fmc_off = 0x40000,                  /* the s5l8922x IOP firmware: the A4's layout */
    .nuarts = 2,
    .gauge_uart = -1,
    .mt_atn = 0xb4,
    .buttons = { .hold = 0xb7, .menu = 0xb6, .volup = 0xb0, .voldown = 0xb1 },
    .i2c = {
        /* ponytail: the D1755 PMU as the iPod 2G's D1759 register model. */
        { 0, 0x74, TYPE_PCF50633, 0x9d },
        { 0, 0x4a, TYPE_CS42L58 },
        { 0, 0x3a, TYPE_CD3272MIKEY },
        { 2, 0x1d, TYPE_LIS302DL },       /* its DT interrupt (0xa2) is not driven */
    },
    .pwroff_knob = { 57, 67, 240 },      /* off a 4.2.1 screendump of the sheet */
};

/* iPhone 3GS (N88AP, S5L8920); N88AP 8C148a DT. */
static const S5L8920Board s5l8920_n88 = {
    .desc = "iPhone 3GS (N88AP, S5L8920)",
    .dram_size = 0x10000000,
    /* ponytail: the K48's fuse words; the 8920's are unmeasured. */
    .chipid = { 0x31800387, 0x80758000 },
    .board_id = 0x00,
    .fmc_off = 0x400,                    /* the s5l8920x IOP firmware: FMC +0x400, ECC +0x800 */
    .nuarts = 5,                         /* iap, debug, umts, bluetooth, gas gauge */
    .gauge_uart = 4,
    .gauge_mah = 1219,                   /* ponytail: the 3GS's rated cell, not measured */
    .nor = true,
    .baseband = true,
    .mt_atn = 0xb4,
    .buttons = { .hold = 0xb7, .menu = 0xb6, .volup = 0xb0, .voldown = 0xb1 },
    .i2c = {
        { 0, 0x74, TYPE_PCF50633, 0x9d },
        { 0, 0x4a, TYPE_CS42L58 },       /* cs42l61: a register file to its driver, as on the iPad */
        { 0, 0x39, TYPE_CD3272MIKEY },
        { 0, 0x1d, TYPE_LIS302DL },
        { 0, 0x1e, TYPE_S5L8930_AK8973 },
    },
    .pwroff_knob = { 57, 67, 240 },
};

#define TYPE_S5L8920_MACHINE "s5l8920-machine"
OBJECT_DECLARE_TYPE(S5L8920MachineState, S5L8920MachineClass, S5L8920_MACHINE)

struct S5L8920MachineClass {
    MachineClass parent;
    const S5L8920Board *board;
};

struct S5L8920MachineState {
    MachineState parent;
    const S5L8920Board *board;
    ARMCPU *cpu;
    MemoryRegion dram, dram_hi, sram, chipid, cpu_debug;
    DeviceState *vic[S5L8920_VIC_COUNT];
    DeviceState *gpio;
    DeviceState *iopcore;
    IPodTouchMultitouchState *mt;
    synopsys_usb_state *usb_otg;
    Pcf50633State *pmu;
    LIS302DLState *accel;
    char *kboot_path;
    char *nand_path;
    char *nand_overlay_path;
    char *nor_path;
    char *nor_rw_path;
    char *usb_tcp_addr;                  /* usbmuxd-qemu host bridge; empty = the built-in host */
    bool btn_hold, btn_home;             /* button-hold/-home properties */
    bool gles_debug;                     /* paint what the GL bridge refuses magenta (tests) */
    GuestPackage pkg;                    /* hw/arm/guest-package.c: it_boot and the GL shim's hello */
    QEMUTimer *pwroff_timer;             /* system_powerdown gesture */
    int pwroff_phase, pwroff_step;
};

/* hw/arm/s5l8920_dart.c: IOVA -> PA through a DART (its device as opaque). */
hwaddr s5l8920_dart_xlate(void *opaque, uint32_t va, unsigned sid);

#define KBOOT_MAGIC "K48KBOOT"
#define KBOOT_TRAILER_LEN 24
#define KBOOT_SEGMENT_LEN 20

static qemu_irq s5l8920_irq(S5L8920MachineState *s, int irq)
{
    qemu_irq ap = qdev_get_gpio_in(s->vic[irq / 32], irq % 32);
    DeviceState *split;

    /* The IOP's VICs see the same sources under the same numbers. */
    split = qdev_new(TYPE_SPLIT_IRQ);
    qdev_prop_set_uint16(split, "num-lines", 2);
    qdev_realize_and_unref(split, NULL, &error_fatal);
    qdev_connect_gpio_out(split, 0, ap);
    qdev_connect_gpio_out(split, 1, s5l8930_iop_core_irq(s->iopcore, irq));
    return qdev_get_gpio_in(split, 0);
}

/*
 * Stage the K48KBOOT bundle on every reset, as ipad1_cpu_reset does.
 * ponytail: a copy of the ipad1 loader; share it once both machines settle.
 */
static void s5l8920_cpu_reset(void *opaque)
{
    S5L8920MachineState *s = S5L8920_MACHINE(opaque);
    CPUState *cs = CPU(s->cpu);
    g_autofree char *data = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize size;
    const uint8_t *trailer;
    uint32_t load_pa, entry_pa, bootargs_pa, image_len;

    gles_host_set_debug(s->gles_debug);
    gles_host_reset();
    guest_pkg_reset(&s->pkg);
    cpu_reset(cs);
    if (!g_file_get_contents(s->kboot_path, &data, &size, &gerr)) {
        error_report("s5l8920: cannot read kboot bundle '%s': %s",
                     s->kboot_path, gerr->message);
        exit(1);
    }
    if (size < KBOOT_TRAILER_LEN ||
        memcmp(data + size - KBOOT_TRAILER_LEN, KBOOT_MAGIC, 8) != 0) {
        error_report("s5l8920: '%s' is not a K48KBOOT bundle", s->kboot_path);
        exit(1);
    }
    trailer = (const uint8_t *)data + size - KBOOT_TRAILER_LEN;
    load_pa = ldl_le_p(trailer + 8);
    entry_pa = ldl_le_p(trailer + 12);
    bootargs_pa = ldl_le_p(trailer + 16);
    image_len = ldl_le_p(trailer + 20);
    if (image_len > size - KBOOT_TRAILER_LEN || load_pa < S5L8920_DRAM_BASE ||
        (uint64_t)load_pa + image_len > S5L8920_DRAM_BASE + s->board->dram_size) {
        error_report("s5l8920: kboot bundle does not fit in DRAM");
        exit(1);
    }
    address_space_write(&address_space_memory, load_pa, MEMTXATTRS_UNSPECIFIED,
                        data, image_len);
    for (gsize off = image_len; off + KBOOT_SEGMENT_LEN <= size - KBOOT_TRAILER_LEN;) {
        const uint8_t *seg = (const uint8_t *)data + off;
        uint32_t pa = ldl_le_p(seg + 8), len = ldl_le_p(seg + 12);
        bool zero = ldl_le_p(seg + 16) & 1;

        off += KBOOT_SEGMENT_LEN + (zero ? 0 : len);
        if (memcmp(seg, "K48SEG\0\0", 8) != 0 || off > size - KBOOT_TRAILER_LEN) {
            error_report("s5l8920: malformed segment in kboot bundle");
            exit(1);
        }
        if (zero) {
            address_space_set(&address_space_memory, pa, 0, len, MEMTXATTRS_UNSPECIFIED);
        } else {
            address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                                seg + KBOOT_SEGMENT_LEN, len);
        }
    }
    s->cpu->env.regs[0] = bootargs_pa;
    cpu_set_pc(cs, entry_pa);
}

/* An I2C controller (the A4's block) and the board's slaves on it. */
static void s5l8920_i2c_create(S5L8920MachineState *s, int n)
{
    DeviceState *ctl = qdev_new(TYPE_S5L8930_I2C);
    SysBusDevice *sbd = SYS_BUS_DEVICE(ctl);
    I2CBus *bus;

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8920_I2C_BASE(n));
    sysbus_connect_irq(sbd, 0, s5l8920_irq(s, S5L8920_IRQ_I2C(n)));
    bus = I2C_BUS(qdev_get_child_bus(ctl, "i2c"));

    for (const S5L8920I2CDevice *d = s->board->i2c; d->type; d++) {
        I2CSlave *slave;

        if (d->bus != n) {
            continue;
        }
        slave = i2c_slave_new(d->type, d->addr);
        if (!strcmp(d->type, TYPE_PCF50633)) {
            /*
             * D1755: the D1759's Dialog layout one event byte wider. Events
             * 0x01-0x04, status 0x05-0x08 (bit 3 VBUS: the halt path
             * 805cb764), masks 0x09-0x0c (start writes FF DF FF FF), and
             * "pmu go stdby" sets 0x0d bit 0 (805cb7c4).
             */
            qdev_prop_set_uint8(DEVICE(slave), "event-count", 4);
            qdev_prop_set_uint8(DEVICE(slave), "shutdown-reg", 0x0d);
            qdev_prop_set_uint8(DEVICE(slave), "usb-status-reg", 0x05);
            /* ADC as the D1815's (control 0x30, 10-bit result 0x31-0x32,
             * _readADCGated 805cd2ca..805cd412); the RTC counter at 0x4c
             * (read twice as a ripple guard). */
            qdev_prop_set_uint8(DEVICE(slave), "adc-reg", 0x30);
            qdev_prop_set_uint8(DEVICE(slave), "rtc-reg", 0x4c);
            /* ponytail: the backlight is not decoded (DT backlight, SWI?);
             * keep the panel lit by pointing the enable at a scratch byte
             * the reset sets. Replace when the D1755 backlight is found. */
            qdev_prop_set_uint8(DEVICE(slave), "backlight-enable-reg", 0xfe);
            qdev_prop_set_uint8(DEVICE(slave), "backlight-enable-bit", 0x01);
            qdev_prop_set_uint8(DEVICE(slave), "backlight-level-reg", 0);
        }
        i2c_slave_realize_and_unref(slave, bus, &error_fatal);
        if (d->irq_pin) {
            qdev_connect_gpio_out(DEVICE(slave), 0,
                                  qemu_irq_invert(qdev_get_gpio_in(s->gpio, d->irq_pin)));
        }
        if (!strcmp(d->type, TYPE_PCF50633)) {
            s->pmu = PCF50633(slave);
            s->pmu->usb_cable = true;
        } else if (!strcmp(d->type, TYPE_LIS302DL)) {
            s->accel = LIS302DL(slave);
        }
    }
}

/*
 * The guest-services trap (mcr p15,3,Rn,c15,c15,0) the GLES shim
 * (contrib/gles-public) uses: GLES, and guest packages (which also answer
 * the shim's hello). ipad1.c's agent and pasteboard are left out until a
 * test here needs them.
 */
static void s5l8920_qemu_call(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    CPUState *cs = env_cpu(env);
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
        if (!guest_pkg_call(&S5L8920_MACHINE(qdev_get_machine())->pkg, cs, &q, &err)) {
            return;
        }
    }
    q.error = err;
    cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 1);
}

static const ARMCPRegInfo s5l8920_cp_reginfo[] = {
    { .name = "QEMU_CALL", .cp = 15, .opc1 = 3, .crn = 15, .crm = 15,
      .opc2 = 0, .access = PL0_RW, .state = ARM_CP_STATE_AA32,
      .type = ARM_CP_IO | ARM_CP_NO_RAW | ARM_CP_RAISES_EXC,
      .readfn = qemu_call_status,
      .writefn = s5l8920_qemu_call },
};

static void s5l8920_pwroff_tick(void *opaque);
static Notifier s5l8920_powerdown_notifier;

static void s5l8920_init(MachineState *machine)
{
    S5L8920MachineState *s = S5L8920_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    DeviceState *dev, *iop;
    SysBusDevice *sbd;
    int i;

    s->board = S5L8920_MACHINE_GET_CLASS(s)->board;
    if (!s->kboot_path) {
        error_report("s5l8920: kboot= is required");
        exit(1);
    }

    cpuobj = object_new(machine->cpu_type);
    s->cpu = ARM_CPU(cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(sysmem), &error_abort);
    object_property_set_bool(cpuobj, "has_el3", false, NULL);
    object_property_set_bool(cpuobj, "has_el2", false, NULL);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    object_unref(cpuobj);
    define_arm_cp_regs(s->cpu, s5l8920_cp_reginfo);

    memory_region_init_ram(&s->dram, NULL, "s5l8920.dram", s->board->dram_size,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8920_DRAM_BASE, &s->dram);
    memory_region_init_alias(&s->dram_hi, NULL, "s5l8920.dram-hi", &s->dram, 0,
                             s->board->dram_size);
    memory_region_add_subregion(sysmem, S5L8920_DRAM_BASE + s->board->dram_size,
                                &s->dram_hi);
    memory_region_init_ram(&s->sram, NULL, "s5l8920.sram", S5L8920_SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, S5L8920_SRAM_BASE, &s->sram);

    create_unimplemented_device("s5l8920.periph", 0x80000000, 0x40000000);

    {
        uint32_t chipid[] = { s->board->chipid[0], s->board->chipid[1], 0, 0 };

        memory_region_init_rom(&s->chipid, NULL, "s5l8920.chipid", 0x1000, &error_fatal);
        memcpy(memory_region_get_ram_ptr(&s->chipid), chipid, sizeof(chipid));
        memory_region_add_subregion(sysmem, S5L8920_CHIPID_BASE, &s->chipid);
    }
    memory_region_init_ram(&s->cpu_debug, NULL, "s5l8920.cpu-debug", 0x1000, &error_fatal);
    memory_region_add_subregion(sysmem, S5L8920_CPU_DEBUG_BASE, &s->cpu_debug);

    /* Three PL192s, daisy-chained VIC2 -> VIC1 -> VIC0 -> CPU. */
    s->vic[0] = pl192_manual_init((char *)"vic0",
                                  qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ),
                                  qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ),
                                  NULL);
    memory_region_add_subregion(sysmem, S5L8920_VIC_BASE(0), &PL192(s->vic[0])->iomem);
    for (i = 1; i < S5L8920_VIC_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("vic%d", i);
        s->vic[i] = pl192_manual_init(name, NULL);
        memory_region_add_subregion(sysmem, S5L8920_VIC_BASE(i), &PL192(s->vic[i])->iomem);
        PL192(s->vic[i])->daisy = PL192(s->vic[i - 1]);
    }
    /* The IOP: the kernel's EmbeddedIOP firmware on a second core. */
    s->iopcore = qdev_new("s5l8930.iop-core");
    object_property_set_link(OBJECT(s->iopcore), "dram", OBJECT(&s->dram), &error_abort);
    object_property_set_link(OBJECT(s->iopcore), "sysmem", OBJECT(sysmem), &error_abort);
    qdev_prop_set_uint32(s->iopcore, "dram-base", 0);   /* the kext's phys - 0x40000000 */
    qdev_realize_and_unref(s->iopcore, NULL, &error_fatal);

    sysbus_create_varargs("s5l8920.pmgr", S5L8920_PMGR_BASE,
                          s5l8920_irq(s, S5L8920_IRQ_TIMER0),
                          s5l8920_irq(s, S5L8920_IRQ_TIMER1), NULL);

    dev = qdev_new(TYPE_S5L8930_GPIO);
    s->gpio = dev;
    qdev_prop_set_uint32(dev, "ports", 0x2e);           /* DT #gpio-ports */
    qdev_prop_set_uint32(dev, "int-groups", 7);         /* DT #interrupt-groups */
    qdev_prop_set_bit(dev, "pin-int-enable", true);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8920_GPIO_BASE);
    sysbus_connect_irq(sbd, 0, s5l8920_irq(s, S5L8920_IRQ_GPIO));

    for (i = 0; i < s->board->nuarts; i++) {
        Chardev *chr = serial_hd(i);
        int fifo = 256;

        if (i == s->board->gauge_uart) {
            /* the bq27540 behind HDQ-over-UART, as the iPad's (a 16-byte FIFO, s5l8930_hdq.c) */
            chr = qemu_chardev_new(NULL, TYPE_CHARDEV_S5L8930_HDQ, NULL, NULL, &error_abort);
            s5l8930_hdq_set_capacity(chr, s->board->gauge_mah);
            s5l8930_hdq_set_battery(chr, 80, true);
            fifo = 16;
        }
        exynos4210_uart_create(S5L8920_UART_BASE(i), fifo, i, chr,
                               s5l8920_irq(s, S5L8920_IRQ_UART(i)), true);
    }

    /* AP side of the IOP; NAND pages come from its page store. */
    dev = qdev_new(TYPE_S5L8930_IOP);
    iop = dev;
    if (s->nand_path) {
        qdev_prop_set_string(dev, "nand", s->nand_path);
    }
    if (s->nand_overlay_path) {
        qdev_prop_set_string(dev, "nand-overlay", s->nand_overlay_path);
    }
    object_property_set_link(OBJECT(dev), "core", OBJECT(s->iopcore), &error_fatal);
    qdev_prop_set_bit(dev, "fw-size-mask", true);   /* the 8920's window register */
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8920_IOP_BASE);

    dev = qdev_new(TYPE_S5L8930_SHA1);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8920_SHA1_BASE);

    dev = qdev_new(TYPE_IPOD_TOUCH_PKE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, S5L8920_PKE_BASE, &IPOD_TOUCH_PKE(dev)->iomem);

    /* CDMA + AES: the A4's block with 28 of its channels wired. */
    dev = qdev_new(TYPE_S5L8930_CDMA);
    qdev_prop_set_uint8(dev, "version", 1);     /* DT cdma-version */
    qdev_prop_set_uint64(dev, "dram-size", s->board->dram_size);
    qdev_prop_set_uint32(dev, "paced-base", S5L8920_I2S0_FIFO);
    qdev_prop_set_uint32(dev, "paced-ports", 1);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S5L8920_CDMA_BASE);
    sysbus_mmio_map(sbd, 1, S5L8920_AES_BASE);
    for (i = 0; i < S5L8920_CDMA_CHANNELS; i++) {
        sysbus_connect_irq(sbd, i, s5l8920_irq(s, S5L8920_IRQ_CDMA(i)));
    }

    /* H2FMI: the two NAND buses the IOP firmware drives, over the IOP's page store. */
    {
        DeviceState *cdma = dev;

        dev = qdev_new(TYPE_S5L8930_H2FMI);
        object_property_set_link(OBJECT(dev), "iop", OBJECT(iop), &error_abort);
        object_property_set_link(OBJECT(dev), "cdma", OBJECT(cdma), &error_abort);
        qdev_prop_set_uint32(dev, "fmc-offset", s->board->fmc_off);
        qdev_prop_set_uint32(dev, "ecc-offset", s->board->fmc_off * 2);
        if (s->board->fmc_off == 0x400) {       /* the s5l8920x firmware's ECC summary */
            qdev_prop_set_uint32(dev, "ecc-blank-summary", 0x40);
        }
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        for (i = 0; i < 2; i++) {
            sysbus_mmio_map(sbd, i, S5L8920_H2FMI_BASE(i));
            sysbus_connect_irq(sbd, i, s5l8920_irq(s, S5L8920_IRQ_FMI(i)));
        }
    }

    s5l8920_i2c_create(s, 0);
    s5l8920_i2c_create(s, 2);

    /*
     * SPI0: the N88's 1 MiB NOR (as the iPad's), chip select GPIO 0x1204
     * (DT function-spi_cs0). The N18 has none; nor=/nor-rw= put one there
     * for kboot's grafted nor-flash node (s5l8920_kboot.py --nor).
     */
    if (s->board->nor || s->nor_path || (s->nor_rw_path && s->nor_rw_path[0])) {
        dev = ipod_touch_spi_create(S5L8920_SPI_BASE(0), s5l8920_irq(s, S5L8920_IRQ_SPI(0)), 0,
                                    "nor", false);
        IPOD_TOUCH_SPI(dev)->nor->nor_path = s->nor_path;
        if (s->nor_rw_path && s->nor_rw_path[0]) {
            ipod_touch_nor_spi_open_overlay(IPOD_TOUCH_SPI(dev)->nor, s->nor_rw_path, &error_fatal);
        }
        qdev_connect_gpio_out(s->gpio, S5L8930_GPIO_PIN(0x1204),
            qdev_get_gpio_in_named(DEVICE(IPOD_TOUCH_SPI(dev)->nor), SSI_GPIO_CS, 0));
    } else {
        ipod_touch_spi_create(S5L8920_SPI_BASE(0), s5l8920_irq(s, S5L8920_IRQ_SPI(0)), 0, "none", false);
    }
    /* SPI2: the baseband link (N88), a controller with nothing on it. */
    if (s->board->baseband) {
        ipod_touch_spi_create(S5L8920_SPI_BASE(2), NULL, 2, "none", false);
    }
    /* SPI1: the Zephyr multi-touch. */
    dev = qdev_new(TYPE_IPOD_TOUCH_SPI);
    qdev_prop_set_uint8(dev, "index", 1);
    qdev_prop_set_string(dev, "peripheral", "multitouch");
    qdev_prop_set_uint32(dev, "tx-fifo-depth", 0x10000);   /* N1F55 firmware: one 53196-byte transfer */
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S5L8920_SPI_BASE(1));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s5l8920_irq(s, S5L8920_IRQ_SPI(1)));
    s->mt = IPOD_TOUCH_SPI(dev)->mt;
    s->mt->profile = &mt_profile_n81;    /* the same N1F55 sensor */
    qdev_connect_gpio_out_named(DEVICE(s->mt), "atn", 0,
                                qdev_get_gpio_in(s->gpio, s->board->mt_atn));

    /*
     * DARTs: dart0 in front of the CLCD, scaler and TV-out (DT iommu-parent,
     * use-legacy-DART), dart1 in front of JPEG and the video encoder.
     */
    DeviceState *dart0 = sysbus_create_simple("s5l8920.dart", S5L8920_DART_BASE(0),
                                              s5l8920_irq(s, S5L8920_IRQ_DART(0)));
    sysbus_create_simple("s5l8920.dart", S5L8920_DART_BASE(1), s5l8920_irq(s, S5L8920_IRQ_DART(1)));

    /* Display: the S5L8720's M2 CLCD and the same Samsung MIPI-DSIM, a 320x480 panel. */
    dev = qdev_new(TYPE_IPOD_TOUCH_MIPI_DSI);
    IPOD_TOUCH_MIPI_DSI(dev)->direct_boot = true;
    IPOD_TOUCH_MIPI_DSI(dev)->hs_clock_at_reset = true;   /* as iBoot leaves the link */
    memory_region_add_subregion(sysmem, S5L8920_DSIM_BASE, &IPOD_TOUCH_MIPI_DSI(dev)->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    dev = qdev_new("ipodtouch.lcd");
    /* kboot's vram (the boot logo, then the kernel console): where iBoot leaves window 1 */
    qdev_prop_set_uint32(dev, "fb-base", 0x4f700000);
    qdev_prop_set_bit(dev, "ctrl-readback", true);
    IPOD_TOUCH_LCD(dev)->sysmem = sysmem;
    IPOD_TOUCH_LCD(dev)->mt = s->mt;
    memory_region_add_subregion(sysmem, S5L8920_CLCD_BASE, &IPOD_TOUCH_LCD(dev)->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s5l8920_irq(s, S5L8920_IRQ_CLCD));
    ipod_lcd_set_iommu(dev, s5l8920_dart_xlate, dart0);

    ipod_scaler_set_iommu(sysbus_create_simple("ipodtouch.scaler", S5L8920_SCALER_BASE,
                                               s5l8920_irq(s, S5L8920_IRQ_SCALER)),
                          s5l8920_dart_xlate, dart0, 0);
    sysbus_create_simple("ipodtouch.swi", S5L8920_SWI_BASE, NULL);

    /*
     * TV-out: the S5L8720's SDO and mixers (DT tv-out reg 0x5600000,
     * 0x5200000, 0x5100000; interrupts 0x23, 0x27). SpringBoard's power-off
     * swaps every framebuffer, AppleM2TVOut's too, and waits for each: with
     * TV-out unmodelled its swap never completed and the guest never halted.
     */
    dev = qdev_new("ipodtouch.tvout");
    memory_region_add_subregion(sysmem, 0x85600000, &IPOD_TOUCH_TVOUT(dev)->sdo_iomem);
    memory_region_add_subregion(sysmem, 0x85200000, &IPOD_TOUCH_TVOUT(dev)->mixer1_iomem);
    memory_region_add_subregion(sysmem, 0x85100000, &IPOD_TOUCH_TVOUT(dev)->mixer2_iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s5l8920_irq(s, 0x23));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, s5l8920_irq(s, 0x27));

    /*
     * I2S0, the CS42L58 codec's port: the A4's controller (registers at
     * +0x400, as AppleS5L8920XI2SController writes them), its TX FIFO at
     * the block's base where CDMA channel 0x15 streams PCM. Without it
     * mediaserverd's stop path never sees the FIFO drain.
     */
    dev = qdev_new(TYPE_S5L8930_I2S);
    qdev_prop_set_bit(dev, "audio-out", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S5L8920_I2S0_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, S5L8920_I2S0_FIFO);

    /* USB device mode: the S5L8720's DWC OTG core and PHY; the built-in host enumerates it. */
    dev = qdev_new(TYPE_IPOD_TOUCH_USB_PHYS);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S5L8920_USB_PHY_BASE);
    {
        static uint32_t hwcfg[] = { 0, 0x7a8f60d0, 0x082000e8, 0x01f08024 };

        dev = ipod_touch_init_usb_otg(s5l8920_irq(s, S5L8920_IRQ_USB_OTG), hwcfg);
        s->usb_otg = S5L8900USBOTG(dev);
        synopsys_usb_set_tcp_addr(s->usb_otg, s->usb_tcp_addr);
        s->usb_otg->builtin_host = !s->usb_otg->server_host && !getenv("IT_USB_TCP");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        memory_region_add_subregion(sysmem, S5L8920_USB_OTG_BASE, &s->usb_otg->iomem);
    }

    s->pwroff_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, s5l8920_pwroff_tick, s);
    qemu_register_powerdown_notifier(&s5l8920_powerdown_notifier);
    qemu_register_reset(s5l8920_cpu_reset, s);
}

static char *s5l8920_get_kboot(Object *obj, Error **errp)
{
    return g_strdup(S5L8920_MACHINE(obj)->kboot_path);
}

static void s5l8920_set_kboot(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    g_free(s->kboot_path);
    s->kboot_path = g_strdup(value);
}

static char *s5l8920_get_nand(Object *obj, Error **errp)
{
    return g_strdup(S5L8920_MACHINE(obj)->nand_path);
}

static void s5l8920_set_nand(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    g_free(s->nand_path);
    s->nand_path = g_strdup(value);
}

static char *s5l8920_get_nand_overlay(Object *obj, Error **errp)
{
    return g_strdup(S5L8920_MACHINE(obj)->nand_overlay_path);
}

static void s5l8920_set_nand_overlay(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    g_free(s->nand_overlay_path);
    s->nand_overlay_path = g_strdup(value);
}

static char *s5l8920_get_nor(Object *obj, Error **errp)
{
    return g_strdup(S5L8920_MACHINE(obj)->nor_path);
}

static void s5l8920_set_nor(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    g_free(s->nor_path);
    s->nor_path = g_strdup(value);
}

static char *s5l8920_get_nor_rw(Object *obj, Error **errp)
{
    return g_strdup(S5L8920_MACHINE(obj)->nor_rw_path);
}

static void s5l8920_set_nor_rw(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    g_free(s->nor_rw_path);
    s->nor_rw_path = g_strdup(value);
}

/*
 * Buttons: GPIO interrupt pins, active low, idle high in the GPIO model
 * (DT function-button_hold 0x1607 = interrupt 0xb7, menu 0x1606 = 0xb6).
 * ponytail: the GPIO side only; the PMU's wake latch (DT wake_button_* on
 * the D1755's STAT) is not driven, so a press cannot wake a sleeping AP.
 *   qom-set path=/machine property=button-home value=true   (then false)
 */
static void s5l8920_set_button(S5L8920MachineState *s, int pin, bool down)
{
    qemu_set_irq(qdev_get_gpio_in(s->gpio, pin), !down);
}

static bool s5l8920_get_button_hold(Object *obj, Error **errp)
{
    return S5L8920_MACHINE(obj)->btn_hold;
}

static void s5l8920_set_button_hold(Object *obj, bool value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    s->btn_hold = value;
    s5l8920_set_button(s, s->board->buttons.hold, value);
}

static bool s5l8920_get_button_home(Object *obj, Error **errp)
{
    return S5L8920_MACHINE(obj)->btn_home;
}

static void s5l8920_set_button_home(Object *obj, bool value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    s->btn_home = value;
    s5l8920_set_button(s, s->board->buttons.menu, value);
}

/*
 * QMP system_powerdown -> the user's power-off gesture, as on the iPod and
 * iPad machines: Home (wakes the panel), hold Hold past SpringBoard's
 * threshold, then drag "slide to power off". Guest time throughout. The
 * guest then unmounts, syncs the FTL and ends in the PMU's power command,
 * where QEMU exits (pcf50633's shutdown-reg / standby write).
 */
enum { PWROFF_IDLE, PWROFF_HOME, PWROFF_WAKE, PWROFF_HOLD, PWROFF_SETTLE, PWROFF_DRAG };
#define PWROFF_DRAG_STEPS 24

static void s5l8920_pwroff_arm(S5L8920MachineState *s, int ms)
{
    timer_mod(s->pwroff_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)ms * SCALE_MS);
}

static void s5l8920_pwroff_touch(S5L8920MachineState *s, int x, int y, bool down)
{
    ipod_touch_multitouch_set_finger(s->mt, 0, x / 319.0f, 1.0f - y / 479.0f, down);
}

static void s5l8920_pwroff_tick(void *opaque)
{
    S5L8920MachineState *s = opaque;
    const S5L8920PowerKnob *k = &s->board->pwroff_knob;

    switch (s->pwroff_phase) {
    case PWROFF_HOME:
        s5l8920_set_button(s, s->board->buttons.menu, false);
        s->pwroff_phase = PWROFF_WAKE;
        s5l8920_pwroff_arm(s, 2000);
        break;
    case PWROFF_WAKE:
        s5l8920_set_button(s, s->board->buttons.hold, true);
        s->pwroff_phase = PWROFF_HOLD;
        s5l8920_pwroff_arm(s, 3500);
        break;
    case PWROFF_HOLD:
        s5l8920_set_button(s, s->board->buttons.hold, false);
        s->pwroff_phase = PWROFF_SETTLE;
        s5l8920_pwroff_arm(s, 1500);
        break;
    case PWROFF_SETTLE:
        s5l8920_pwroff_touch(s, k->x, k->y, true);
        s->pwroff_phase = PWROFF_DRAG;
        s->pwroff_step = 0;
        s5l8920_pwroff_arm(s, 80);
        break;
    case PWROFF_DRAG:
        s->pwroff_step++;
        s5l8920_pwroff_touch(s, k->x + k->drag * s->pwroff_step / PWROFF_DRAG_STEPS, k->y,
                             s->pwroff_step < PWROFF_DRAG_STEPS);
        if (s->pwroff_step < PWROFF_DRAG_STEPS) {
            s5l8920_pwroff_arm(s, 80);
        } else {
            s->pwroff_phase = PWROFF_IDLE;
        }
        break;
    }
}

static void s5l8920_powerdown_req(Notifier *n, void *opaque)
{
    S5L8920MachineState *s = S5L8920_MACHINE(qdev_get_machine());

    if (s->pwroff_phase != PWROFF_IDLE) {
        return;
    }
    s5l8920_set_button(s, s->board->buttons.menu, true);
    s->pwroff_phase = PWROFF_HOME;
    s5l8920_pwroff_arm(s, 300);
}

static Notifier s5l8920_powerdown_notifier = { .notify = s5l8920_powerdown_req };

static char *s5l8920_get_usb_tcp_addr(Object *obj, Error **errp)
{
    return g_strdup(S5L8920_MACHINE(obj)->usb_tcp_addr);
}

static void s5l8920_set_usb_tcp_addr(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    g_free(s->usb_tcp_addr);
    s->usb_tcp_addr = g_strdup(value);
}

static void s5l8920_instance_init(Object *obj)
{
    guest_pkg_init(&S5L8920_MACHINE(obj)->pkg, obj);
}

static void s5l8920_instance_finalize(Object *obj)
{
    g_free(S5L8920_MACHINE(obj)->usb_tcp_addr);
    g_free(S5L8920_MACHINE(obj)->nor_path);
    g_free(S5L8920_MACHINE(obj)->nor_rw_path);
    g_free(S5L8920_MACHINE(obj)->kboot_path);
    g_free(S5L8920_MACHINE(obj)->nand_path);
    g_free(S5L8920_MACHINE(obj)->nand_overlay_path);
}

static bool s5l8920_get_gles_debug(Object *obj, Error **errp)
{
    return S5L8920_MACHINE(obj)->gles_debug;
}

static void s5l8920_set_gles_debug(Object *obj, bool value, Error **errp)
{
    S5L8920_MACHINE(obj)->gles_debug = value;
    gles_host_set_debug(value);
}

static char *s5l8920_get_gles_rejects(Object *obj, Error **errp)
{
    return gles_host_rejects();
}

static void s5l8920_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->init = s5l8920_init;
    mc->max_cpus = 2;        /* the AP and the IOP core */
    mc->default_cpus = 2;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a8");
    mc->default_ram_size = 0x10000000;
    object_class_property_add_str(klass, "kboot", s5l8920_get_kboot, s5l8920_set_kboot);
    object_class_property_set_description(klass, "kboot",
        "K48KBOOT bundle from imgtools/s5l8920_kboot.py");
    object_class_property_add_str(klass, "nand", s5l8920_get_nand, s5l8920_set_nand);
    object_class_property_set_description(klass, "nand",
        "NAND page-store directory (geometry.json + bus<b>-ce<c>.pages); blank chips if unset");
    object_class_property_add_str(klass, "nand-overlay", s5l8920_get_nand_overlay,
                                  s5l8920_set_nand_overlay);
    object_class_property_set_description(klass, "nand-overlay",
        "Copy-on-write directory for guest NAND writes; the nand store is then read-only");
    object_class_property_add_bool(klass, "button-hold", s5l8920_get_button_hold,
                                   s5l8920_set_button_hold);
    object_class_property_set_description(klass, "button-hold", "Hold/power button pressed; set true then false");
    object_class_property_add_bool(klass, "button-home", s5l8920_get_button_home,
                                   s5l8920_set_button_home);
    object_class_property_set_description(klass, "button-home", "Home button pressed; set true then false");
    object_class_property_add_str(klass, "usb-tcp-addr", s5l8920_get_usb_tcp_addr,
                                  s5l8920_set_usb_tcp_addr);
    object_class_property_set_description(klass, "usb-tcp-addr",
        "usbmuxd-qemu host bridge host:port; unset = IT_USB_TCP or the built-in host");
    object_class_property_add_str(klass, "nor", s5l8920_get_nor, s5l8920_set_nor);
    object_class_property_set_description(klass, "nor",
        "1 MiB SPI NOR image on spi0 (nvram, effaceable); no NOR if neither this nor nor-rw is set");
    object_class_property_add_str(klass, "nor-rw", s5l8920_get_nor_rw, s5l8920_set_nor_rw);
    object_class_property_set_description(klass, "nor-rw",
        "1 MiB private writable NOR copy; guest writes (effaceable) persist here across boots");
    object_class_property_add_bool(klass, "gles-debug", s5l8920_get_gles_debug, s5l8920_set_gles_debug);
    object_class_property_set_description(klass, "gles-debug",
        "Paint what the GL bridge refuses magenta (tests)");
    object_class_property_add_str(klass, "gles-rejects", s5l8920_get_gles_rejects, NULL);
    object_class_property_set_description(klass, "gles-rejects",
        "Every GL bridge refusal so far, NAME<TAB>COUNT per line");
}

static const TypeInfo s5l8920_machine_info = {
    .name = TYPE_S5L8920_MACHINE,
    .parent = TYPE_MACHINE,
    .abstract = true,
    .instance_size = sizeof(S5L8920MachineState),
    .class_size = sizeof(S5L8920MachineClass),
    .instance_init = s5l8920_instance_init,
    .instance_finalize = s5l8920_instance_finalize,
    .class_init = s5l8920_class_init,
};

/* One machine type per board: -M n18, -M n88. */
static void s5l8920_board_class_init(ObjectClass *klass, void *data)
{
    const S5L8920Board *board = data;

    S5L8920_MACHINE_CLASS(klass)->board = board;
    MACHINE_CLASS(klass)->desc = board->desc;
}

static const TypeInfo s5l8920_board_types[] = {
    {
        .name = MACHINE_TYPE_NAME("n18"),
        .parent = TYPE_S5L8920_MACHINE,
        .class_init = s5l8920_board_class_init,
        .class_data = (void *)&s5l8920_n18,
    }, {
        .name = MACHINE_TYPE_NAME("n88"),
        .parent = TYPE_S5L8920_MACHINE,
        .class_init = s5l8920_board_class_init,
        .class_data = (void *)&s5l8920_n88,
    },
};

static void s5l8920_machine_types(void)
{
    type_register_static(&s5l8920_machine_info);
    for (int i = 0; i < ARRAY_SIZE(s5l8920_board_types); i++) {
        type_register_static(&s5l8920_board_types[i]);
    }
}

type_init(s5l8920_machine_types)
