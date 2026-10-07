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
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/misc/unimp.h"
#include "hw/misc/ios_baseband.h"
#include "hw/core/split-irq.h"
#include "hw/arm/ipod_touch_pke.h"
#include "hw/arm/ipod_touch_sdio.h"
#include "net/net.h"
#include "qemu/config-file.h"
#include "qemu/option.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "hw/arm/ipod_touch_tvout.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/arm/ipod_touch_buttons.h"
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
#include "hw/arm/ipod_touch_2g.h"
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
#include "hw/arm/guest-pasteboard.h"
#include "hw/arm/ipod-agent.h"
#include "qemu/guest-random.h"

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
#define S5L8920_SDIO_BASE       0x80000000      /* SDHC, sdio,s5l8920x */
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
#define S5L8920_IRQ_SDIO        0x22
#define S5L8920_IRQ_DART(n)     (0x5a - (n))
#define S5L8920_IRQ_CDMA(ch)    (0x2a + (ch))   /* DT lists channels 1.. from 0x2b */

typedef struct S5L8920I2CDevice {
    uint8_t bus;                         /* i2c0 or i2c2 */
    uint8_t addr;
    const char *type;
    int16_t irq_pin;                     /* GPIO interrupt its gpio-out 0 drives, active low; 0 = none */
} S5L8920I2CDevice;

/*
 * DT buttons interrupts: GPIO interrupt numbers. Active low unless the
 * button's DT function-button_* word has flag 0x100 (N88 hold and menu,
 * which also take both edges, interrupt type 7).
 */
typedef struct S5L8920Buttons {
    uint16_t hold, menu, volup, voldown;
    bool hold_menu_high;                 /* hold and menu active high */
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
    int8_t bt_uart;                      /* the BCM4325's H4 HCI (ipod_touch_bt.c); 0 = none */
    uint16_t gauge_mah;
    bool nor;                            /* a SPI NOR on spi0 (the N88's; the N18 boots from NAND) */
    bool baseband;                       /* spi2, the baseband link */
    /* The fake modem on spi2 (baseband=on; docs/baseband/): IFX protocol version, DT
     * max-data-size, MRDY (AP out) / SRDY (AP in) and radio_on / bb_rst GPIOs. */
    uint8_t bb_ifx;
    uint16_t bb_max_data;
    uint16_t bb_mrdy, bb_srdy, bb_radio_on, bb_rst;
    const char *bb_compat;               /* the DT baseband node's compatible, to re-match it */
    bool no_isp;                         /* unmatch the DT's isp node: there is no ISP model */
    uint32_t fmc_off;                    /* FMC within each FMI window, as its IOP firmware addresses it */
    uint16_t mt_atn;                     /* multi-touch ATN, a GPIO interrupt */
    const MTSensorProfile *mt_profile;   /* the sensor the multitouch model reports */
    S5L8920Buttons buttons;
    /* The ring/silent switch (DT function-button_ringerab): its GPIO and which level is silent
     * (the function's flags 0x100: active high). 0: none (the iPod). */
    uint16_t ringer;
    bool ringer_active_high;
    S5L8920I2CDevice i2c[8];             /* in creation order (the snapshot's) */
    S5L8920PowerKnob pwroff_knob;
    const char *wifi_board;              /* the card's CIS VERS_1 board string; NULL = no Wi-Fi card */
    const char *wifi_fw_version;
    uint8_t wifi_mac[6];
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
    .mt_profile = &mt_profile_n81,       /* the N81's N1F55 sensor */
    .buttons = { .hold = 0xb7, .menu = 0xb6, .volup = 0xb0, .voldown = 0xb1 },
    .i2c = {
        /* ponytail: the D1755 PMU as the iPod 2G's D1759 register model. */
        { 0, 0x74, TYPE_PCF50633, 0x9d },
        { 0, 0x4a, TYPE_CS42L58 },
        { 0, 0x3a, TYPE_CD3272MIKEY },
        { 2, 0x1d, TYPE_LIS302DL },       /* its DT interrupt (0xa2) is not driven */
    },
    .pwroff_knob = { 57, 67, 240 },      /* off a 4.2.1 screendump of the sheet */
    /* A BCM4329 B1: AppleBCMWLAN's "N18 - 4329 B1" (s=B1, P=N18 -> 4329b1/n18.bin, whose version this is). */
    .wifi_board = "P=N18",
    .wifi_fw_version = "wl0: Oct 13 2010 15:39:53 version 4.221.38.1",
    .wifi_mac = { 0x02, 0x00, 0x00, 0x18, 0x00, 0x01 },  /* synthetic, locally administered */
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
    .bt_uart = 3,                        /* uart3/bluetooth,n88 */
    .gauge_mah = 1219,                   /* ponytail: the 3GS's rated cell, not measured */
    .nor = true,
    .baseband = true,
    .bb_ifx = 1, .bb_max_data = 0x7f8,   /* DT spi2 protocol-version, max-data-size */
    .bb_mrdy = 0x1802, .bb_srdy = 0x1304, .bb_radio_on = 0x1405, .bb_rst = 0x1407,
    .bb_compat = "baseband,n88",
    .no_isp = true,
    .mt_atn = 0xb4,
    .mt_profile = &mt_profile_n88,       /* N1F54 */
    .buttons = { .hold = 0xb7, .menu = 0xb6, .volup = 0xb0, .voldown = 0xb1, .hold_menu_high = true },
    .ringer = 0x1403,                    /* function-button_ringerab flags 0: active low (the N90's is high) */
    .i2c = {
        { 0, 0x74, TYPE_PCF50633, 0x9d },
        { 0, 0x4a, TYPE_CS42L58 },       /* cs42l61: a register file to its driver, as on the iPad */
        { 0, 0x39, TYPE_CD3272MIKEY },
        { 0, 0x1d, TYPE_LIS302DL },
        { 0, 0x1e, TYPE_S5L8930_AK8973 },
        { 2, 0x49, TYPE_S5L8930_TSL2561 },
    },
    .pwroff_knob = { 57, 67, 240 },
};

