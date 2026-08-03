#ifndef HW_IPOD_TOUCH_TETHERED_H
#define HW_IPOD_TOUCH_TETHERED_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"

/*
 * Simulated "demo card" / AppleTetheredDevice.
 *
 * The shipping N72AP DeviceTree carries an I2C node at i2c0 address 0x29
 * (compatible "tethered,tethereddevice").  The AppleTetheredDevice kext matches
 * that node and publishes AppleTetheredDeviceUserClient; SpringBoard's
 * SBTetherController opens it, and when -isTethered returns true SpringBoard
 * reconfigures itself for on-stage/demo operation (no idle sleep, no auto-lock,
 * dithering off, PCForceDemoMaxHBI, etc.).
 *
 * The user client has exactly two external methods.  Selector 0 is the presence
 * test SpringBoard's -isTethered calls, and ITS BODY DIFFERS BY OS VERSION:
 *
 *   2.1.1: I2C READ of one byte, which must equal 0x82, then drive /charger's
 *          function-control_0/1/2 and log "... set 1A".
 *   3.1.3: I2C WRITE of two bytes (0x01, 0x04), and nothing else gates it --
 *          Apple REMOVED the 0x82 identity check.  `cmp rN,#0x82` appears
 *          nowhere in the 3.1.3 kext (verified two ways -- resilient
 *          disassembly and a raw ARM-encoding search -- with the 2.1.1 kext as
 *          a positive control).
 *
 * We answer 0x82 on reads so the 2.1.1 path also works.  On 3.1.3 the byte is
 * never read, and the two writes this model logs are exactly that (1, 4) pair.
 *
 * CONSEQUENCE, measured: on 3.1.3 the entire presence test is "did the write to
 * 0x29 succeed", and our I2C controller never NAKs (IT_I2C_NAK is off by
 * default), so an ABSENT slave still reports success.  -isTethered is therefore
 * true on a stock run and the guest sits in demo mode permanently, with or
 * without this device -- a single-variable A/B (IT_TETHERED set vs unset, one
 * warm overlay) was identical in both arms.  This device only becomes
 * load-bearing under IT_I2C_NAK=1, which should make an uncarded run report
 * false.  Note that flipping IT_I2C_NAK on by default therefore turns demo mode
 * OFF, and the device will begin auto-locking, dimming and idle-sleeping.
 *
 * Only instantiated when IT_TETHERED=1.  IT_TETHERED_TRACE=1 logs every access.
 */

#define IT_TETHERED_MAGIC 0x82

#define TYPE_IPOD_TOUCH_TETHERED "ipod-touch-tethered"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchTetheredState, IPOD_TOUCH_TETHERED)

typedef struct IPodTouchTetheredState {
    I2CSlave i2c;
    uint8_t cmd;      /* last register selected by a write */
    uint8_t control[3]; /* last values the driver drove onto control_0/1/2 */
} IPodTouchTetheredState;

#endif
