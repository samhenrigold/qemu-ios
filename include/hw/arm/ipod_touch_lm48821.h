#ifndef HW_LM48821_H
#define HW_LM48821_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"

/* National LM48821 amplifier, I2C 0x76, single-byte gain/mute control. */

#define TYPE_LM48821 "lm48821"
OBJECT_DECLARE_SIMPLE_TYPE(LM48821State, LM48821)

typedef struct LM48821State {
    I2CSlave i2c;
    uint8_t control;
} LM48821State;

double lm48821_gain(uint8_t control, unsigned channel);

#endif