#define TYPE_S5L8920_MACHINE "s5l8920-machine"
OBJECT_DECLARE_TYPE(S5L8920MachineState, S5L8920MachineClass, S5L8920_MACHINE)
static void s5l8920_apply_ring_switch(S5L8920MachineState *s);

struct S5L8920MachineClass {
    MachineClass parent;
    const S5L8920Board *board;
};

struct S5L8920MachineState {
    MachineState parent;
    const S5L8920Board *board;
    ARMCPU *cpu;
    MemoryRegion dram, dram_hi, dram_lo, sram, chipid, cpu_debug;
    DeviceState *vic[S5L8920_VIC_COUNT];
    DeviceState *gpio;
    DeviceState *iopcore;
    IPodTouchMultitouchState *mt;
    synopsys_usb_state *usb_otg;
    Pcf50633State *pmu;
    LIS302DLState *accel;
    char *kboot_path;
    uint32_t panel_w, panel_h;           /* "panel=WxH", issue #21; 0 = the 320x480 panel */
    char *nand_path;
    char *nand_overlay_path;
    char *nor_path;
    char *nor_rw_path;
    char *die_id;                        /* ChipID words 2-3 of the unit, hex pair */
    char *usb_tcp_addr;                  /* usbmuxd-qemu host bridge; empty = the built-in host */
    bool btn_hold, btn_home;             /* button-hold/-home properties */
    bool ring_silent;                    /* ring-switch: the side switch at silent (default off: ring) */
    QEMUBH *bt_kick;                     /* resumes the CDMA receive from the HCI's UART */
    bool gles_debug;                     /* paint what the GL bridge refuses magenta (tests) */
    bool wifi;                           /* bridge the Wi-Fi card to -netdev id=wifi0 (default on) */
    bool usb_attached;                   /* usb-attached (default on) */
    int battery_level;                   /* battery-level, -1 = the PMU model's own */
    unsigned battery_charging;           /* battery-charging: 0 auto, 1 on, 2 off */
    bool baseband_on;                    /* baseband=on: the fake modem behind spi2 */
    char *imei;                          /* the unit's IMEI, for that modem to report */
    DeviceState *bb_modem;
    GuestPackage pkg;                    /* hw/arm/guest-package.c: it_boot and the GL shim's hello */
    GuestPasteboard pb;                  /* hw/arm/guest-pasteboard.c */
    IPodAgent *agent;                    /* hw/arm/ipod-agent.c: it_agent, as on the iPod and iPad */
    QEMUTimer *pwroff_timer;             /* system_powerdown gesture */
    int pwroff_phase, pwroff_step, pwroff_tries;
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
    ipod_agent_reset(s->agent);
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
    if (s->bb_modem) {
        /*
         * kboot (s5l8920_kboot.py, fill_dt) unmatches and renames the baseband node
         * ("nobb"); with the modem attached, give it back its name and compatible.
         * lockdownd compares the DT's IMEI with the modem's +CGSN (iBoot fills it on
         * hardware), so that comes from the modem too.
         */
        g_autofree char *imei = object_property_get_str(OBJECT(s->bb_modem), "imei", &error_abort);
        const A4DTEdit edits[] = {
            { "nobb", "name", "baseband", sizeof("baseband") },
            { "baseband", "device_type", "baseband", sizeof("baseband") },
            { "baseband", "compatible", s->board->bb_compat, strlen(s->board->bb_compat) + 1 },
            { "baseband", "device-imei", imei, strlen(imei) },
        };

        for (int i = 0; i < ARRAY_SIZE(edits); i++) {
            a4_dt_edit((uint8_t *)data, image_len, load_pa, bootargs_pa, &edits[i]);
        }
    }
    /*
     * No ISP model: AppleH2CamIn loads the ISP CPU's firmware and then waits on
     * its mailbox (+0x13c bit 30), which nothing answers. 4.x gives up after a
     * few 2 s waits; 3.1.3's ISP_waitCommunicationEnd resets its count and
     * busy-waits (IODelay) forever, which starved SpringBoard: Home took ~20 s
     * to light the panel. Unmatched, the board reads as camera-less, as the A4
     * machines do by default (ipad1.c).
     */
    if (s->board->no_isp) {
        a4_dt_edit((uint8_t *)data, image_len, load_pa, bootargs_pa,
                   &(A4DTEdit){ "isp", "compatible", "none", 5 });
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
            qdev_prop_set_uint8(DEVICE(slave), "brick-mux", 6);   /* the dock data lines, as the D1815's */
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
            s->pmu->usb_cable = s->usb_attached;
        } else if (!strcmp(d->type, TYPE_LIS302DL)) {
            s->accel = LIS302DL(slave);
            /* the iPod/iPad machines' names, which the app's tilt and shake set */
            object_property_add_alias(OBJECT(s), "accel-orientation", OBJECT(slave), "orientation");
            object_property_add_alias(OBJECT(s), "accel-x", OBJECT(slave), "x");
            object_property_add_alias(OBJECT(s), "accel-y", OBJECT(slave), "y");
            object_property_add_alias(OBJECT(s), "accel-z", OBJECT(slave), "z");
            object_property_add_alias(OBJECT(s), "accel-shake", OBJECT(slave), "shake");
        }
    }
}

