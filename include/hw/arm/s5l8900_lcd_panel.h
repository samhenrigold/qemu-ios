#ifndef HW_ARM_S5L8900_LCD_PANEL_H
#define HW_ARM_S5L8900_LCD_PANEL_H

#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"

#define TYPE_S5L8900_LCD_PANEL "s5l8900.lcdpanel"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900LCDPanelState, S5L8900_LCD_PANEL)

typedef struct S5L8900LCDPanelState {
    SSIPeripheral ssidev;
    uint32_t cur_cmd;
} S5L8900LCDPanelState;

#endif
