#ifndef HW_LM48821_H
#define HW_LM48821_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"

/* National LM48821 speaker amplifier, I2C 0x76. Pure stub: ACKs writes and
 * returns its register file so AppleEmbeddedAudio's amp bring-up succeeds. */

#define TYPE_LM48821 "lm48821"
OBJECT_DECLARE_SIMPLE_TYPE(LM48821State, LM48821)

typedef struct LM48821State {
    I2CSlave i2c;
    uint32_t cmd;
    bool have_reg;
    uint8_t regs[256];
} LM48821State;

#endif