/*
 * The guest-services trap (mcr p15,3,Rn,c15,c15,0) the GLES shim
 * (contrib/gles-public) uses, as ipad1's: GLES, guest packages (which also
 * answer the shim's hello), the pasteboard and the guest agent (it_agent).
 */
static int s5l8920_agent_copy(void *opaque, uint32_t address, uint8_t *data, size_t length, bool write)
{
    if (length && length - 1 > UINT32_MAX - address) {
        return -1;
    }
    return cpu_memory_rw_debug(opaque, address, data, length, write);
}

static void s5l8920_qemu_call(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    CPUState *cs = env_cpu(env);
    S5L8920MachineState *s = S5L8920_MACHINE(qdev_get_machine());
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
    case QC_AG_HELLO:
    case QC_AG_POLL:
    case QC_AG_READ:
    case QC_AG_WRITE:
    case QC_AG_DONE:
    case QC_AG_HOSTTIME:
    case QC_UI_POLL:
    case QC_UI_READ:
    case QC_UI_WRITE:
    case QC_UI_DONE:
    case QC_AG_UI_ROUTE: {
        uint64_t candidate = 0;
        if (q.call_number == QC_AG_HELLO || q.call_number == QC_AG_UI_ROUTE) {
            qemu_guest_getrandom_nofail(&candidate, sizeof(candidate));
        }
        q.retval = ipod_agent_call(s->agent, q.call_number, q.args.ag.token, q.args.ag.buffer_guest_ptr,
                                   q.args.ag.offset, q.args.ag.length, qemu_clock_get_ms(QEMU_CLOCK_REALTIME),
                                   candidate, s5l8920_agent_copy, cs);
        err = q.retval < 0 ? EINVAL : 0;
        break;
    }
    default:
        if (!guest_pb_call(&s->pb, cs, &q, &err) && !guest_pkg_call(&s->pkg, cs, &q, &err)) {
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

/* The pacing source over a UART's URXH: what its receive FIFO holds (UFSTAT). */
static uint32_t s5l8920_uart_rx_avail(void *opaque, hwaddr addr, bool to_device)
{
    uint32_t f = address_space_ldl_le(&address_space_memory, (addr & ~0xffull) + 0x18,
                                      MEMTXATTRS_UNSPECIFIED, NULL);

    return to_device ? 0 : (f & 0x100) ? 256 : (f & 0xff);
}

/* Off the UART's receive path: the CDMA reads URXH, which re-enters the UART. */
static void s5l8920_bt_kick(void *opaque)
{
    s5l8930_cdma_kick(DEVICE(opaque));
}

static void s5l8920_bt_rxdma(void *opaque, int n, int level)
{
    S5L8920MachineState *s = opaque;

    if (level) {
        qemu_bh_schedule(s->bt_kick);
    }
}

static void s5l8920_init(MachineState *machine)
{
    S5L8920MachineState *s = S5L8920_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    DeviceState *dev, *iop, *bt_uart = NULL;
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
    /*
     * PA 0 is DRAM's first page, as on the A4 machines (ipad1.c). iOS 6
     * (xnu-2107) links at 0x80001000, leaves that page out, and copies its
     * reset and exception vectors to ml_vtophys(gPhysBase) = PA 0 once the
     * V=P mapping is gone; unmapped, the copy is an external abort before the
     * console. ponytail: one page, aliased; the real remap's size is unmeasured.
     */
    memory_region_init_alias(&s->dram_lo, NULL, "s5l8920.dram-lo", &s->dram, 0, 0x1000);
    memory_region_add_subregion(sysmem, 0, &s->dram_lo);

    {
        uint32_t chipid[] = { s->board->chipid[0], s->board->chipid[1], 0, 0 };

        /* the unit's die-id words, as ipad1's die-id property */
        if (s->die_id && sscanf(s->die_id, "%" SCNx32 ":%" SCNx32, &chipid[2], &chipid[3]) != 2) {
            error_report("s5l8920: die-id must be \"0xWORD2:0xWORD3\", got \"%s\"", s->die_id);
            exit(1);
        }

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
    s5l8920_apply_ring_switch(s);

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
        if (i == s->board->bt_uart) {
            chr = it_bt_chardev(chr, true, 2000);
        }
        dev = exynos4210_uart_create(S5L8920_UART_BASE(i), fifo, i, chr,
                                     s5l8920_irq(s, S5L8920_IRQ_UART(i)), true);
        if (s->board->bt_uart && i == s->board->bt_uart) {
            bt_uart = dev;
        }
    }

    /*
     * Wi-Fi, as on the iPad (docs/ipad1/wifi.md): the iPod's Broadcom dongle
     * model dressed as the board's chip, behind the SDHC at 0x80000000, which
     * the IOP firmware's sdiodrv drives.
     */
    DeviceState *sdio = NULL;
    if (s->board->wifi_board) {
        BCMSDIOChip bcm4329 = {
            .manfid = 0x02d0, .prodid = 0x4329,
            .chipid = 0x00034329,                   /* rev 3 = B1 */
            .sdiod_base = 0x18011000,
            .vers1 = { "", "", "s=B1", s->board->wifi_board },
            .no_common_funce = true,
            .fw_version = s->board->wifi_fw_version,
        };
        IPodTouchSDIOState *card = IPOD_TOUCH_SDIO(qdev_new(TYPE_IPOD_TOUCH_SDIO));

        memcpy(bcm4329.mac, s->board->wifi_mac, sizeof(bcm4329.mac));
        ipod_touch_sdio_set_chip(card, &bcm4329);
        object_property_add_alias(OBJECT(machine), "wifi-bssid", OBJECT(card), "bssid");
        card->card_present = true;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(card), &error_fatal);
        if (s->wifi && !qemu_find_netdev("wifi0")) {
            /* no backend given: NAT it */
            QemuOpts *o = qemu_opts_parse_noisily(qemu_find_opts("netdev"), "type=user,id=wifi0", false);
            Error *err = NULL;
            if (o) {
                netdev_add(o, &err);
            }
            if (err) {
                warn_reportf_err(err, "Wi-Fi has no network: ");
            }
        }
        if (s->wifi) {
            ipod_touch_sdio_setup_net(card);
        }
        sdio = qdev_new(TYPE_S5L8930_SDIO);
        object_property_set_link(OBJECT(sdio), "card", OBJECT(card), &error_fatal);
        sbd = SYS_BUS_DEVICE(sdio);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, S5L8920_SDIO_BASE);
        sysbus_connect_irq(sbd, 0, s5l8920_irq(s, S5L8920_IRQ_SDIO));
        sysbus_connect_irq(SYS_BUS_DEVICE(card), 0, qdev_get_gpio_in(sdio, 0));
    }

    /* AP side of the IOP; NAND pages come from its page store. */
    dev = qdev_new(TYPE_S5L8930_IOP);
    iop = dev;
    if (sdio) {
        object_property_set_link(OBJECT(dev), "sdio", OBJECT(sdio), &error_fatal);
    }
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
    if (bt_uart) {
        /*
         * The serial driver receives the HCI by CDMA from URXH (DT dma-channels,
         * channel 0xd). Pace that chain by the UART's receive FIFO and resume it
         * on the UART's Rx DMA request; without anything behind the port,
         * 3.1.3's BlueTool never got its HCI_Reset answered, BTServer never came
         * up, and every BTSessionAttach of SpringBoard's (four on a Home wake)
         * cost its 5 s timeout.
         */
        hwaddr urxh = S5L8920_UART_BASE(s->board->bt_uart) + 0x24;

        s5l8930_cdma_set_source(dev, urxh, 4, s5l8920_uart_rx_avail, NULL);
        s->bt_kick = qemu_bh_new(s5l8920_bt_kick, dev);
        sysbus_connect_irq(SYS_BUS_DEVICE(bt_uart), 2,
                           qemu_allocate_irq(s5l8920_bt_rxdma, s, 0));
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
            qdev_prop_set_bit(dev, "cfg-v0", true);
        }
        /* A transfer starts with its control write on every firmware these boards run: the s5l8920x IOP
         * firmware's, and 3.1.3's s5l8922x one, which fills the FIFO for the next page before writing
         * control 5 (without it that page completes onto the previous chip). 4.2.1's s5l8922x firmware
         * is indifferent: afc + persist pass either way. */
        qdev_prop_set_bit(dev, "explicit-start", true);
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
    /*
     * SPI2: the baseband link (N88). baseband=on puts the fake modem behind it
     * (hw/misc/ios_baseband*.c, hw/arm/s5l8930_bbspi.c); otherwise a controller
     * with nothing on it. Its DT interrupt is the SRDY GPIO.
     */
    if (s->board->baseband && s->baseband_on && s->board->bb_ifx) {
        DeviceState *bb = qdev_new(TYPE_IOS_BASEBAND);

        qdev_prop_set_int32(bb, "ifx-version", s->board->bb_ifx);
        qdev_prop_set_int32(bb, "ifx-max-data", s->board->bb_max_data);
        object_property_add_child(OBJECT(s), "baseband-modem", OBJECT(bb));
        if (s->imei && s->imei[0]) {
            object_property_set_str(OBJECT(bb), "imei", s->imei, &error_fatal);
        }
        qdev_realize_and_unref(bb, NULL, &error_fatal);
        s->bb_modem = bb;
        dev = qdev_new(TYPE_IOS_BASEBAND_SPI);
        object_property_set_link(OBJECT(dev), "modem", OBJECT(bb), &error_abort);
        object_property_set_link(OBJECT(dev), "cdma",
            object_resolve_path_type("", TYPE_S5L8930_CDMA, NULL), &error_abort);
        qdev_prop_set_uint32(dev, "base", S5L8920_SPI_BASE(2));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S5L8920_SPI_BASE(2));
        qdev_connect_gpio_out(s->gpio, S5L8930_GPIO_PIN(s->board->bb_mrdy),
                              qdev_get_gpio_in_named(bb, "mrdy", 0));
        qdev_connect_gpio_out_named(bb, "srdy", 0,
            qdev_get_gpio_in(s->gpio, S5L8930_GPIO_PIN(s->board->bb_srdy)));
        qdev_connect_gpio_out(s->gpio, S5L8930_GPIO_PIN(s->board->bb_radio_on),
                              qdev_get_gpio_in_named(bb, "ctl", 0));
        qdev_connect_gpio_out(s->gpio, S5L8930_GPIO_PIN(s->board->bb_rst),
                              qdev_get_gpio_in_named(bb, "ctl", 1));
    } else if (s->board->baseband) {
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
    s->mt->profile = s->board->mt_profile;
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
    if (s->panel_w) {
        qdev_prop_set_uint32(dev, "panel-width", s->panel_w);
        qdev_prop_set_uint32(dev, "panel-height", s->panel_h);
    }
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
    qdev_prop_set_bit(dev, "ctrl-run", true);   /* AppleS5L8920XI2SController starts TX with CTRL bit 0 */
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
    const S5L8920Buttons *b = &s->board->buttons;
    bool high = b->hold_menu_high && (pin == b->hold || pin == b->menu);

    qemu_set_irq(qdev_get_gpio_in(s->gpio, pin), high ? down : !down);
}

