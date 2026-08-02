#ifndef IPOD_TOUCH_TVOUT_H
#define IPOD_TOUCH_TVOUT_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define SDO_CLKCON 0x0
#define SDO_CONFIG 0x8
#define SDO_IRQ 0x280
#define SDO_IRQMASK 0x284

#define MXR_STATUS 0x0
#define MXR_INTSTAT 0xc   /* mixer interrupt status; bit0 = underrun */
#define MXR_CFG 0x4

#define TYPE_IPOD_TOUCH_TVOUT                "ipodtouch.tvout"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchTVOutState, IPOD_TOUCH_TVOUT)

typedef struct IPodTouchTVOutState {
    SysBusDevice parent_obj;

    MemoryRegion mixer1_iomem;
    MemoryRegion mixer2_iomem;
    MemoryRegion sdo_iomem;
    qemu_irq irq;
    /*
     * AppleM2TVOut registers TWO interrupt event sources on the tv-out node
     * (indices 0 and 1); the node's "interrupts" property is <0x1e 0x26>. Only
     * 0x1e was ever wired. IT_TVOUT_VBLANK drives the second one as a periodic
     * vblank so queued swaps can retire.
     */
    qemu_irq irq2;
    bool vblank_shim;
    bool irq2_pending;
    uint32_t mixer1_intstat;
    QEMUTimer *vblank_timer;

    uint32_t mixer1_status;
    uint32_t mixer1_cfg;

    uint32_t mixer2_status;
    uint32_t mixer2_cfg;

    uint32_t sdo_clkcon;
    uint32_t sdo_config;
    uint32_t sdo_irq;
    uint32_t sdo_irq_mask;

    uint32_t irq_count;
} IPodTouchTVOutState;

#endif