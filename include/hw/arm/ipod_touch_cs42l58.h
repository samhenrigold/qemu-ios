#ifndef HW_CS42L58_H
#define HW_CS42L58_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"

#define TYPE_CS42L58                 "cs42l58"
OBJECT_DECLARE_SIMPLE_TYPE(CS42L58State, CS42L58)

typedef struct CS42L58State {
	I2CSlave i2c;
	uint32_t cmd;
} CS42L58State;

#endif