/* The app bridge's buttons (contrib/ios-app), on the board's pins; no-op on other machines. */
void s5l8920_press_button(IPodTouchButton button, bool down)
{
    S5L8920MachineState *s = (S5L8920MachineState *)
        object_dynamic_cast(OBJECT(qdev_get_machine()), TYPE_S5L8920_MACHINE);
    const S5L8920Buttons *b;

    if (!s) {
        return;
    }
    b = &s->board->buttons;
    switch (button) {
    case IPOD_TOUCH_BUTTON_HOME:    s5l8920_set_button(s, b->menu, down); break;
    case IPOD_TOUCH_BUTTON_POWER:   s5l8920_set_button(s, b->hold, down); break;
    case IPOD_TOUCH_BUTTON_VOLUP:   s5l8920_set_button(s, b->volup, down); break;
    case IPOD_TOUCH_BUTTON_VOLDOWN: s5l8920_set_button(s, b->voldown, down); break;
    }
}

bool ipod_touch_mipi_dsi_panel_off(void);   /* hw/arm/ipod_touch_mipi_dsi.c */

/* The guest's power-off command reached the PMU: 5.x's halt then restarts through it, so this, not a
 * QEMU SHUTDOWN event, is the evidence (as ipad1's D1815). */
static bool s5l8920_get_guest_shutdown_confirmed(Object *obj, Error **errp)
{
    return pcf50633_guest_shutdown_confirmed();
}

