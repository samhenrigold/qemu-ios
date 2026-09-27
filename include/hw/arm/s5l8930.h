/*
 * Apple S5L8930 ("A4") SoC, as used by the iPad 1 (K48AP) machine.
 *
 * Addresses: 7B500 K48AP device tree (arm-io maps child offsets at 0x80000000).
 * Kernel-side register contracts: docs/ipad1/research/gap-kernel-platform-mmio.md.
 */
#ifndef HW_ARM_S5L8930_H
#define HW_ARM_S5L8930_H

#include "hw/sysbus.h"

/* Memory */
#define S5L8930_DRAM_BASE        0x40000000
#define S5L8930_DRAM_SIZE        0x10000000   /* 256 MiB */
#define S5L8930_SRAM_BASE        0x84000000
#define S5L8930_SRAM_SIZE        0x00040000
#define S5L8930_KERNEL_VIRT_BASE 0xc0000000

/* Peripherals */
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
#define S5L8930_DART2_BASE       0x89d00000   /* IOMMU in front of the display pipe */
#define S5L8930_DART2_SIZE       0x2000
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
#define S5L8930_IOP_BASE         0x86300000   /* AP-side IOP control block */
#define S5L8930_IOP_SIZE         0x1000
#define S5L8930_IOP_VIC_BASE     0xbf300000   /* the IOP's own 4 VICs, used as doorbells */
#define S5L8930_IOP_VIC_SIZE     0x40000

/* Interrupt numbers: VIC n owns 32n..32n+31 */
#define S5L8930_IRQ_IOP          0x03          /* IOP -> AP doorbell */
#define S5L8930_IRQ_USB_OTG      0x0d
#define S5L8930_IRQ_TIMER1       0x05          /* second event timer, unused by the kernel */
#define S5L8930_IRQ_TIMER0       0x06          /* event timer; the kernel routes it to FIQ */
#define S5L8930_IRQ_I2C(n)       (0x13 + (n))
#define S5L8930_IRQ_UART(n)      (0x16 + (n))
#define S5L8930_IRQ_DSIM         0x28
#define S5L8930_IRQ_CLCD         0x29
#define S5L8930_IRQ_DISP_PIPE0   0x2a
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
#define S5L8930_GPIO_PINS        (0x16 * 8)    /* 22 ports x 8 pins */

/*
 * IOP (hw/arm/s5l8930_iop.c): high-level emulation of the second core's
 * host-visible surface. MMIO 0 = control block (S5L8930_IOP_SIZE at
 * S5L8930_IOP_BASE), MMIO 1 = the IOP-side VIC window (S5L8930_IOP_VIC_SIZE at
 * S5L8930_IOP_VIC_BASE). sysbus IRQ 0 = S5L8930_IRQ_IOP. Reads and writes the
 * message rings in guest DRAM through address_space_memory.
 */
#define TYPE_S5L8930_IOP "s5l8930.iop"

/*
 * I2C controller (hw/arm/s5l8930_i2c.c): the newer FIFO-style block the
 * AppleS5L8920XI2CController kext drives. One MMIO region of S5L8930_I2C_SIZE;
 * sysbus IRQ 0. Exposes a standard QEMU I2C bus named "i2c" for slaves.
 * The Dialog D1815 PMU slave (TYPE_S5L8930_D1815) lives in the same file.
 */
#define TYPE_S5L8930_I2C "s5l8930.i2c"
#define TYPE_S5L8930_D1815 "s5l8930.d1815"
#define TYPE_S5L8930_TCA6408 "s5l8930.tca6408"   /* GPIO expander at 0x20 on i2c0 */

/*
 * Display (hw/arm/s5l8930_display.c): DisplayPipe0 + CLCD + RGBOUT/TV-out
 * ready bits + minimal DART2, scanning out a 1024x768 framebuffer to a QEMU
 * console. MMIO 0 pipe, 1 CLCD, 2 DART2, 3 RGBOUT, 4 TVOUT, 5 RGBOUT2;
 * sysbus IRQ 0 = pipe (S5L8930_IRQ_DISP_PIPE0), 1 = CLCD.
 */
#define TYPE_S5L8930_DISPLAY "s5l8930.display"

#endif
