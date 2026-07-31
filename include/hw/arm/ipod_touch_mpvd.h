#ifndef HW_ARM_IPOD_TOUCH_MPVD_H
#define HW_ARM_IPOD_TOUCH_MPVD_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_MPVD "ipodtouch.mpvd"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMPVDState, IPOD_TOUCH_MPVD)

/* mpvd@1600000 in the device tree: arm-io base + 0x01600000, 0x1000 bytes. */
#define MPVD_REG_SIZE 0x1000

typedef struct IPodTouchMPVDState {
    SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t regs[MPVD_REG_SIZE / 4];
} IPodTouchMPVDState;

#endif
