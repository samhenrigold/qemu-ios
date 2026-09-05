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

/*
 * A stream job: the driver fills the 0x938-0x97c descriptor block, writes the
 * command at 0x99c and then kicks it at 0x984/0x988. Those are the only writes
 * that mean "start work", i.e. the only ones that should raise the completion
 * interrupt.
 */
#define AMC_JOB_CMD         0x99c
#define AMC_JOB_GO          0x984
#define AMC_JOB_GO2         0x988

/*
 * Per-channel stream position, read through the accessor at VA 0xc0611864 as
 * 0xa44 + n * 0x14. Reads back 0. Making it advance was tried twice and is a
 * dead end: it is not an input to the self test's byte-count assertion at all
 * (see the result block below). What it does feed is the ring-buffer copy at
 * VA 0xc060e36c, which turns it into a source address inside the buffer
 * aperture -- so it matters for moving PCM, not for passing the self test.
 */
#define AMC_POS_BASE        0xa44
#define AMC_POS_STRIDE      0x14

/*
 * The buffer aperture. Device tree reg range 2 of /arm-io/amc translates to
 * physical 0x22000000 size 0x30000; the machine already backs it, as part of
 * the 1 MB "llb" RAM allocated at the same address. The engine's own memory.
 */
#define AMC_BUF_BASE        0x22000000
#define AMC_BUF_SIZE        0x30000

/*
 * The result block, at a FIXED OFFSET in that aperture -- no register carries
 * its address, because the engine already owns the memory. Measured on the
 * running guest: AppleAMC_r2's [this+0x48c] is 0xea744000, and
 * cpu_get_phys_page_debug resolves that to physical 0x22028000.
 *
 * Layout, from the driver's use of it. [this+0x490] is exactly base + 0x100
 * and is the payload cursor, so the header is 0x100 bytes and the payload
 * follows it:
 *
 *     +0x02  halfword  output buffer count -- initialization fails if > 2
 *                                       (VA 0xc060c748)
 *     +0x04  halfword  buffer capacity in S16 samples
 *
 * Assertions 934 and 935 (AppleAMCDriver_r2.cpp) are
 * `table[[this+0x7c]] == sample_capacity << 1`. The left-hand side is a CONSTANT
 * out of the driver's own __DATA -- table at VA 0xc0626ebc holds
 * { 0x1200, 0x1000, 0x2000, 0x4000, 0x140, 0x140 } and [this+0x7c] is 1 here,
 * so it is 0x1000 -- which means the sample capacity in this header is the only
 * value hardware contributes to either assertion.
 */
#define AMC_RESULT_OFFSET   0x28000
#define AMC_RESULT_BUFFERS 0x02        /* halfword, must be <= 2 */
#define AMC_RESULT_CAPACITY   0x04        /* halfword */

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
    bool codec_decode;
    uint32_t pending;
    void *decoder;
    QEMUTimer *decode_timer;
} IPodTouchAMCState;

#endif
