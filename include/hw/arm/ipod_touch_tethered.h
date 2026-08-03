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
 * (compatible "tethered,tethereddevice").  The AppleTetheredDevice kext
 * matches that node, and its probe reads a single byte from the device and
 * requires it to equal 0x82 before it treats a demo card as present.  Userspace
 * (SpringBoard's SBTetherController) then opens the driver's user client; when
 * -isTethered returns true, SpringBoard reconfigures itself for on-stage/demo
 * operation (no idle sleep, no auto-lock, dithering off, etc.).
 *
 * This model presents exactly that: an I2C slave at 0x29 whose read register
 * returns the magic 0x82 identification byte, so the guest believes a demo card
 * is attached.  It is only instantiated when IT_TETHERED=1.
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
