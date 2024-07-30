#include "hw/arm/ipod_touch_lis302dl.h"

static int lis302dl_event(I2CSlave *i2c, enum i2c_event event)
{
    return 0;
}

static uint8_t lis302dl_recv(I2CSlave *i2c)
{
    LIS302DLState *s = LIS302DL(i2c);
    printf("Reading accelerometer register %d\n", s->cmd);

    switch(s->cmd) {
        case ACCEL_WHOAMI:
            return ACCEL_WHOAMI_VALUE;
	case ACCEL_OUT_X:
	    return s->ctrl_reg1 == 1 ? s->out_x : 0;
	case ACCEL_OUT_Y:
	    return s->ctrl_reg1 << 1 == 1 ? s->out_y : 0;
	case ACCEL_OUT_Z:
	    return s->ctrl_reg1 << 2 == 1 ? s->out_z : 0;
	case ACCEL_CTRL_REG1:
	    return s->ctrl_reg1;
        case ACCEL_CTRL_REG2:
    	    return s->ctrl_reg2;
	case ACCEL_CTRL_REG3:
	    return s->ctrl_reg3;	    
        default:
	    printf("%s Unknown register %d\n", __func__, s->cmd);
            return 0;
    }
}

static int lis302dl_send(I2CSlave *i2c, uint8_t data)
{
    LIS302DLState *s = LIS302DL(i2c);
    s->cmd = data;
    return 0;
}

static void lis302dl_init(Object *obj)
{

}

static void lis302dl_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = lis302dl_event;
    k->recv = lis302dl_recv;
    k->send = lis302dl_send;
}

static const TypeInfo lis302dl_info = {
    .name          = TYPE_LIS302DL,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = lis302dl_init,
    .instance_size = sizeof(LIS302DLState),
    .class_init    = lis302dl_class_init,
};

static void lis302dl_register_types(void)
{
    type_register_static(&lis302dl_info);
}

type_init(lis302dl_register_types)
