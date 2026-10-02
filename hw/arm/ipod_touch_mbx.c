#include "hw/arm/ipod_touch_mbx.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "cpu.h"

/*
 * MMIO trace, off unless MBX_TRACE=1 is in the emulator's environment.
 *
 * The guest PC is logged with every access: AppleMBX.kext is carved out at
 * /Users/shg/Developer/ipod2g-re/kexts/com.apple.driver.AppleMBX.macho with
 * __text at 0xc04e4000, so a PC in that range maps straight onto a disassembly
 * line and tells you which driver routine touched the register.
 */
static int mbx_trace = -1;

static bool mbx_tracing(void)
{
    if (mbx_trace < 0) {
        const char *v = getenv("MBX_TRACE");
        mbx_trace = (v && *v && *v != '0') ? 1 : 0;
    }
    return mbx_trace == 1;
}

static uint32_t mbx_guest_pc(void)
{
    if (!current_cpu) {
        return 0;
    }
    return (uint32_t)ARM_CPU(current_cpu)->env.regs[15];
}

#define MBX_TRACE(fmt, ...)                                                    \
    do {                                                                       \
        if (mbx_tracing()) {                                                    \
            fprintf(stderr, "[MBX] pc=0x%08x " fmt "\n", mbx_guest_pc(),        \
                    ##__VA_ARGS__);                                             \
        }                                                                      \
    } while (0)

/*
 * Completion shim for the unemulated MBX.
 *
 * The GPU itself is not emulated and is out of scope -- the MBX Lite command
 * format is proprietary and essentially un-reverse-engineered. But the hang it
 * causes is not a rendering problem: completion is interrupt-driven, and the
 * machine wired no interrupt to the MBX at all. An app that submits work then
 * sleeps waiting for the completion interrupt never wakes.
 *
 * Evidence: tracing every MBX access while launching a real OpenGL ES app
 * (AwesomeBall) shows 34 accesses ending in a submission write to 0x130, and
 * then silence -- the guest is blocked, not polling.
 *
 * So raise the interrupt shortly after a submission and let the driver's
 * handler run. The interrupt number is 0x35, taken from the mbx node's
 * "interrupts" property in a real device's ioreg dump.
 *
 * This cannot make anything render. The intent is only that the guest stops
 * waiting forever, so the UI survives an app that touches the GPU.
 */
/*
 * What the driver actually does with these registers, read out of
 * com.apple.driver.AppleMBX (__text at 0xc04e4000).
 *
 *   0x130  interrupt MASK/enable. The driver writes 0x867c and caches the same
 *          value at this->0x140 (c04e8988-c04e89a4). It is not a doorbell, so
 *          arming a completion off a write here fires off the driver's power-on
 *          sequence, not off a frame.
 *   0x12c  interrupt STATUS. Bit 0x40 also doubles as "idle": c04e8624 spins on
 *          it before programming the engine.
 *   0x134  write-1-to-clear for 0x12c. c04e8998 writes 0x7ff to clear all.
 *
 * The ISR is at c04ea478:
 *
 *   if (this->0x214 != 2) return;                  <-- bails with no MMIO at all
 *   pending = [0x12c] & this->0x140;
 *   [0x134] = pending;
 *   ... 0x400 -> 2D sync, 0x10/0x20/0x200 -> state machine ...
 *   0x4 -> this->0x13c,  0x8 -> this->0x13d,  0x40 -> this->0x13e
 *   if (0x13c && 0x13d && 0x13e) -> frame done
 *   if (anything handled) bl c04e9b00               <-- queue processor
 *
 * and c04e9b00 eventually reaches c04e9898, which does
 *
 *   this->0x208 = completedCommand->0x48;
 *   commandGate->commandWakeup(&this->0x208);
 *
 * waking waitForTimeStamp (c04e8058), which commandSleeps on &this->0x208 until
 * it reaches the requested timestamp. So a faithful completion has to raise the
 * interrupt with 0x4|0x8|0x40 set in 0x12c *while* 0x130 is armed and the device
 * state this->0x214 is 2. The current shim raises it 1ms later, by which time
 * the driver's idle path (c04e5cc0) has written 0x130 <- 0 and this->0x214 <- 0,
 * so the ISR returns immediately -- which is exactly the observed "interrupt
 * fires, guest ignores it, zero MBX accesses".
 */
#define MBX_MMU_CTRL_REG 0x1020
#define MBX_MMU_ENABLE   0x00000001   /* driver's request */
#define MBX_MMU_ACK      0x00010000   /* hardware's acknowledgement */
#define MBX_SUBMIT_REG   0x130        /* interrupt mask, per the notes above */
#define MBX_STATUS_REG   0x12c
#define MBX_INTCLR_REG   0x134        /* write-1-to-clear for 0x12c */

/* 0x4 | 0x8 | 0x40: the three the ISR folds into "frame done". */
#define MBX_INT_FRAME_DONE 0x4c

/*
 * Bit 10: the 2D completion. Distinct from the 0x4c 3D trio above - the ISR
 * tests it first, before any of them, and it is the only source of the
 * driver's "2D idle" flag. Nothing in this model set it, so any guest waiting
 * on 2D completion waited forever; that is why MBX 2D work cannot currently
 * make progress even with LK_ENABLE_MBX2D=1.
 *
 * Verified against the real ISR: 2.1.1 AppleMBX c04ea478 and 3.1.3 (7E18)
 * kernelcache c075c4e0 are structurally identical, and it is bit 0x20 - not
 * 0x400 - that grows the TA parameter buffer, which is the actual TA overflow.
 */
#define MBX_INT_2D_SYNC    0x400

/*
 * How often to post a completion while the driver's mask is armed. The ISR
 * bails out unless the device object is in its running state, so a completion
 * posted at the wrong moment is simply dropped; repeating means the driver
 * eventually sees one once it is ready. 16 ms is roughly a frame and keeps the
 * interrupt rate far too low to storm.
 */
#define MBX_COMPLETE_PERIOD_NS (16 * 1000 * 1000)

static uint64_t ipod_touch_mbx1_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;
    uint32_t val;

    switch(addr)
    {
        case MBX_STATUS_REG:
            /* 0x40 was pinned here unconditionally; keep that as the base so
             * behaviour is unchanged when the shim is off.
             *
             * 0x100 is pinned for the same reason, and it is what stopped
             * Doodle Jump dead. DECODED FROM THE HANG, not guessed: with the
             * app on screen the vCPU sat at 100% with PC pinned at 0xc075ad18,
             * which disassembles to
             *
             *     ldr r3, [r2, #0x12c]   ; MBX status
             *     tst r3, #0x100
             *     beq .-8                ; spin until bit 8 is set
             *
             * and the instructions immediately after the loop load r2=0x100,
             * r1=0x134 to acknowledge it through the write-1-to-clear register.
             * So 0x100 is a completion the driver waits on and then clears, and
             * nothing in this model ever set it -- the same shape as the 2D-sync
             * gap noted above for 0x400.
             *
             * Worth knowing WHY this only surfaced now: it is not a regression.
             * Improvements to the GLES layer let the app render further than it
             * ever had (its background draws now), which walked it into an MBX
             * path we had never reached, let alone modelled.
             */
            val = 0x40 | 0x100 | s->status;
            /* STATUS is observational. The driver acknowledges only its
             * enabled pending events through the separate W1C register. */
            break;
        case 0xf00:
            val = (2 << 0x10) | (1 << 0x18); // seems to be some kind of identifier
            break;
        case MBX_MMU_CTRL_REG:
            /*
             * Bit 0 is the driver's enable *request*; bit 16 is the hardware's
             * *acknowledgement*. AppleMBXMMU drives them as a handshake:
             *
             *   enable  (0xc04ee224): set bit 0,   spin until bit 16 sets
             *   disable (0xc04ee770): clear bit 0, spin until bit 16 clears
             *
             * Echoing writes back unchanged satisfies the enable loop by
             * accident -- writing 1 to bit 0 leaves the stale bit 16 set -- but
             * makes the disable loop spin forever, because nothing ever clears
             * bit 16. That is the real hang behind "OpenGL ES apps freeze":
             * AwesomeBall tears the MMU down on startup and never comes back,
             * burning ~21M reads of this one register. It is not the completion
             * interrupt at all.
             *
             * So acknowledge the request: mirror bit 0 into bit 16.
             */
            val = s->addr;
            if (s->irq_enabled) {
                val = (val & ~MBX_MMU_ACK) | ((val & MBX_MMU_ENABLE) ? MBX_MMU_ACK : 0);
            }
            if (!s->mmu_written) {
                /*
                 * Only before the guest has ever driven this register does
                 * "ready" make sense. Testing val == 0 instead meant the
                 * fallback also fired on a legitimate *disable* request
                 * (bit 0 clear -> mirror gives 0), so bit 16 never cleared and
                 * AppleMBX's MMU teardown loop spun forever - 100% of guest CPU
                 * in the register accessor at AppleMBX+0xfb0.
                 */
                val = MBX_MMU_ACK;
            }
            break;
        case MBX_SUBMIT_REG:
            /* The mask reads back: 1.x's AppleMBX re-arms it read-modify-write (|= bits | 0x8000). */
            val = s->int_mask;
            break;
        default:
            val = 0;
            break;
    }
    MBX_TRACE("mbx1 rd  [0x%06x] -> 0x%08x", (uint32_t)addr, val);
    return val;
}

