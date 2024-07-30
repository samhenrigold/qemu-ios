#ifndef HW_LIS302DL_H
#define HW_LIS302DL_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"

#define TYPE_LIS302DL                 "lis302dl"
OBJECT_DECLARE_SIMPLE_TYPE(LIS302DLState, LIS302DL)

#define ACCEL_WHOAMI	0x0F
#define ACCEL_OUT_X     0x29
#define ACCEL_OUT_Y     0x2B
#define ACCEL_OUT_Z     0x2D
#define ACCEL_CTRL_REG1 0x20
#define ACCEL_CTRL_REG2 0x21
#define ACCEL_CTRL_REG3 0x22

#define ACCEL_WHOAMI_VALUE	0x3B

typedef struct LIS302DLState {
	I2CSlave i2c;
	uint32_t cmd;
	uint32_t out_x;
	uint32_t out_y;
	uint32_t out_z;
	uint16_t ctrl_reg1;
	uint16_t ctrl_reg2;
	uint16_t ctrl_reg3;
} LIS302DLState;

#endif
