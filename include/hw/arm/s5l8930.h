/*
 * Apple S5L8930 ("A4") SoC, as used by the iPad 1 (K48AP) machine.
 *
 * Addresses: 7B500 K48AP device tree (arm-io maps child offsets at 0x80000000).
 * Kernel-side register contracts: docs/research/gap-kernel-platform-mmio.md.
 */
#ifndef HW_ARM_S5L8930_H
#define HW_ARM_S5L8930_H

#include "hw/core/sysbus.h"
#include "hw/arm/ipod_touch_sdio.h"
#include "system/address-spaces.h"

/* Memory */
#define S5L8930_DRAM_BASE        0x40000000
#define S5L8930_DRAM_SIZE        0x10000000   /* 256 MiB */
#define S5L8930_SRAM_BASE        0x84000000
#define S5L8930_SRAM_SIZE        0x00040000
#define S5L8930_KERNEL_VIRT_BASE 0xc0000000

/* Peripherals */
#define S5L8930_SPI_BASE(n)      (0x82000000 + (n) * 0x100000)   /* spi0 NOR, spi1 multitouch */
#define S5L8930_UART_BASE(n)     (0x82500000 + (n) * 0x100000)
#define S5L8930_I2C_BASE(n)      (0x83200000 + (n) * 0x100000)   /* i2c0, i2c2 on K48 */
#define S5L8930_I2C_SIZE         0x1000
#define S5L8930_DISP_PIPE0_BASE  0x89000000   /* AppleDisplayPipe */
#define S5L8930_DISP_PIPE0_SIZE  0x7000
#define S5L8930_RGBOUT_BASE      0x89100000
#define S5L8930_CLCD_BASE        0x89200000   /* CLCD timing generator */
#define S5L8930_CLCD_SIZE        0x2000
#define S5L8930_TVOUT_BASE       0x89400000
#define S5L8930_DSIM_BASE        0x89500000   /* MIPI-DSIM, same IP as the iPod's */
#define S5L8930_RGBOUT2_BASE     0x89600000
#define S5L8930_DART1_BASE       0x88d00000   /* IOMMU in front of ISP, JPEG, VENC (DT dart1) */
#define S5L8930_DART2_BASE       0x89d00000   /* IOMMU in front of the display pipe */
#define S5L8930_DART_SIZE        0x2000
#define S5L8930_PKE_BASE         0x83100000   /* RSA engine; operand SRAM at +0x800 */
#define S5L8930_SHA1_BASE        0x80100000   /* SHA-1 engine; data FIFO at +0xA0 via CDMA ch 4 */
#define S5L8930_SHA1_SIZE        0x1000
#define S5L8930_CDMA_BASE        0x87000000   /* shared DMA engine, channel n at n<<12 */
#define S5L8930_CDMA_SIZE        0x26000
#define S5L8930_AES_BASE         0x87800000   /* CDMA AES filter contexts, ctx n at n<<12 */
#define S5L8930_AES_SIZE         0x9000
#define S5L8930_PWM_BASE         0x83500000   /* codec MCLK source; unmodeled */
#define S5L8930_AMC_BASE         0x84100000   /* audio media codec registers */
#define S5L8930_AMC_AUX_BASE     0x84300000   /* third AMC window, unmodeled */
#define S5L8930_AMC_AUX_SIZE     0x5000
#define S5L8930_AMC_PORT_BASE    0x84800000   /* AMC output port (CDMA 0x17 FIFO at +0x2c) */
#define S5L8930_I2S_BASE(n)      (0x84500400 + (n) * 0x1000)
#define S5L8930_SWI_BASE         0xbf600000   /* backlight/core-voltage single-wire, same IP as the S5L8720's */
#define S5L8930_PMGR_BASE        0xbf100000   /* clocks, gates, timer (+0x2000), POWER_ID (+0x6000) */
#define S5L8930_PMGR_SIZE        0x8000
#define S5L8930_VIC_BASE(n)      (0xbf200000 + (n) * 0x10000)
#define S5L8930_VIC_COUNT        4
#define S5L8930_CHIPID_BASE      0xbf500000
#define S5L8930_GPIO_BASE        0xbfa00000
#define S5L8930_GPIO_SIZE        0x1000
#define S5L8930_CPU_DEBUG_BASE   0xbf701000
#define S5L8930_USB_PHY_BASE     0x86000000   /* otgphyctrl,s5l8930x */
#define S5L8930_USB_OTG_BASE     0x86100000   /* Synopsys DWC OTG, device mode */
#define S5L8930_SCALER_BASE      0x89300000   /* scaler,s5l8930x|s5l8720x (M2 scaler/CSC) */
#define S5L8930_USB_EHCI_BASE    0x86400000   /* usb-ehci,s5l8930x (host) */
#define S5L8930_USB_OHCI0_BASE   0x86500000   /* usb-ohci,s5l8930x (host) */
#define S5L8930_H2FMI_BASE       0x81200000   /* FMI0; FMI1 at +0x100000 */
#define S5L8930_SDIO_BASE        0x80000000   /* SDHC, standard SDHCI registers */
#define S5L8930_IOP_BASE         0x86300000   /* AP-side IOP control block */
#define S5L8930_IOP_SIZE         0x1000
#define S5L8930_IOP_VIC_BASE     0xbf300000   /* the IOP's own 4 VICs, used as doorbells */