/* The line follows the unmasked status (the software interrupt, the completion shim). */
static void ipod_touch_mbx_update_irq(IPodTouchMBXState *s)
{
    if (s->irq) {
        qemu_set_irq(s->irq, (s->status & s->int_mask) != 0);
    }
}

static void ipod_touch_mbx1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;
    MBX_TRACE("mbx1 wr  [0x%06x] <- 0x%08x", (uint32_t)addr, (uint32_t)val);

    switch(addr)
    {
	case MBX_MMU_CTRL_REG:
	    s->addr = val;
	    s->mmu_written = true;
	    break;
	case MBX_STATUS_REG:
	    /*
	     * A write sets status bits: the software interrupt. 1.x's AppleMBX raises bit 0 this way
	     * (3A101a c03aaa18) so that its own ISR runs the command queue, which is what releases a
	     * display swap LayerKit tied to the GPU (mbx2DSwapNotification). Nothing answered it, and
	     * under LK_ENABLE_OGL=1 the fourth swap waited forever.
	     */
	    s->status |= (uint32_t)val;
	    ipod_touch_mbx_update_irq(s);
	    break;
	case MBX_SUBMIT_REG:
	    s->int_mask = val;
	    if (s->complete_shim) {
	        if (val) {
	            timer_mod(s->complete_timer,
	                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MBX_COMPLETE_PERIOD_NS);
	        } else {
	            timer_del(s->complete_timer);
	            s->status = 0;
	        }
	    }
	    ipod_touch_mbx_update_irq(s);
	    break;
	case MBX_INTCLR_REG:
	    s->status &= ~(uint32_t)val;
	    ipod_touch_mbx_update_irq(s);
	    break;
    }
}

