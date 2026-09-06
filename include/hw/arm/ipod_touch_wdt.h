#ifndef HW_ARM_IPOD_TOUCH_WDT_H
#define HW_ARM_IPOD_TOUCH_WDT_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_WDT "ipodtouch.wdt"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchWDTState, IPOD_TOUCH_WDT)

/* Watchdog control register (offset 0) and the command AppleARMWatchDogTimer's
 * PEHaltRestart handler writes to trigger a full-SoC reset. */
#define WDT_CTRL      0x0
#define WDT_CNT       0x4
#define WDT_RESET_COMMAND 0x100000

typedef struct IPodTouchWDTState {
    SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t ctrl;
    uint32_t cnt;
    bool noreset;
} IPodTouchWDTState;

#endif