static bool s5l8920_get_display_sleeping(Object *obj, Error **errp)
{
    return ipod_touch_mipi_dsi_panel_off();
}

/* The ring/silent switch's pad at the level its position and polarity give; the GPIO keeps it across
 * resets, and both edges interrupt, so a flip at run time reaches the guest. */
static void s5l8920_apply_ring_switch(S5L8920MachineState *s)
{
    if (s->gpio && s->board->ringer) {
        s5l8930_gpio_set_rest_level(s->gpio, S5L8930_GPIO_PIN(s->board->ringer),
                                    s->ring_silent == s->board->ringer_active_high);
    }
}

/* After every device reset (the GPIO model's puts every input high): buttons at rest. */
static void s5l8920_machine_reset(MachineState *machine, ResetType type)
{
    S5L8920MachineState *s = S5L8920_MACHINE(machine);

    qemu_devices_reset(type);
    s5l8920_set_button(s, s->board->buttons.hold, s->btn_hold);
    s5l8920_set_button(s, s->board->buttons.menu, s->btn_home);
    if (s->pmu) {   /* the charge and level the properties asked for survive the PMU's reset */
        pcf50633_set_charging_mode(s->pmu, s->battery_charging);
        if (s->battery_level >= 0) {
            pcf50633_set_battery_level(s->pmu, s->battery_level);
        }
    }
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
enum { PWROFF_IDLE, PWROFF_HOME, PWROFF_WAKE, PWROFF_HOLD, PWROFF_SETTLE, PWROFF_DRAG, PWROFF_LIFT };
#define PWROFF_DRAG_STEPS 24
#define PWROFF_DRAG_TRIES 3

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
        s5l8920_pwroff_touch(s, k->x + k->drag * s->pwroff_step / PWROFF_DRAG_STEPS, k->y, true);
        if (s->pwroff_step < PWROFF_DRAG_STEPS) {
            s5l8920_pwroff_arm(s, 80);
        } else {
            /* Rest at the end before lifting: a busy guest can miss the moves
             * and see only a lift at the far end. */
            s->pwroff_phase = PWROFF_LIFT;
            s5l8920_pwroff_arm(s, 500);
        }
        break;
    case PWROFF_LIFT:
        s5l8920_pwroff_touch(s, k->x + k->drag, k->y, false);
        /* 3.1.1 (software-drawn sheet) sometimes leaves the knob where the
         * touch began, as a user's missed slide does; slide again. Once the
         * guest is shutting down the sheet is gone and a drag does nothing. */
        if (++s->pwroff_tries < PWROFF_DRAG_TRIES) {
            s->pwroff_phase = PWROFF_SETTLE;
            s5l8920_pwroff_arm(s, 2500);
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
    s->pwroff_tries = 0;
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

static char *s5l8920_get_die_id(Object *obj, Error **errp)
{
    return g_strdup(S5L8920_MACHINE(obj)->die_id);
}

static void s5l8920_set_die_id(Object *obj, const char *value, Error **errp)
{
    g_free(S5L8920_MACHINE(obj)->die_id);
    S5L8920_MACHINE(obj)->die_id = g_strdup(value);
}

static bool s5l8920_get_wifi(Object *obj, Error **errp)
{
    return S5L8920_MACHINE(obj)->wifi;
}

static void s5l8920_set_wifi(Object *obj, bool value, Error **errp)
{
    S5L8920_MACHINE(obj)->wifi = value;
}

/* The agent's properties (tests and the app drive it over QMP), as ipad1's. */
static void s5l8920_set_agent_request(Object *obj, const char *value, Error **errp)
{
    int error = ipod_agent_submit(S5L8920_MACHINE(obj)->agent, value);
    if (error) {
        error_setg(errp, "%s", ipod_agent_submit_error(error));
    }
}

static void s5l8920_cancel_agent_request(Object *obj, const char *value, Error **errp)
{
    ipod_agent_cancel(S5L8920_MACHINE(obj)->agent, value);
}

static char *s5l8920_get_agent_result(Object *obj, Error **errp)
{
    return ipod_agent_take_result(S5L8920_MACHINE(obj)->agent);
}

static char *s5l8920_get_agent_status(Object *obj, Error **errp)
{
    return g_strdup(ipod_agent_status(S5L8920_MACHINE(obj)->agent, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)));
}

/* The iPod machine's battery and cable properties, on the PCF50633 model's API. */
static bool s5l8920_get_usb_attached(Object *obj, Error **errp)
{
    return S5L8920_MACHINE(obj)->usb_attached;
}

static void s5l8920_set_usb_attached(Object *obj, bool value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    s->usb_attached = value;
    if (s->pmu) {
        pcf50633_set_usb_cable(s->pmu, value);
    }
}

static void s5l8920_get_battery_level(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);
    int64_t value = s->battery_level;

    if (s->pmu) {
        pcf50633_update_battery(s->pmu);
        value = pcf50633_level_for_adc(s->pmu->adc_values[4]);
    }
    visit_type_int(v, name, &value, errp);
}

