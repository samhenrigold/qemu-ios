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

/*
 * Per-engine register banks. AppleAMC_r2's bank selector (VA 0xc0612010) is a
 * jump table: engines 0-3 are base + (n << 5), then 0x100, 0x180, 0x200, and
 * 0x230 (0x240 on AMC 2.1, which also adds 0x280 and 0x2c0 -- we are 2.0).
 * Within a bank, +0x10 is the command register and +0x14 the state register,
 * whose low three bits the driver polls.
 */
#define AMC_BANK_CMD        0x10
#define AMC_BANK_STATE      0x14
#define AMC_CMD_START       0x04
#define AMC_STATE_DONE      0x7

#define AMC_INT_ACK_MASK    0x7fff

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
