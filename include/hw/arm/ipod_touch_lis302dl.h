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
#define ACCEL_STATUS    0x27
#define ACCEL_OUT_X     0x29
#define ACCEL_OUT_Y     0x2B
#define ACCEL_OUT_Z     0x2D
#define ACCEL_CTRL_REG1 0x20
#define ACCEL_CTRL_REG2 0x21
#define ACCEL_CTRL_REG3 0x22

#define ACCEL_WHOAMI_VALUE	0x3B

/* CTRL_REG1 per-axis enable bits (public ST LIS302DL datasheet). */
#define ACCEL_CTRL_REG1_XEN 0x01
#define ACCEL_CTRL_REG1_YEN 0x02
#define ACCEL_CTRL_REG1_ZEN 0x04

typedef struct LIS302DLState {
	I2CSlave i2c;
	uint32_t cmd;          /* current register pointer */
	bool pointer_set;      /* have we consumed the sub-address byte this xfer? */
	int8_t out_x;          /* signed acceleration counts (18mg/digit) */
	int8_t out_y;
	int8_t out_z;
	int8_t base_x;         /* steady-state vector, restored after a shake */
	int8_t base_y;
	int8_t base_z;
	uint32_t orientation;  /* last orientation applied via QMP */
	uint16_t ctrl_reg1;
	uint16_t ctrl_reg2;
	uint16_t ctrl_reg3;
	QEMUTimer *shake_timer;
	int shake_ticks;
} LIS302DLState;

/* Host-drivable controls, forwarded from the machine's QMP properties. */
void lis302dl_apply_orientation(LIS302DLState *s, uint32_t o);
void lis302dl_shake(LIS302DLState *s);
void lis302dl_set_axis_value(LIS302DLState *s, char axis, int v);

#endif
