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
 * One of the kext's user-client methods reads a single byte from the I2C device
 * and requires it to equal 0x82; this model answers 0x82 so that path can
 * succeed.
 *
 * MEASURED, and recorded here so nobody re-derives it wrongly: the guest reaches
 * demo mode WITHOUT this device.  The kext matches on the DeviceTree node alone
 * (its start() does no I2C), so -isTethered is already true on a stock run, and
 * the 0x82 byte is never actually read -- our slave only ever sees writes
 * (register selects 0x01/0x04).  A single-variable A/B with IT_TETHERED set vs
 * unset, on one warm overlay, was identical: both regenerated the tether
 * preferences after deletion and both logged "Tether dithering: 0".  So this is
 * a faithful model of the absent hardware, but it is NOT what puts the device
 * into demo mode, and the 0x82 path is not yet shown to be reachable on 3.1.3.
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
