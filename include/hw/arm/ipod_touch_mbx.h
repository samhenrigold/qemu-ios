#ifndef HW_ARM_IPOD_TOUCH_MBX_H
#define HW_ARM_IPOD_TOUCH_MBX_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_IPOD_TOUCH_MBX "ipodtouch.mbx"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMBXState, IPOD_TOUCH_MBX)

typedef struct IPodTouchMBXState {
    SysBusDevice busdev;
    MemoryRegion iomem1;
    MemoryRegion iomem2;
    uint64_t addr;
    bool alreadypatched;

    /* irq_enabled (the mbx-irq machine option, default on) gates the verified
     * MMU request/ack mirror in ipod_touch_mbx1_read. */
    qemu_irq irq;
    bool irq_enabled;
    uint32_t status;
} IPodTouchMBXState;

void ipod_touch_mbx_set_patch_usb_gate(bool enabled);

#endif
