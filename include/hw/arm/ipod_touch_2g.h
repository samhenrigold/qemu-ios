#ifndef HW_ARM_IPOD_TOUCH_H
#define HW_ARM_IPOD_TOUCH_H

#include "exec/hwaddr.h"
#include "hw/boards.h"
#include "hw/intc/pl192.h"
#include "hw/arm/boot.h"
#include "ui/console.h"
#include "cpu.h"
#include "hw/dma/pl080.h"
#include "hw/i2c/ipod_touch_i2c.h"
#include "hw/arm/ipod_touch_timer.h"
#include "hw/arm/ipod_touch_clock.h"
#include "hw/arm/ipod_touch_chipid.h"
#include "hw/arm/ipod_touch_gpio.h"
#include "hw/arm/ipod_touch_sysic.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/arm/ipod_touch_spi.h"
#include "hw/arm/ipod_touch_sha1.h"
#include "hw/arm/ipod_touch_amc.h"
#include "hw/arm/ipod_touch_i2s.h"
#include "hw/arm/ipod_touch_lm48821.h"
#include "hw/arm/ipod_touch_aes.h"
#include "hw/arm/ipod_touch_pke.h"
#include "hw/arm/ipod_touch_unknown1.h"
#include "hw/arm/ipod_touch_wdt.h"
#include "hw/arm/ipod_touch_mpvd.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "hw/arm/ipod_touch_mipi_dsi.h"
#include "hw/arm/ipod_touch_fmss.h"
#include "hw/arm/ipod_touch_mbx.h"
#include "hw/arm/ipod_touch_scaler_csc.h"
#include "hw/arm/ipod_touch_sdio.h"
#include "hw/arm/ipod_touch_tvout.h"
#include "hw/arm/ipod_touch_lis302dl.h"
#include "hw/arm/guest-services/general.h"

#define TYPE_IPOD_TOUCH "iPod-Touch"

#define S5L8720_VIC_N	  2
#define S5L8720_VIC_SIZE  32

#define S5L8720_TIMER1_IRQ 0x7
#define S5L8720_USB_OTG_IRQ 0x13
#define S5L8720_SPI0_IRQ 0x9
#define S5L8720_SPI1_IRQ 0xA
#define S5L8720_SPI2_IRQ 0xB
#define S5L8720_LCD_IRQ 0xD
#define S5L8720_DMAC0_IRQ 0x10
#define S5L8720_DMAC1_IRQ 0x11
#define S5L8720_I2C0_IRQ 0x15
#define S5L8720_SPI3_IRQ 0x1C
#define S5L8720_I2C1_IRQ 0x16
#define S5L8720_TVOUT_SDO_IRQ 0x1E
#define S5L8720_TVOUT_VSYNC_IRQ 0x26
#define S5L8720_SHA1_IRQ 0x28
#define S5L8720_AMC_IRQ 0x12
/* i2s0 interrupts=0x2c but interrupt-parent is the GPIO IC, so this is a
 * GPIO interrupt number: group 0x2c/32 = 1, bit 0x2c%32 = 12. */
#define S5L8720_I2S0_GPIO_INT 0x2C
#define S5L8720_SDIO_IRQ 0x2A
/* From the real device's ioreg: the mbx node's interrupts property is 0x35. */
#define S5L8720_MBX_IRQ 0x35
#define S5L8720_FMSS_IRQ 0x36
#define S5L8720_SPI4_IRQ 0x37

// GPIO interrupts
#define S5L8900_GPIO_G0_IRQ 0x21
#define S5L8900_GPIO_G1_IRQ 0x20
#define S5L8900_GPIO_G2_IRQ 0x1F
#define S5L8900_GPIO_G3_IRQ 0x03
#define S5L8900_GPIO_G4_IRQ 0x02

// key codes
#define KEY_UP        (1 << 7)

#define KEY_P         25
#define KEY_P_DOWN    KEY_P
#define KEY_P_UP      (KEY_P_DOWN | KEY_UP)

#define KEY_H         35
#define KEY_H_DOWN    KEY_H
#define KEY_H_UP      (KEY_H_DOWN | KEY_UP)

#define KEY_MIN       12
#define KEY_MIN_DOWN  KEY_MIN
#define KEY_MIN_UP    (KEY_MIN_DOWN | KEY_UP)

