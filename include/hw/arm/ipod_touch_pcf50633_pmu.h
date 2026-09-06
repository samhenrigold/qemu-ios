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
#define PMU_ADC_CONTROL 0x40
#define PMU_ADC_RESULT_LO 0x41
#define PMU_ADC_RESULT_HI 0x42
#define PMU_ADC_DONE (1 << 5)
#define PMU_IRQ_MASK_A 0x07

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
	bool shutdown_armed;  // obsolete host flag; retained for snapshot wire compatibility
    qemu_irq irq;
    QEMUTimer *adc_timer;
    uint16_t adc_values[16];
    uint16_t adc_sample;
    /* IEEE double bits migrate through the integer wire format. */
    union { double drain_rate; uint64_t drain_rate_bits; }; /* Percent/minute. */
    union { double drain_level; uint64_t drain_level_bits; };
    int64_t drain_updated_ns;
    uint8_t charging_mode; /* 0 auto, 1 on, 2 off; external power still required */
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
/*
 * Final power command. Native 7E18 shutdown writes 0x90 after sequencing
 * regulators down and masking interrupts. It must work without host arming.
 *
 * 5F138 idle sleep clears register 0x10 bit 6 (0x7f -> 0x3f), then continues
 * through regulator and wake-mask setup before writing 0x6f=0x80 and printing
 * "pmu go hib". The earlier register-0x10 shutdown heuristic terminated this
 * sleep sequence prematurely. Store those ordinary register writes; only the
 * final power commands confirm shutdown. Filesystem cleanliness still needs
 * separate persistence verification.
 */
#define PMU_SHUTDOWN_REG 0x0a
#define PMU_SHUTDOWN_GO  0x01
#define PMU_STANDBY_CMD  0x6f
#define PMU_STANDBY_GO   0x90

#define PMU_EVENT_A_REG 0x01   // read-to-clear interrupt status (block 0x01..0x03)
#define PMU_EVENT_C_REG 0x03   // EVENT_C: holds the wake-button interrupt bits
#define PMU_STAT_REG    0x19   // live button STATE
#define PMU_STAT_MENU   (1 << 0)
#define PMU_STAT_HOLD   (1 << 1)

// Update live cable status and latch the corresponding power-source event.
void pcf50633_set_usb_cable(Pcf50633State *s, bool attached);
unsigned pcf50633_adc_for_level(unsigned percent);
unsigned pcf50633_level_for_adc(unsigned counts);
void pcf50633_update_battery(Pcf50633State *s);
void pcf50633_set_battery_level(Pcf50633State *s, unsigned level);
void pcf50633_set_battery_drain(Pcf50633State *s, double rate);
void pcf50633_set_battery_adc(Pcf50633State *s, unsigned counts);
void pcf50633_set_charging_mode(Pcf50633State *s, unsigned mode);
// Set/clear the live button STATE bits in reg 0x19.
void pcf50633_set_stat(Pcf50633State *s, uint8_t bits, bool on);
// Latch a wake-button interrupt in EVENT_C (reg 0x03); cleared when iOS reads it.
void pcf50633_latch_wake_event(Pcf50633State *s, uint8_t bits);
// Host-thread-safe evidence that the guest reached the final PMU power-off.
bool pcf50633_guest_shutdown_confirmed(void);

#endif
