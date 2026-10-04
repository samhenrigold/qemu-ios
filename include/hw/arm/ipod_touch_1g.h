#ifndef HW_ARM_IPOD_TOUCH_1G_H
#define HW_ARM_IPOD_TOUCH_1G_H

/*
 * iPod touch 1G (N45AP, S5L8900) machine. Memory map and interrupt numbers
 * from devos50's ipod_touch.h (branch ipod_touch_1g); the models behind
 * them are the shared ipodtouch.* ones wherever the two SoCs agree, and the
 * s5l8900_* ones where they do not.
 */

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/boards.h"
#include "hw/intc/pl192.h"
#include "hw/arm/ipod_touch_timer.h"
#include "hw/arm/ipod_touch_clock.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/arm/ipod_touch_sysic.h"
#include "hw/arm/ipod_touch_gpio.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "hw/arm/ipod_touch_multitouch.h"
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "hw/arm/s5l8900_fmc.h"
#include "hw/arm/s5l8900_adm.h"
#include "hw/arm/guest-package.h"
#include "cpu.h"

#define TYPE_IPOD_TOUCH_1G "iPod-Touch-1G"
#define TYPE_IPOD_TOUCH_1G_MACHINE MACHINE_TYPE_NAME(TYPE_IPOD_TOUCH_1G)
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouch1GMachineState, IPOD_TOUCH_1G_MACHINE)

/* VIC lines */
#define N45_TIMER1_IRQ      0x7
#define N45_SPI0_IRQ        0x9
#define N45_SPI1_IRQ        0xA
#define N45_SPI2_IRQ        0xB
#define N45_MBX_IRQ         0xC
#define N45_LCD_IRQ         0xD
#define N45_USB_OTG_IRQ     0x13
#define N45_DMAC0_IRQ       0x10
#define N45_DMAC1_IRQ       0x11
#define N45_I2C0_IRQ        0x15
#define N45_I2C1_IRQ        0x16
#define N45_UART0_IRQ       0x18
#define N45_TVOUT_SDO_IRQ   0x1E
#define N45_ADM_IRQ         0x25
#define N45_TVOUT_MIXER_IRQ 0x26
#define N45_SDIO_IRQ        0x2A    /* the DT sdio node */
#define N45_NAND_ECC_IRQ    0x2B
/* GPIO interrupt groups 0..6 */
#define N45_GPIO_G0_IRQ 0x21
#define N45_GPIO_G1_IRQ 0x20
#define N45_GPIO_G2_IRQ 0x1F
#define N45_GPIO_G3_IRQ 0x03
#define N45_GPIO_G4_IRQ 0x02
#define N45_GPIO_G5_IRQ 0x01
#define N45_GPIO_G6_IRQ 0x00

/* Buttons: GPIO pad 0x16, and their GPIO-IC interrupt numbers (group 1). */
#define N45_GPIO_BUTTON_POWER     0x1605
#define N45_GPIO_BUTTON_HOME      0x1606
#define N45_GPIO_BUTTON_POWER_IRQ 0x2D
#define N45_GPIO_BUTTON_HOME_IRQ  0x2E