/*
 * The IOP's view of DRAM. The EmbeddedIOP firmware's 'cnfg' memory map declares
 * {iop 0xc0000000 -> phys 0x40000000, 0x40000000 bytes} on every build; from
 * EmbeddedIOP-20 (iOS 4.3) the kernel hands the IOP addresses in that window
 * (rings, messages, DMA segments) where iBoot-817/931's kexts passed physical
 * ones. H-class instrument for the HLE (docs/fidelity-ledger K48 #33): goes
 * away with the IOP core, whose bus does the same mapping.
 */
/* hw/arm/s5l8930_iop_core.c: the IOP as a second core running the uploaded firmware. */
qemu_irq s5l8930_iop_core_irq(DeviceState *dev, int irq);
void s5l8930_iop_core_run(DeviceState *dev, uint32_t fw_base, uint32_t fw_size);
void s5l8930_iop_core_stop(DeviceState *dev);
void s5l8930_iop_core_set_iop(DeviceState *dev, DeviceState *iop);
void s5l8930_iop_trace_rings(DeviceState *iop);

static inline hwaddr s5l8930_iop_pa(hwaddr a)
{
    return (a & 0xf0000000) == 0xc0000000 ? a - 0xc0000000 + S5L8930_DRAM_BASE : a;
}
#define S5L8930_IOP_VIC_SIZE     0x40000

/* Interrupt numbers: VIC n owns 32n..32n+31 */
#define S5L8930_IRQ_IOP          0x03          /* IOP -> AP doorbell */
#define S5L8930_IRQ_SDIO         0x26          /* SDHC: the Wi-Fi card interrupt */
#define S5L8930_IRQ_USB_OTG      0x0d
#define S5L8930_IRQ_SCALER       0x0b
#define S5L8930_IRQ_USB_EHCI     0x0e
#define S5L8930_IRQ_USB_OHCI0    0x0f
#define S5L8930_IRQ_TIMER1       0x05          /* second event timer, unused by the kernel */
#define S5L8930_IRQ_TIMER0       0x06          /* event timer; the kernel routes it to FIQ */
#define S5L8930_IRQ_FMI(n)       (0x22 + (n))
#define S5L8930_IRQ_I2C(n)       (0x13 + (n))
#define S5L8930_IRQ_SPI(n)       (0x1d + (n))
#define S5L8930_IRQ_UART(n)      (0x16 + (n))
#define S5L8930_IRQ_DSIM         0x28
#define S5L8930_IRQ_CLCD         0x29
#define S5L8930_IRQ_DISP_PIPE0   0x2a
#define S5L8930_IRQ_RGBOUT_PIPE  0x2b   /* DT rgbout interrupts[0], as clcd's [0] is pipe0 */
#define S5L8930_IRQ_CDMA(ch)     (0x30 + (ch))   /* DT lists channels 1.. from 0x31 */
#define S5L8930_IRQ_AMC          0x56          /* first of the AMC's 23 lines */
#define S5L8930_IRQ_GPIO         0x74

/*
 * PMGR (hw/arm/s5l8930_pmgr.c). One MMIO region of S5L8930_PMGR_SIZE.
 * sysbus IRQ 0 = event timer 0 (S5L8930_IRQ_TIMER0), IRQ 1 = event timer 1.
 * Watchdog expiry calls qemu_system_reset_request().
 */
#define TYPE_S5L8930_PMGR "s5l8930.pmgr"

