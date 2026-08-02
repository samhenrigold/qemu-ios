#ifndef HW_IPOD_TOUCH_AMC_H
#define HW_IPOD_TOUCH_AMC_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/irq.h"

#define TYPE_IPOD_TOUCH_AMC "ipodtouch.amc"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchAMCState, IPOD_TOUCH_AMC)

#define AMC_MEM_SIZE        0x3000
#define AMC_NUM_CONTROLLERS 2

/* Per-controller interrupt block; controller 1 is controller 0 + 0x80. */
#define AMC_CTRL_STRIDE     0x80
#define AMC_INT_ENABLE      0xa8c   /* W: set mask bits   */
#define AMC_INT_DISABLE     0xa90   /* W: clear mask bits */
#define AMC_INT_MASK        0xa94   /* R: current mask    */
#define AMC_INT_STATUS      0xa98   /* R: pending sources */
#define AMC_INT_RAWSTATUS   0xa9c   /* R: raw pending     */

#define AMC_CMD             0x110   /* W: command  (0x04, 0x10, 0x60 seen) */
#define AMC_STATE           0x114   /* R: bits [2:0] are the engine state  */
#define AMC_STATE_DONE      0x7

#define AMC_INT_ACK         0xc48   /* W: acknowledge (value & 0x7fff) */
#define AMC_INT_ACK_READ    0xc4c   /* R: reads the acknowledge latch  */

typedef struct IPodTouchAMCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[AMC_MEM_SIZE / 4];
    uint32_t int_mask[AMC_NUM_CONTROLLERS];
    bool irq_armed;
    bool state_handshake;
} IPodTouchAMCState;

#endif