static void s5l8920_set_battery_level(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 100) {
        error_setg(errp, "battery-level must be between 0 and 100");
        return;
    }
    s->battery_level = value;
    if (s->pmu) {
        pcf50633_set_battery_level(s->pmu, value);
    }
}

static char *s5l8920_get_battery_charging(Object *obj, Error **errp)
{
    unsigned mode = S5L8920_MACHINE(obj)->battery_charging;

    return g_strdup(mode == 1 ? "on" : mode == 2 ? "off" : "auto");
}

static void s5l8920_set_battery_charging(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);
    static const char *const modes[] = { "auto", "on", "off" };

    for (unsigned mode = 0; mode < ARRAY_SIZE(modes); mode++) {
        if (!strcmp(value, modes[mode])) {
            s->battery_charging = mode;
            if (s->pmu) {
                pcf50633_set_charging_mode(s->pmu, mode);
            }
            return;
        }
    }
    error_setg(errp, "battery-charging must be auto, on or off");
}

static void s5l8920_instance_init(Object *obj)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    s->wifi = true;
    s->usb_attached = true;
    s->battery_level = -1;
    guest_pkg_init(&s->pkg, obj);
    guest_pb_init(&s->pb, obj, "s5l8920");
    s->agent = ipod_agent_new();
    ipod_agent_publish(s->agent);
    object_property_add_str(obj, "agent-request", NULL, s5l8920_set_agent_request);
    object_property_add_str(obj, "agent-cancel", NULL, s5l8920_cancel_agent_request);
    object_property_add_str(obj, "agent-result", s5l8920_get_agent_result, NULL);
    object_property_add_str(obj, "agent-status", s5l8920_get_agent_status, NULL);
}