/*
 * GPIO interrupt controller (hw/arm/s5l8930_gpio.c). One MMIO region of
 * S5L8930_GPIO_SIZE; sysbus IRQ 0 = S5L8930_IRQ_GPIO. Qdev GPIO inputs
 * 0..S5L8930_GPIO_PINS-1 drive pin input levels (buttons, PMU IRQ, ...).
 */
#define TYPE_S5L8930_GPIO "s5l8930.gpio"
void s5l8930_gpio_set_rest_level(DeviceState *dev, unsigned pin, bool level);
#define S5L8930_GPIO_PINS        (0x16 * 8)    /* 22 ports x 8 pins (the K48; "ports" property) */
#define S5L8930_GPIO_MAX_PINS    (0x2e * 8)    /* the S5L8920's 46 ports */
/* DT pin ids are 0xPPB (port, bit); qdev GPIO outputs 0..PINS-1 follow
 * pins the guest drives as outputs (NOR chip select, panel reset, ...). */
#define S5L8930_GPIO_PIN(dt)     (((dt) >> 8) * 8 + ((dt) & 0xff))
#define S5L8930_GPIO_BTN_MENU    0x000
#define S5L8930_GPIO_BTN_HOLD    0x001
#define S5L8930_GPIO_BTN_VOLUP   0x002
#define S5L8930_GPIO_BTN_VOLDOWN 0x003
#define S5L8930_GPIO_BTN_RINGER  0x004         /* buttons/ringerab: the ring/silent switch */
#define S5L8930_GPIO_NOR_CS      0x505
#define S5L8930_GPIO_MT_ATN      0x205         /* == pin 0x15 */

/*
 * IOP (hw/arm/s5l8930_iop.c): high-level emulation of the second core's
 * host-visible surface. MMIO 0 = control block (S5L8930_IOP_SIZE at
 * S5L8930_IOP_BASE), MMIO 1 = the IOP-side VIC window (S5L8930_IOP_VIC_SIZE at
 * S5L8930_IOP_VIC_BASE). sysbus IRQ 0 = S5L8930_IRQ_IOP. Reads and writes the
 * message rings in guest DRAM through address_space_memory.
 */
#define TYPE_S5L8930_IOP "s5l8930.iop"
/* Raw access to the IOP's NAND page store for the H2FMI model: one page
 * (data then spare, *stride bytes) into buf; false = blank or no store. */
bool s5l8930_iop_nand_read(DeviceState *dev, int bus, uint32_t ce,
                           uint32_t page, uint8_t *buf, uint32_t *stride);
uint32_t s5l8930_iop_nand_program(DeviceState *dev, int bus, uint32_t ce, uint32_t page,
                                  const uint8_t *data, uint32_t len, const uint8_t *meta);
uint32_t s5l8930_iop_nand_erase(DeviceState *dev, int bus, uint32_t ce, uint32_t page);
bool s5l8930_iop_io_failed(void);
void s5l8930_iop_nand_info(DeviceState *dev, uint32_t *id, uint8_t *ce_mask,
                           uint32_t *page_bytes);

/*
 * H2FMI (hw/arm/s5l8930_h2fmi.c): the NAND interfaces iBoot drives directly.
 * MMIO n = FMI n's 1 MiB window at S5L8930_H2FMI_BASE + n MiB (FMI, FMC at
 * +0x40000, ECC at +0x80000); sysbus IRQ n = S5L8930_IRQ_FMI(n). Links:
 * "iop" (page store), "cdma" (FIFO-fed reads).
 */
#define TYPE_S5L8930_H2FMI "s5l8930.h2fmi"

/*
 * SDIO (hw/arm/s5l8930_sdio.c): the SDHC interrupt registers (MMIO 0 at
 * S5L8930_SDIO_BASE, sysbus IRQ 0 = S5L8930_IRQ_SDIO) and the IOP ring-3
 * commands, run against the "card" link (an ipodtouch.sdio dongle). GPIO in 0
 * is the card's interrupt output. The IOP's "sdio" link forwards ring 3 here.
 */
#define TYPE_S5L8930_SDIO "s5l8930.sdio"
#define S5L8930_SDIO_CMD_SIZE 0x200
void s5l8930_sdio_iop_command(DeviceState *dev, uint8_t *cmd);

/*
 * I2C controller (hw/arm/s5l8930_i2c.c): the newer FIFO-style block the
 * AppleS5L8920XI2CController kext drives. One MMIO region of S5L8930_I2C_SIZE;
 * sysbus IRQ 0. Exposes a standard QEMU I2C bus named "i2c" for slaves.
 * The Dialog D1815 PMU slave (TYPE_S5L8930_D1815) lives in the same file.
 */