#define KEY_PLUS      13
#define KEY_PLUS_DOWN KEY_PLUS
#define KEY_PLUS_UP   (KEY_PLUS_DOWN | KEY_UP)

extern const int S5L8900_GPIO_IRQS[5];

#define IT2G_CPREG_VAR_NAME(name) cpreg_##name
#define IT2G_CPREG_VAR_DEF(name) uint64_t IT2G_CPREG_VAR_NAME(name)

#define TYPE_IPOD_TOUCH_MACHINE   MACHINE_TYPE_NAME(TYPE_IPOD_TOUCH)
#define IPOD_TOUCH_MACHINE(obj) \
    OBJECT_CHECK(IPodTouchMachineState, (obj), TYPE_IPOD_TOUCH_MACHINE)

// memory addresses
#define VROM_MEM_BASE   0x0
#define INSECURE_RAM_MEM_BASE 0x8000000
#define SECURE_RAM_MEM_BASE   0xB000000
#define FRAMEBUFFER_MEM_BASE  0xFB00000
#define IBOOT_MEM_BASE        0xFF00000
#define SRAM1_MEM_BASE        0x22020000
#define SHA1_MEM_BASE         0x38000000
#define DMAC0_MEM_BASE        0x38200000
#define USBOTG_MEM_BASE       0x38400000
#define AMC_MEM_BASE          0x38500000
#define DMAC1_0_MEM_BASE      0x38700000
#define DISPLAY_MEM_BASE      0x38900000
#define FMSS_MEM_BASE         0x38A00000
#define AES_MEM_BASE          0x38C00000
#define SDIO_MEM_BASE         0x38D00000
#define VIC0_MEM_BASE         0x38E00000
#define VIC1_MEM_BASE         0x38E01000
#define EDGEIC_MEM_BASE       0x38E02000
#define H264_MEM_BASE         0x38F00000
#define SCALER_CSC_MEM_BASE   0x39000000
#define MPVD_MEM_BASE         0x39600000
#define TVOUT_MIXER2_MEM_BASE 0x39100000
#define TVOUT_MIXER1_MEM_BASE 0x39200000
#define TVOUT_SDO_MEM_BASE    0x39300000
#define SYSIC_MEM_BASE        0x39700000
#define DMAC1_1_MEM_BASE      0x39900000
#define MBX1_MEM_BASE         0x3B000000
#define MBX2_MEM_BASE         0x39400000
#define SPI0_MEM_BASE         0x3C300000
#define USBPHYS_MEM_BASE      0x3C400000
#define CLOCK0_MEM_BASE       0x3C500000
#define I2C0_MEM_BASE         0x3C600000
#define TIMER1_MEM_BASE       0x3C700000
#define WDT_MEM_BASE          0x3C800000
#define I2C1_MEM_BASE         0x3C900000
#define I2S0_MEM_BASE         0x3CA00000
#define UART0_MEM_BASE        0x3CC00000
#define SPI1_MEM_BASE         0x3CE00000
#define GPIO_MEM_BASE         0x3CF00000
#define PKE_MEM_BASE          0x3D000000
#define CHIPID_MEM_BASE       0x3D100000
#define SPI2_MEM_BASE         0x3D200000
#define UNKNOWN1_MEM_BASE     0x3D700000
#define MIPI_DSI_MEM_BASE     0x3D800000
#define SPI3_MEM_BASE         0x3DA00000
#define UART1_MEM_BASE        0x3DB00000
#define UART2_MEM_BASE        0x3DC00000
#define UART3_MEM_BASE        0x3DD00000
#define SWI_MEM_BASE          0x3DE00000
#define CLOCK1_MEM_BASE       0x3E000000
#define SPI4_MEM_BASE         0x3E100000

typedef struct {
    MachineClass parent;
} IPodTouchMachineClass;

