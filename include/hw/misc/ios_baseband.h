/*
 * Fake cellular baseband: the QEMU device side. The transport-independent core
 * (and its API) is in ios_baseband_core.h.
 */
#ifndef HW_MISC_IOS_BASEBAND_H
#define HW_MISC_IOS_BASEBAND_H

#include "hw/misc/ios_baseband_core.h"
#include "chardev/char.h"

#define TYPE_IOS_BASEBAND "ios-baseband"

OBJECT_DECLARE_SIMPLE_TYPE(IosBasebandState, IOS_BASEBAND)

struct IosBasebandState {
    DeviceState parent_obj;

    IosBbCore bb;
    Chardev *chr;                 /* created at realize; hand to the UART */
    QEMUTimer *timer;
    uint8_t out[8192];            /* bytes waiting for the UART */
    unsigned out_len;
};

/* The chardev to pass to the UART model as its backend. */
Chardev *ios_baseband_chardev(DeviceState *dev);

#endif