#define TYPE_S5L8930_I2C "s5l8930.i2c"
#define TYPE_S5L8930_D1815 "s5l8930.d1815"
/* Home (hold=false) / Hold (true) press or release; a press latches the
 * PMU wake event and raises its IRQ line. */
void s5l8930_d1815_button(DeviceState *dev, bool hold, bool down);
void s5l8930_d1815_usb_cable_event(DeviceState *dev);
/* Battery voltage the PMU ADC reports (mux 4); the level SpringBoard shows. */
void s5l8930_d1815_set_vbat(DeviceState *dev, unsigned mv);
/* A USB host on the dock connector: its 15 kOhm pull-downs hold D+/D- near 0 V, which is what
 * ADC mux 6 (DT function-brick_id_voltage) measures; otherwise the lines float mid-scale. */
void s5l8930_d1815_set_usb_host(DeviceState *dev, bool host);
/* The guest has powered off: the standby write, or the halt-with-cable restart into the power-off simulation. */
bool s5l8930_d1815_guest_shutdown_confirmed(void);
#define TYPE_S5L8930_TCA6408 "s5l8930.tca6408"   /* GPIO expander at 0x20 on i2c0 */
/* LTC4099 charger at 0x09 on i2c0 (hw/arm/s5l8930_ltc4099.c); its STAT byte
 * is where the USB arbitrator learns a cable is present (usb-present prop). */
#define TYPE_S5L8930_LTC4099 "s5l8930.ltc4099"
void s5l8930_ltc4099_set_usb(DeviceState *dev, bool present);
/* STAT charge-state bits (secondary_charge_status): charging or not. */
void s5l8930_ltc4099_set_charging(DeviceState *dev, bool charging);
#define TYPE_S5L8930_TSL2581 "s5l8930.tsl2581"   /* ambient light sensor at 0x39 on i2c2 */
#define TYPE_S5L8930_TSL2561 "s5l8930.tsl2561"   /* the 3GS's, at 0x49 on i2c2 */
/* AK8973 magnetometer at 0x1e on i2c0; "heading" (degrees) sets the field. */
#define TYPE_S5L8930_AK8973 "s5l8930.ak8973"
/* L3G4200D gyroscope at 0x68 on i2c2 ("gyro,ap3gdl"); gpio-out 0/1 = INT1/INT2 pin levels. */
#define TYPE_S5L8930_L3G4200D "s5l8930.l3g4200d"
/* The accelerometer whose gravity vector gives the compass its pose. */
struct LIS302DLState;
void s5l8930_ak8973_set_accel(DeviceState *dev, struct LIS302DLState *accel);

/*
 * bq27545 gas gauge (hw/arm/s5l8930_hdq.c): a chardev speaking HDQ-over-UART
 * the way configd's AppleHDQGasGauge plugin bit-bangs it on /dev/tty.gas-gauge
 * (UART5). Attach it as UART5's chardev.
 */
#define TYPE_CHARDEV_S5L8930_HDQ "chardev-s5l8930-hdq-gauge"
/* Battery the gauge reports: level 0..100 %, charging or discharging. Kept
 * across guest resets. */
void s5l8930_hdq_set_battery(Chardev *chr, int level, bool charging);
void s5l8930_hdq_set_capacity(Chardev *chr, int mah);

/*
 * Display (hw/arm/s5l8930_display.c): DisplayPipe0 + CLCD + RGBOUT/TV-out
 * ready bits + minimal DART2, scanning out a 1024x768 framebuffer to a QEMU
 * console. MMIO 0 pipe, 1 CLCD, 2 DART2, 3 RGBOUT, 4 TVOUT, 5 RGBOUT2;
 * sysbus IRQ 0 = pipe (S5L8930_IRQ_DISP_PIPE0), 1 = CLCD, 2 = RGBOUT pipe.
 */
#define TYPE_S5L8930_DISPLAY "s5l8930.display"

/*
 * CDMA + AES filter (hw/arm/s5l8930_cdma.c). MMIO 0 = channel block
 * (S5L8930_CDMA_SIZE at S5L8930_CDMA_BASE), MMIO 1 = AES contexts
 * (S5L8930_AES_SIZE at S5L8930_AES_BASE); sysbus IRQ n = channel n
 * (S5L8930_IRQ_CDMA(n)), n < S5L8930_CDMA_CHANNELS. Chains complete
 * synchronously inside the channel's go write.
 */