/* Post a completion while the driver's interrupt mask is armed. */
static void ipod_touch_mbx_complete(void *opaque)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;

    if (!s->int_mask) {
        return;
    }
    /*
     * Gated on the mask the driver itself armed at 0x130, so a guest that
     * never enables 2D never sees the 2D bit.
     */
    s->status |= (MBX_INT_FRAME_DONE | MBX_INT_2D_SYNC) & s->int_mask;
    if (s->status && s->irq) {
        qemu_irq_raise(s->irq);
    }
    timer_mod(s->complete_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MBX_COMPLETE_PERIOD_NS);
}

static uint64_t ipod_touch_mbx2_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t val = 0;

    switch(addr)
    {
        case 0xC:
            /* Reset request completes synchronously; no guest-memory edits. */
	    break;
	case 0x4:
	    val = 0xFF;
	    break;
        default:
            break;
    }
    MBX_TRACE("mbx2 rd  [0x%03x] -> 0x%08x", (uint32_t)addr, val);
    return val;
}

static void ipod_touch_mbx2_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MBX_TRACE("mbx2 wr  [0x%03x] <- 0x%08x", (uint32_t)addr, (uint32_t)val);
}

static const MemoryRegionOps ipod_touch_mbx1_ops = {
    .read = ipod_touch_mbx1_read,
    .write = ipod_touch_mbx1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps ipod_touch_mbx2_ops = {
    .read = ipod_touch_mbx2_read,
    .write = ipod_touch_mbx2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_mbx_init(Object *obj)
{
    IPodTouchMBXState *s = IPOD_TOUCH_MBX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /*
     * IT_MBX_RAM: back the 16 MB MBX aperture with real RAM instead of pure
     * MMIO. 3.1.3's AppleMBX builds an IOMemoryDescriptor for its surfaces and
     * calls prepare() (which wires physical page frames); over an init_io
     * region there are no page frames, and the driver logs
     *   "MBX: Failed to add a surface mapping because prepare() failed ..."
     * so no surface is ever mapped and nothing composites to the framebuffer.
     * Bring-up probe: RAM makes the aperture wireable, at the cost of the
     * register request/ack mirror on this bank.
     */
    if (getenv("IT_MBX_RAM")) {
        memory_region_init_ram(&s->iomem1, obj, "mbx1_ram", 0x1000000, &error_fatal);
    } else {
        memory_region_init_io(&s->iomem1, obj, &ipod_touch_mbx1_ops, s, TYPE_IPOD_TOUCH_MBX, 0x1000000);
    }
    sysbus_init_mmio(sbd, &s->iomem1);
    memory_region_init_io(&s->iomem2, obj, &ipod_touch_mbx2_ops, s, TYPE_IPOD_TOUCH_MBX, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem2);

    sysbus_init_irq(sbd, &s->irq);

    s->complete_shim = getenv("IT_MBX_COMPLETE") != NULL;
    if (s->complete_shim) {
        s->irq_enabled = true;
        s->complete_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         ipod_touch_mbx_complete, s);
    }
}

/*
 * Warm resets have to clear the MMU handshake.
 *
 * addr backs register 0x1020, whose bit 0 is the driver's MMU enable request.
 * The guest clears it on the way down, so without a reset the next boot starts
 * with the MMU marked disabled and AppleMBXDevice initialises against the
 * previous boot's state. That matters beyond the GPU: CoreSurface uses the MBX
 * as its swap device ("AppleMBX: Using AppleM2CLCD as legacy swap device"), so
 * a half-initialised MBX stops SpringBoard ever programming its framebuffer
 * into the display controller -- the panel stays on the boot logo even though
 * SpringBoard is running and has attached to IOMobileFramebuffer.
 */
static void ipod_touch_mbx_reset(DeviceState *dev)
{
    IPodTouchMBXState *s = IPOD_TOUCH_MBX(dev);

    s->addr = 0;
    s->mmu_written = false;
    s->status = 0;
    /* The completion shim's mask and its timer are part of the interrupt
     * state. Zeroing the mask without disarming the timer left a completion
     * scheduled against a mask the new boot never wrote; disarming without
     * zeroing the mask would let the shim re-arm from stale state. */
    s->int_mask = 0;
    if (s->complete_timer) {
        timer_del(s->complete_timer);
    }
    if (s->irq) {
        qemu_irq_lower(s->irq);
    }
}

/* irq_enabled and complete_shim come from machine options and the environment,
 * not from the guest, so they are configuration rather than state. The
 * completion timer is re-armed by the next masked write from the guest -- and
 * it does not exist at all unless the shim is on, so migrating the pointer
 * would trip vmstate's "array with a NULL base" assertion and kill the source
 * QEMU mid-save. Measured: that is exactly what it did. */
static const VMStateDescription vmstate_ipod_touch_mbx = {
    .name = "ipod_touch_mbx",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(addr, IPodTouchMBXState),
        VMSTATE_BOOL(mmu_written, IPodTouchMBXState),
        VMSTATE_UNUSED(1), /* Retired guest-patch latch; keep v1 stream layout. */
        VMSTATE_UINT32(status, IPodTouchMBXState),
        VMSTATE_UINT32(int_mask, IPodTouchMBXState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_mbx_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ipod_touch_mbx_reset);
    dc->vmsd = &vmstate_ipod_touch_mbx;
}

static const TypeInfo ipod_touch_mbx_type_info = {
    .name = TYPE_IPOD_TOUCH_MBX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMBXState),
    .instance_init = ipod_touch_mbx_init,
    .class_init = ipod_touch_mbx_class_init,
};

static void ipod_touch_mbx_register_types(void)
{
    type_register_static(&ipod_touch_mbx_type_info);
}

type_init(ipod_touch_mbx_register_types)