static void s5l8920_instance_finalize(Object *obj)
{
    ipod_agent_publish(NULL);
    ipod_agent_free(S5L8920_MACHINE(obj)->agent);
    g_free(S5L8920_MACHINE(obj)->usb_tcp_addr);
    g_free(S5L8920_MACHINE(obj)->die_id);
    g_free(S5L8920_MACHINE(obj)->nor_path);
    g_free(S5L8920_MACHINE(obj)->nor_rw_path);
    g_free(S5L8920_MACHINE(obj)->kboot_path);
    g_free(S5L8920_MACHINE(obj)->nand_path);
    g_free(S5L8920_MACHINE(obj)->nand_overlay_path);
}

static char *s5l8920_get_imei(Object *obj, Error **errp)
{
    return g_strdup(S5L8920_MACHINE(obj)->imei);
}

static void s5l8920_set_imei(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);

    g_free(s->imei);
    s->imei = g_strdup(value);
}

static char *s5l8920_get_panel(Object *obj, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);
    return s->panel_w ? g_strdup_printf("%ux%u", s->panel_w, s->panel_h) : g_strdup("");
}

/* "panel=WxH": a panel of another size than the 320x480 one, as on the iPod 2G, whose M2 CLCD this is
 * (its window keeps 9 bits of height). */
