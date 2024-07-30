#include "hw/arm/ipod_touch_isl29003dl.h"

static int isl29003dl_event(I2CSlave *i2c, enum i2c_event event)
{
    ISL29003DLState *s = ISL29003DL(i2c);
   
    printf("%s Got event %d\n", __func__, event);

    if (event == I2C_START_SEND)
	s->ready = 1;
    return 0;
}

static uint8_t isl29003dl_recv(I2CSlave *i2c)
{
    ISL29003DLState *s = ISL29003DL(i2c);
    printf("Reading lightsensor register %d\n", s->cmd);

    int res = 0;

    switch(s->cmd) {
	case LIGHTSENSOR_COMMAND:
		res = s->command;
		break;
	case LIGHTSENSOR_CONTROL:
		res = s->ctrl;
		break;
	case LIGHTSENSOR_LSB_SENSOR:
		res = s->lsb_sensor;
		break;
	case LIGHTSENSOR_MSB_SENSOR:
		res = s->msb_sensor;
		break;
	default:
		res = 0;
		printf("%s unknown register %d\n", __func__, s->cmd);
    }

    s->cmd += 1;
    return res;
}

static int isl29003dl_send(I2CSlave *i2c, uint8_t data)
{
    ISL29003DLState *s = ISL29003DL(i2c);

    if (s->ready)
    {
	s->curreg = data;
	s->ready = 0;
	printf("%s Lightsensor ready\n", __func__);
    }
    else
    {
	switch(s->curreg++)
	{
		case LIGHTSENSOR_COMMAND:
			s->command = data;
			if ((s->command >> 7) == 1)
			{
				s->lsb_sensor = 0xE;
				s->msb_sensor = 0xE << 4;
				s->it_hi = 0xF;
				s->it_lo = 0x0;
			}
			break;
		default:
			printf("%s unknown register %d\n", __func__, s->curreg-1);
	}
    }
    printf("Writing lightsensor data %d\n", s->cmd);
    return 0;
}

static void isl29003dl_init(Object *obj)
{
}

static void isl29003dl_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = isl29003dl_event;
    k->recv = isl29003dl_recv;
    k->send = isl29003dl_send;
}

static const TypeInfo isl29003dl_info = {
    .name          = TYPE_ISL29003DL,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = isl29003dl_init,
    .instance_size = sizeof(ISL29003DLState),
    .class_init    = isl29003dl_class_init,
};

static void isl29003dl_register_types(void)
{
    type_register_static(&isl29003dl_info);
}

type_init(isl29003dl_register_types)
