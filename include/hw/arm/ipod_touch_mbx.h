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

    /* Completion shim: see ipod_touch_mbx.c. Inert unless mbx-irq=on. */
    qemu_irq irq;
    QEMUTimer *done_timer;
    bool irq_enabled;
    uint32_t status;
} IPodTouchMBXState;

void ipod_touch_mbx_set_patch_usb_gate(bool enabled);

#endif
