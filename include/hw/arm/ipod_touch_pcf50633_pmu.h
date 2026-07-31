#ifndef HW_PCF50633_PMU_H
#define HW_PCF50633_PMU_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "time.h"

/*
 * Power-source status block. AppleD1759PMUPowerSource reads regs 0x04-0x06 as a
 * three-byte auto-increment block and prints it as "status"; bit 3 of the first
 * byte is USB cable presence. Bisected empirically: setting it flips
 * AppleUSBCableDetect to 1 and _usbConnectType to 4, which is what releases the
 * whole USB device stack. Do NOT force the entire block - that hangs boot on the
 * Apple logo.
 */
#define PMU_PWRSRC_STATUS 0x04
#define PMU_PWRSRC_USB    (1 << 3)

#define TYPE_PCF50633                 "pcf50633"
OBJECT_DECLARE_SIMPLE_TYPE(Pcf50633State, PCF50633)

#define PMU_DSBL1 0x30	
#define PMU_MBCS1 0x4B
#define PMU_ADCC1 0x57

// RTC registers
#define PMU_RTCSC 0x59
#define PMU_RTCMN 0x5A
#define PMU_RTCHR 0x5B
#define PMU_RTCWD 0x5C
#define PMU_RTCDT 0x5D
#define PMU_RTCMT 0x5E
#define PMU_RTCYR 0x5F

typedef struct Pcf50633State {
	I2CSlave i2c;
	uint32_t cmd;
	uint32_t ready;
	uint32_t curreg;
	bool addressing;      // next written byte selects the register address
	uint8_t regs[256];    // backing register file so writes read back consistently
	bool usb_cable;       // report a USB cable as present (reg 0x04 bit 3)
} Pcf50633State;

// The D1759 PMU is itself a nested interrupt controller (device tree pmu@73:
// interrupt-controller, raising GPIO IRQ 0x61). Register map recovered from the
// AppleD1759PMU driver in the 2.1.1 kernelcache:
//
//  0x01/0x02/0x03  EVENT_A/B/C  interrupt status, READ-TO-CLEAR, read as a
//                               3-byte block starting at subaddress 0x01.
//  0x07/0x08/0x09  IRQ_MASK_A/B/C  (1 = masked, 0 = enabled; iOS sets these)
//  0x19            GPIO input STATUS ("STAT" provider): live button level.
//
// The wake buttons live in EVENT_C (reg 0x03) and mirror their bit positions in
// the STAT register (reg 0x19): bit 1 = hold/power, bit 0 = menu/home. On a
// press the PMU latches the EVENT_C bit and raises IRQ 0x61; iOS reads EVENT_A-C
// (clearing them), decodes the bit to a specifier (regIdx*8+bit: hold=0x11,
// menu=0x10), and the handler reads STAT reg 0x19 to confirm the button.
#define PMU_EVENT_A_REG 0x01   // read-to-clear interrupt status (block 0x01..0x03)
#define PMU_EVENT_C_REG 0x03   // EVENT_C: holds the wake-button interrupt bits
#define PMU_STAT_REG    0x19   // live button STATE
#define PMU_STAT_MENU   (1 << 0)
#define PMU_STAT_HOLD   (1 << 1)

// Set/clear the live button STATE bits in reg 0x19.
void pcf50633_set_stat(Pcf50633State *s, uint8_t bits, bool on);
// Latch a wake-button interrupt in EVENT_C (reg 0x03); cleared when iOS reads it.
void pcf50633_latch_wake_event(Pcf50633State *s, uint8_t bits);

#endif
