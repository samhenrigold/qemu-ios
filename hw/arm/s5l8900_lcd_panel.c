/*
 * iPod touch 1G LCD panel on SPI1: an ID stub. The display driver probes the
 * panel over SPI with four read commands and only checks the answers; pixels
 * go through the CLCD, not this bus. Stub (fidelity class S): the real panel
 * takes a full init sequence this model swallows. The IDs are the ones
 * devos50 captured (ipod_touch_lcd_panel.c).
 */
#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"
#include "hw/arm/s5l8900_lcd_panel.h"

static uint32_t s5l8900_lcd_panel_transfer(SSIPeripheral *dev, uint32_t value)
{
    S5L8900LCDPanelState *s = S5L8900_LCD_PANEL(dev);

    if (!s->cur_cmd && (value == 0x95 || value == 0xDA || value == 0xDB || value == 0xDC)) {
        s->cur_cmd = value;
        return 0;
    }
    if (s->cur_cmd) {
        uint32_t res = 0;
        switch (s->cur_cmd) {
        case 0x95: res = 0x1;  break;
        case 0xDA: res = 0x71; break;
        case 0xDB: res = 0xC2; break;
        case 0xDC: res = 0x0;  break;
        default: break;
        }
        s->cur_cmd = 0;
        return res;
    }
    return 0;
}

static void s5l8900_lcd_panel_realize(SSIPeripheral *d, Error **errp)
{
}

static void s5l8900_lcd_panel_class_init(ObjectClass *klass, const void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = s5l8900_lcd_panel_realize;
    k->transfer = s5l8900_lcd_panel_transfer;
}

static const TypeInfo s5l8900_lcd_panel_type_info = {
    .name = TYPE_S5L8900_LCD_PANEL,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(S5L8900LCDPanelState),
    .class_init = s5l8900_lcd_panel_class_init,
};

static void s5l8900_lcd_panel_register_types(void)
{
    type_register_static(&s5l8900_lcd_panel_type_info);
}

type_init(s5l8900_lcd_panel_register_types)
