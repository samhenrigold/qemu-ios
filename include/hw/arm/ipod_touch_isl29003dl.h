#ifndef HW_ISL29003DL_H
#define HW_ISL29003DL_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "time.h"

#define TYPE_ISL29003DL                 "isl29003dl"
OBJECT_DECLARE_SIMPLE_TYPE(ISL29003DLState, ISL29003DL)

#define LIGHTSENSOR_COMMAND    0x00
#define LIGHTSENSOR_CONTROL    0x01
#define LIGHTSENSOR_IT_HI      0x02
#define LIGHTSENSOR_IT_LO      0x03
#define LIGHTSENSOR_LSB_SENSOR 0x04
#define LIGHTSENSOR_MSB_SENSOR 0x05
#define LIGHTSENSOR_LSB_TIMER  0x06
#define LIGHTSENSOR_MSB_TIMER  0x07

typedef struct ISL29003DLState {
	I2CSlave i2c;
	uint32_t cmd;
	uint32_t command;
	uint32_t ctrl;
	uint32_t curreg;
	uint32_t ready;
	uint32_t it_hi;
	uint32_t it_lo;
	uint32_t lsb_sensor;
	uint32_t msb_sensor;
	uint32_t lsb_timer;
	uint32_t msb_timer;
} ISL29003DLState;

#endif