#define TYPE_S5L8930_CDMA "s5l8930.cdma"
/*
 * A device FIFO in [base, base+size) that paces the channels reading it: a
 * channel takes only avail() bytes, stays running, and resumes on kick().
 */
/*
 * DART register block (hw/arm/s5l8930_dart.c), shared by the display's dart2
 * and TYPE_S5L8930_DART (dart1, MMIO 0). 4 stream IDs x 64 segment entries.
 */
#define S5L8930_DART_SIDS        4
#define S5L8930_DART_SEGS        64
typedef struct S5L8930Dart {
    uint32_t regs[S5L8930_DART_SIZE / 4];
    uint32_t ste[S5L8930_DART_SIDS][S5L8930_DART_SEGS];
} S5L8930Dart;
extern const MemoryRegionOps s5l8930_dart_ops;     /* opaque: S5L8930Dart * */
/* `va` for stream `sid` -> PA: `va` itself if its segment has no table, -1 on an invalid PTE. */
hwaddr s5l8930_dart_xlate(S5L8930Dart *d, unsigned sid, uint32_t va);
#define TYPE_S5L8930_DART "s5l8930.dart"
/* dart2 translation of `va` for stream ID `sid`; -1 if unmapped. */
hwaddr s5l8930_dart2_xlate(void *display, uint32_t va, unsigned sid);
/* The iPod scaler model (hw/arm/ipod_touch_scaler.c) behind an IOMMU. */
void ipod_scaler_set_iommu(DeviceState *scaler,
                           hwaddr (*xlate)(void *opaque, uint32_t va, unsigned sid),
                           void *opaque, unsigned sid);
void ipod_scaler_set_version(DeviceState *scaler, uint32_t version);

void s5l8930_cdma_set_source(DeviceState *dev, hwaddr base, hwaddr size,
                             uint32_t (*avail)(void *opaque, hwaddr addr, bool to_device),
                             void *opaque);
void s5l8930_cdma_kick(DeviceState *dev);
void s5l8930_cdma_sink_done(DeviceState *dev, uint32_t fifo_base, uint32_t size);

/*
 * I2S controller (hw/arm/s5l8930_i2s.c). One MMIO region (0x1000) at
 * S5L8930_I2S_BASE(n); "audio-out" routes its TX FIFO to the
 * host audio backend (i2s0, the CS42L61 codec port).
 */
#define TYPE_S5L8930_I2S "s5l8930.i2s"

/*
 * Frame rate of I2S port n. Its bit clock is PMGR NCO n (0xbf100100 +
 * 0x10 n): AppleS5L8930XPerformanceControllerFunctionNCOFrequency writes
 * +4 = 64 * fs (0x002b1100 for 44.1 kHz, c0645046) when AppleARMIISAudio
 * sets the device rate. 44.1 kHz until the kernel has programmed it.
 */
#define S5L8930_NCO_BCLK(n)      (S5L8930_PMGR_BASE + 0x104 + 0x10 * (n))
static inline unsigned s5l8930_i2s_rate(unsigned port)
{
    uint32_t bclk = address_space_ldl_le(&address_space_memory,
                                         S5L8930_NCO_BCLK(port),
                                         MEMTXATTRS_UNSPECIFIED, NULL);

    return bclk >= 64 * 8000 && bclk <= 64 * 192000 ? bclk / 64 : 44100;
}
#define S5L8930_CDMA_CHANNELS    0x26

/*
 * SHA-1 engine (hw/arm/s5l8930_sha1.c): one MMIO region of S5L8930_SHA1_SIZE.
 * No interrupt line: the kext completes on the CDMA channel, not IRQ 0x25.
 */
#define TYPE_S5L8930_SHA1 "s5l8930.sha1"

/*
 * A kboot bundle's DT property edit (hw/arm/ipad1.c, also used by the S5L8920
 * boards): the node named `name` gets `prop` overwritten in place (same slot,
 * zero-padded) when the slot holds `vlen` bytes.
 */
typedef struct A4DTEdit {
    const char *name, *prop;
    const void *value;
    uint32_t vlen;
} A4DTEdit;
void a4_dt_edit(uint8_t *image, size_t image_len, uint32_t load_pa,
                uint32_t bootargs_pa, const A4DTEdit *e);

#endif
