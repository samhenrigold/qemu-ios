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

/*
 * RTC. There is no BCD calendar here -- that was a guess carried over from the
 * real PCF50633, and iOS never reads one. AppleD1759PMURTC (the same code in
 * 2.1.1's AppleD1759PMU-36.2 and 3.1.3's -94.7) treats the D1759 RTC as:
 *
 *   0x5C..0x5F  a free-running 32-bit LITTLE-ENDIAN seconds counter, read-only.
 *               3.1.3 reads the four bytes, reads them a second time, and
 *               retries until both reads agree -- a ripple guard, so the four
 *               bytes must be a coherent snapshot.
 *   0x64..0x67  a 32-bit LITTLE-ENDIAN offset in the PMU's battery-backed
 *               scratch, written by the OS when the clock is set.
 *
 * and computes UTC = counter + offset (AppleD1759PMU 3.1.3 VA 0xc06025e4:
 * "bl read_counter; ldr r3,[r4,#0x8c]; add r0,r0,r3"). So the counter is
 * seconds since the Unix epoch as long as the offset is zero, which is what
 * this model reports until the guest writes its own offset.
 */
#define PMU_RTC_COUNTER 0x5C   // .. 0x5F, 32-bit LE seconds, read-only
#define PMU_RTC_OFFSET  0x64   // .. 0x67, 32-bit LE, written by the guest

typedef struct Pcf50633State {
	I2CSlave i2c;
	uint32_t cmd;
	uint32_t ready;
	uint32_t curreg;
	bool addressing;      // next written byte selects the register address
	uint8_t regs[256];    // backing register file so writes read back consistently
	uint32_t rtc_latch;   // snapshot of the RTC counter, taken when 0x5C is read
	bool usb_cable;       // report a USB cable as present (reg 0x04 bit 3)
	bool shutdown_armed;  // a powerdown is in flight; honour the standby write
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
// Power latch. Register 0x10 bit 6 is the "system power is on" latch: iBoot
// sets it while still running from SRAM (the very first 0x10 write of any boot,
// value 0x7f, from pc 0x2200227a), and it stays set for the whole life of the
// machine. iOS clears it -- and only it, 0x7f -> 0x3f, a read-modify-write of
// exactly this bit -- as the last register access of a clean shutdown, right
// before the kernel prints "pmu waiting for stdby" and spins waiting for the
// power rail to collapse. Nothing else in a whole boot-to-shutdown run touches
// bit 6; the other traffic to 0x10 toggles bit 5 (0x7f <-> 0x5f).
//
// So this write is the guest's power-off request, and it is the point at which
// the root volume has already been unmounted -- which is what makes a clean
// shutdown flush HFS+'s in-memory catalog to flash.
#define PMU_PWRLATCH_REG 0x10
#define PMU_PWRLATCH_ON  (1 << 6)

/*
 * Standby command. 3.1.3 does NOT power off by clearing the 0x10 latch the way
 * 2.1.1 does -- traced end to end on 7E18, its shutdown reads 0x10 as 0xe0 and
 * writes 0xe0 straight back, bit 6 untouched. What it does instead, as the very
 * last PMU access of the whole sequence, is write 0x90 here:
 *
 *   ... 0x50..0x5a (scratch saved)  0x30=00 0x31=00 (regulators down)
 *       0x1d=12  0x10 rmw (no change)  0x0a=18  0x33=00 0x34=80 0x31=00
 *       0x07=d1 0x08=ff 0x09=f0 (all interrupts masked)  0x61=00
 *       0x6f=90   <-- and then the guest never touches the PMU again
 *
 * 0x6f is written exactly once in a whole boot-to-shutdown run and never during
 * normal operation, so this write IS the "cut the rails now" request; on real
 * silicon the power simply goes at this point. Without a model for it the guest
 * sat in "pmu waiting for stdby" forever with the root volume still mounted --
 * which is the whole reason a clean shutdown has never worked on 3.1.3, and why
 * everything installed in a session could vanish with it.
 *
 * Honoured only while a powerdown is actually in flight (pcf50633_arm_shutdown,
 * called from the machine's powerdown notifier), so that if any sleep or idle
 * path ever writes the same register it cannot take the machine down with it.
 */
#define PMU_STANDBY_CMD  0x6f
#define PMU_STANDBY_GO   0x90

#define PMU_EVENT_A_REG 0x01   // read-to-clear interrupt status (block 0x01..0x03)
#define PMU_EVENT_C_REG 0x03   // EVENT_C: holds the wake-button interrupt bits
#define PMU_STAT_REG    0x19   // live button STATE
#define PMU_STAT_MENU   (1 << 0)
#define PMU_STAT_HOLD   (1 << 1)

// Set/clear the live button STATE bits in reg 0x19.
void pcf50633_set_stat(Pcf50633State *s, uint8_t bits, bool on);
// Latch a wake-button interrupt in EVENT_C (reg 0x03); cleared when iOS reads it.
void pcf50633_latch_wake_event(Pcf50633State *s, uint8_t bits);
// A powerdown has been requested: honour the guest's standby write when it
// finishes unmounting and sequencing its rails down.
void pcf50633_arm_shutdown(Pcf50633State *s);
// Host-thread-safe evidence that the guest reached the final PMU power-off.
bool pcf50633_guest_shutdown_confirmed(void);

#endif