/* Memory map */
#define N45_RAM_BASE          0x08000000   /* 128 MiB */
#define N45_RAM_SIZE          0x08000000
#define N45_IBOOT_BASE        0x18000000
#define N45_IBOOT_SIZE        0x00400000
#define N45_VROM_BASE         0x20000000
#define N45_VROM_SIZE         0x00010000
#define N45_LLB_BASE          0x22000000   /* holds the two 8900 stubs */
#define N45_SRAM1_BASE        0x22020000
#define N45_NOR_BASE          0x24000000
#define N45_NOR_SIZE          0x00100000
#define N45_SHA1_BASE         0x38000000
#define N45_CLOCK0_BASE       0x38100000
#define N45_DMAC0_BASE        0x38200000
#define N45_USBOTG_BASE       0x38400000
#define N45_ADM_BASE          0x38800000
#define N45_DISPLAY_BASE      0x38900000
#define N45_NAND_BASE         0x38A00000
#define N45_AES_BASE          0x38C00000
#define N45_SDIO_BASE         0x38D00000
#define N45_VIC0_BASE         0x38E00000
#define N45_VIC1_BASE         0x38E01000
#define N45_EDGEIC_BASE       0x38E02000
#define N45_NAND_ECC_BASE     0x38F00000
#define N45_TVOUT1_BASE       0x39100000
#define N45_TVOUT2_BASE       0x39200000
#define N45_TVOUT3_BASE       0x39300000
#define N45_MPVD_BASE         0x39600000
#define N45_H264BPD_BASE      0x39800000
#define N45_DMAC1_BASE        0x39900000
#define N45_SYSIC_BASE        0x39A00000
#define N45_MBX_BASE          0x3B000000
#define N45_SPI0_BASE         0x3C300000
#define N45_USBPHYS_BASE      0x3C400000
#define N45_CLOCK1_BASE       0x3C500000
#define N45_I2C0_BASE         0x3C600000
#define N45_I2C1_BASE         0x3C900000
#define N45_IIS2_BASE         0x3CA00000
#define N45_UART0_BASE        0x3CC00000
#define N45_UART1_BASE        0x3CC04000
#define N45_UART2_BASE        0x3CC08000
#define N45_UART3_BASE        0x3CC0C000
#define N45_UART4_BASE        0x3CC10000
#define N45_IIS1_BASE         0x3CD00000
#define N45_I2S1_DMA_REQ_ID   2            /* dmac1 */
#define N45_SPI1_BASE         0x3CE00000
#define N45_SPI2_BASE         0x3D200000
#define N45_IIS0_BASE         0x3D400000
#define N45_TIMER1_BASE       0x3E200000
#define N45_WATCHDOG_BASE     0x3E300000
#define N45_GPIO_BASE         0x3E400000
#define N45_CHIPID_BASE       0x3E500000
#define N45_ENGINE_8900_BASE  0x3F000000

/* Bootrom jump-table slots iBoot-204 calls for 8900 image handling. */
#define N45_VROM_JT_8900_VERIFY 0x2000008c
#define N45_VROM_JT_8900_DECRYPT 0x20000090

#include "hw/arm/ipod_touch_lis302dl.h"

typedef struct IPodTouch1GMachineState {
    MachineState parent;
    ARMCPU *cpu;
    AddressSpace *nsas;
    qemu_irq *irq[2];
    PL192State *vic0;
    PL192State *vic1;
    Clock *sysclk;
    IPodTouchSYSICState *sysic;
    IPodTouchGPIOState *gpio;
    Pcf50633State *pmu;
    IPodTouchMultitouchState *mt;
    IPodTouchLCDState *lcd;
    S5L8900FMCState *fmc;
    S5L8900ADMState *adm;

    char *bootrom_path;
    char *iboot_path;
    char *nand_path;
    char *nand_overlay;
    uint32_t tvout_workaround;
    bool usb_wrangler_quirk;
    bool usb_wrangler_quirk_done;

    bool kbd_cmd, kbd_shift;

    char *usb_tcp_addr;              /* host bridge (usbmuxd-qemu) host:port, empty = no link */
    QEMUTimer *pwroff_timer;         /* system_powerdown: the hold-and-slide gesture */
    int pwroff_phase, pwroff_step;

    /* guest services: the GL bridge (QC_GLES*) and guest-package delivery, on the QEMU_CALL cp15 register */
    GuestPackage pkg;
    bool gles_debug;
    bool wifi;                       /* the Marvell 88W8686 on the SDIO bus (default on) */
    char *wifi_mac;                  /* the card's EEPROM MAC ("wifi-mac", the unit identity's) */
    LIS302DLState *accel;            /* the LIS302DL on I2C0; it keeps the attitude (accel-pitch/-roll/-pose) */
} IPodTouch1GMachineState;

#endif