typedef struct {
	MachineState parent;
	AddressSpace *nsas;
	/* IT_BOOT_ARGS: repeated early writes of the kernel command line.
	 * IT_AMFI_ALLOW_TASKPORT: one-shot patch of the AMFI task-port MAC hooks,
	 * ridden on the same timer. */
	QEMUTimer *boot_args_timer;
	unsigned boot_args_writes;
	bool amfi_patched;
	qemu_irq **irq;
	ARMCPU *cpu;
	PL192State *vic0;
	PL192State *vic1;
	IPodTouchClockState *clock0;
	IPodTouchClockState *clock1;
	IPodTouchTimerState *timer1;
	IPodTouchSPIState *spi0_state;
	IPodTouchSPIState *spi1_state;
	IPodTouchSPIState *spi4_state;
	IPodTouchChipIDState *chipid_state;
	IPodTouchGPIOState *gpio_state;
	IPodTouchSYSICState *sysic;
	synopsys_usb_state *usb_otg;
	IPodTouchUSBPhysState *usb_phys_state;
	IPodTouchSHA1State *sha1_state;
	IPodTouchAESState *aes_state;
	IPodTouchPKEState *pke_state;
	IPodTouchI2CState *i2c0_state;
	IPodTouchI2CState *i2c1_state;
	IPodTouchLCDState *lcd_state;
	IPodTouchMIPIDSIState *mipi_dsi_state;
	IPodTouchFMSSState *fmss_state;
	IPodTouchMBXState *mbx_state;
	IPodTouchScalerCSCState *scaler_csc_state;
	IPodTouchSDIOState *sdio_state;
	IPodTouchTVOutState *tvout_state;
	LIS302DLState *lis302dl_state;
	Clock *sysclk;
	char bootrom_path[1024];
	char nor_path[1024];
	char nand_path[1024];
	char nand_overlay[1024];  /* writable COW overlay dir; empty = writes discarded */
	char boot_args[512];      /* replaces boot-args in the NOR's nvram, in memory only */

	/* USB device-mode options; see ipod_touch_instance_init(). */
	char usb_tcp_addr[256];   /* host:port of the host bridge, empty = disabled */
	bool usb_attached;        /* assert PMU USB cable presence */
	bool mbx_irq;             /* wire the MBX completion interrupt */
	bool usb_patch_mux_gate;  /* patch past the unregistered PTP interface function */
	bool wifi;                /* present a BCM4325 on the SDIO bus */
	IT2G_CPREG_VAR_DEF(REG0);
	IT2G_CPREG_VAR_DEF(REG1);
	IT2G_CPREG_VAR_DEF(QEMU_CALL);

	/*
	 * Host keyboard -> guest text input (see ipod_touch_kbd_event). Bare
	 * printable keys pressed in the display window are converted to unichars
	 * and queued here; the buttons move behind the host Command modifier so
	 * the plain keys are free for typing. The queue is drained by injecting a
	 * call to GraphicsServices' _GSPostSyntheticKeyEvent in the guest.
	 */
	uint16_t kbd_ring[256];   /* queued unichars */
	unsigned kbd_head, kbd_tail;
	bool kbd_cmd;             /* host Command/Meta held */
	bool kbd_shift;           /* host Shift held */

	/*
	 * Scripted "slide to power off" (see ipod_touch_powerdown_tick). QEMU's
	 * system_powerdown arms this; it holds the hold button, waits for
	 * SpringBoard's power-off sheet and drags its slider, which is the only
	 * path iPhone OS 2.1.1 offers to a clean shutdown.
	 */
	QEMUTimer *pwroff_timer;
	int pwroff_phase;
	int pwroff_step;

	/*
	 * On-screen-keyboard typist (see ipod_touch_osk_tick). Enabled with
	 * IT_OSK=1. Instead of queueing unichars for a guest agent to insert,
	 * each typed character is turned into taps on iOS's own on-screen
	 * keyboard, so the text lands in whatever process is frontmost with no
	 * guest code at all.
	 */
	bool osk_enabled;
	QEMUTimer *osk_timer;
	uint16_t osk_pending[64];   /* unichars waiting to be turned into taps */
	unsigned osk_p_head, osk_p_tail;
	uint16_t osk_tapx[64], osk_tapy[64];  /* taps waiting to be delivered */
	unsigned osk_t_head, osk_t_tail;
	int osk_phase;              /* 0 idle, 1 finger down, 2 inter-tap gap */
	int osk_last_x, osk_last_y; /* where the finger currently is */
	bool osk_shifted;           /* shift key currently latched on the OSK */
	bool osk_numeric;           /* the ".?123" page is showing */
} IPodTouchMachineState;

#endif
