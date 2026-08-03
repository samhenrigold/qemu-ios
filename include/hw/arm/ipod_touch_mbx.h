#ifndef HW_ARM_IPOD_TOUCH_MBX_H
#define HW_ARM_IPOD_TOUCH_MBX_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "qemu/timer.h"

#define TYPE_IPOD_TOUCH_MBX "ipodtouch.mbx"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMBXState, IPOD_TOUCH_MBX)

typedef struct IPodTouchMBXState {
    SysBusDevice busdev;
    MemoryRegion iomem1;
    MemoryRegion iomem2;
    uint64_t addr;
    bool mmu_written;   /* has the guest ever driven MBX_MMU_CTRL_REG? */
    bool alreadypatched;

    /* irq_enabled (the mbx-irq machine option, default on) gates the verified
     * MMU request/ack mirror in ipod_touch_mbx1_read. */
    qemu_irq irq;
    bool irq_enabled;
    uint32_t status;

    /*
     * IT_MBX_COMPLETE: 3.1.3's SpringBoard submits work and then sleeps on the
     * MBX device's command gate waiting for a completion interrupt that this
     * model never raised. Track the driver's interrupt mask (0x130) and, while
     * it is armed, post completions on a timer.
     */
    bool complete_shim;
    uint32_t int_mask;
    QEMUTimer *complete_timer;
} IPodTouchMBXState;

void ipod_touch_mbx_set_patch_usb_gate(bool enabled);

#endif
