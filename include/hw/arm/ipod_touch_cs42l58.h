#ifndef HW_CS42L58_H
#define HW_CS42L58_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"

#define TYPE_CS42L58                 "cs42l58"
#define TYPE_CS42L59                 "cs42l59"   /* + power-down status, rate in 0x0a */
#define TYPE_CS42L61                 "cs42l61"   /* the 3GS's; its output volume, as cs42l58_output_gain reads it */
OBJECT_DECLARE_SIMPLE_TYPE(CS42L58State, CS42L58)

typedef struct CS42L58State {
	I2CSlave i2c;
	uint32_t cmd;
	bool have_cmd;
	bool autoinc;
	uint8_t regs[128];
	bool pdn_status;     /* CS42L59: 0x38 bit 3 reports power-down done */
	uint8_t rate_reg;    /* the register whose low five bits select LRCLK */
	Clock *lrclk;
	uint8_t volume;      /* CS42L58_VOLUME_*: how cs42l58_output_gain reads the output volume */
} CS42L58State;

enum {
	CS42L58_VOLUME_NONE,     /* volume not modeled: unity */
	CS42L58_VOLUME_CS42L59,
	CS42L58_VOLUME_CS42L61,
};

/* The linear gain the codec's output volume (and mute) applies to the PCM it is sent. */
double cs42l58_output_gain(CS42L58State *s);

#endif
