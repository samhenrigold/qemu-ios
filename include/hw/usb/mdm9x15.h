/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_USB_MDM9X15_H
#define HW_USB_MDM9X15_H

#include "chardev/char.h"
#include "hw/core/qdev.h"

/* The modem's UART control link shares its power/runtime state with HSIC. */
Chardev *mdm9x15_chardev(DeviceState *dev);

#endif
