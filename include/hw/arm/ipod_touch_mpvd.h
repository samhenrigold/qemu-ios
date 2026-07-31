#ifndef HW_ARM_IPOD_TOUCH_MPVD_H
#define HW_ARM_IPOD_TOUCH_MPVD_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_MPVD "ipodtouch.mpvd"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMPVDState, IPOD_TOUCH_MPVD)

/*
 * mpvd@1600000 has SEVEN reg windows, not one:
 *   0x01600000 0x01610000 0x01620000 0x01630000 0x01641000 0x01650000 0x01660000
 * (each 0x1000, all relative to the arm-io base 0x38000000). The driver touches
 * more than the first, so back the whole 0x39600000..0x3966ffff span as one
 * region - nothing else in the machine lives there, the next occupied address
 * is SYSIC at 0x39700000.
 */
#define MPVD_REG_SIZE 0x70000

typedef struct IPodTouchMPVDState {
    SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t regs[MPVD_REG_SIZE / 4];
} IPodTouchMPVDState;

#endif