static void s5l8920_set_panel(Object *obj, const char *value, Error **errp)
{
    S5L8920MachineState *s = S5L8920_MACHINE(obj);
    unsigned w, h;
    char end;

    if (s->gpio) {
        error_setg(errp, "panel must be set before the machine starts");
        return;
    }
    if (sscanf(value, "%ux%u%c", &w, &h, &end) != 2 || w < 64 || h < 64 || w > 1024 || h > 511 || (w & 1)) {
        error_setg(errp, "panel must be WxH (even width 64..1024, height 64..511)");
        return;
    }
    s->panel_w = w;
    s->panel_h = h;
}

static bool s5l8920_get_ring_switch(Object *obj, Error **errp)
{
    return S5L8920_MACHINE(obj)->ring_silent;
}

static void s5l8920_set_ring_switch(Object *obj, bool value, Error **errp)
{
    S5L8920_MACHINE(obj)->ring_silent = value;
    s5l8920_apply_ring_switch(S5L8920_MACHINE(obj));
}

static bool s5l8920_get_baseband(Object *obj, Error **errp)
{
    return S5L8920_MACHINE(obj)->baseband_on;
}

static void s5l8920_set_baseband(Object *obj, bool value, Error **errp)
{
    S5L8920_MACHINE(obj)->baseband_on = value;
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
    mc->reset = s5l8920_machine_reset;
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
    object_class_property_add_str(klass, "die-id", s5l8920_get_die_id, s5l8920_set_die_id);
    object_class_property_set_description(klass, "die-id",
        "the unit's ChipID die-id words 2-3, \"0xWORD2:0xWORD3\" (identity.json); zeros if unset");
    object_class_property_add_bool(klass, "usb-attached", s5l8920_get_usb_attached, s5l8920_set_usb_attached);
    object_class_property_set_description(klass, "usb-attached", "The dock cable is in (default on)");
    object_class_property_add(klass, "battery-level", "int", s5l8920_get_battery_level, s5l8920_set_battery_level,
                              NULL, NULL);
    object_class_property_set_description(klass, "battery-level", "Battery charge, 0-100 percent (the PMU's ADC)");
    object_class_property_add_str(klass, "battery-charging", s5l8920_get_battery_charging, s5l8920_set_battery_charging);
    object_class_property_set_description(klass, "battery-charging", "auto, on or off");
    object_class_property_add_bool(klass, "guest-shutdown-confirmed", s5l8920_get_guest_shutdown_confirmed, NULL);
    object_class_property_set_description(klass, "guest-shutdown-confirmed",
        "The guest's power-off command reached the PMU this run");
    object_class_property_add_bool(klass, "display-sleeping", s5l8920_get_display_sleeping, NULL);
    object_class_property_set_description(klass, "display-sleeping", "The panel is off (DSI display-off)");
    object_class_property_add_bool(klass, "wifi", s5l8920_get_wifi, s5l8920_set_wifi);
    object_class_property_set_description(klass, "wifi",
        "Bridge the Wi-Fi card to -netdev id=wifi0 (a NAT one is made when absent); off keeps the card, unbridged");
    object_class_property_add_bool(klass, "gles-debug", s5l8920_get_gles_debug, s5l8920_set_gles_debug);
    object_class_property_add_bool(klass, "baseband", s5l8920_get_baseband, s5l8920_set_baseband);
    object_class_property_add_bool(klass, "ring-switch", s5l8920_get_ring_switch, s5l8920_set_ring_switch);
    object_class_property_add_str(klass, "panel", s5l8920_get_panel, s5l8920_set_panel);
    object_class_property_set_description(klass, "ring-switch",
        "the side switch (buttons/ringerab, iPhone): on = silent, off = ring (default); settable at run time");
    object_class_property_set_description(klass, "baseband",
        "radio boards: put the fake cellular modem behind spi2 (default off: a bare controller)");
    object_class_property_add_str(klass, "imei", s5l8920_get_imei, s5l8920_set_imei);
    object_class_property_set_description(klass, "imei",
        "the unit's IMEI (FirmwareKit's device.lock.json machine.imei), for the modem (baseband=on) to report");
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
