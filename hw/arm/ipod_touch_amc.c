/*
 * AMC -- the S5L8720's audio media codec ("amc,s5l8720x").
 *
 * Device tree: /device-tree/arm-io/amc,
 *     reg = <0x00500000 0x00003000  0x1a000000 0x00030000>, interrupts = <0x12>.
 * arm-io's ranges translate those to 0x38500000 (registers, 12 KB) and
 * 0x22000000 (a 192 KB buffer window, which the machine already backs with
 * RAM). Driven by AppleAMC_r2, which owns the IOAudio2 output transformer
 * streams -- i.e. it is on the path of *every* sound the system plays.
 *
 * Why this exists: the register window was not mapped at all. The first access
 * from the driver therefore hit unassigned memory, which QEMU reports as
 * MEMTX_DECODE_ERROR and the ARM core turns into an external data abort -- a
 * kernel-mode abort, so XNU panics and the guest stops dead the moment any
 * sound is played. Even with the window merely present and reading zero the
 * driver still never returns: AppleAMC_r2 waits for its work to finish with an
 * unbounded
 *
 *     do { IOSleep(0); status = read(AMC + 0xa98); }
 *     while ((status & this->pending_mask) == 0);
 *
 * (VA 0xc060c6dc in the 3.1.3 kernelcache; the same shape appears four more
 * times in the driver's self test). There is no timeout and no bail-out, and it
 * runs on the thread that started playback.
 *
 * So this model is deliberately minimal: it is a plain register file plus one
 * piece of behaviour -- an interrupt source reports itself pending as soon as
 * the driver enables it. That is what lets the wait above complete and the
 * driver unwind normally. No audio is produced; the AMC is a hardware AAC/MP3
 * decode/encode engine with its own DE program, MMU and linked-list DMA, and
 * emulating that is a much larger job. Not hanging is the point here.
 */

#include "hw/arm/ipod_touch_amc.h"

#define AMC_REG(off) (s->regs[(off) / 4])

static bool amc_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_AMC_TRACE") != NULL;
    }
    return on;
}

#define AMCT(fmt, ...) do { if (amc_trace()) { \
    fprintf(stderr, "[AMC] " fmt "\n", ##__VA_ARGS__); } } while (0)

/*
 * Level-triggered, with a real acknowledge path so it cannot storm: the line
 * only goes high once the driver has both enabled a source and touched a
 * control register, and any write to the acknowledge register drops it again.
 */
static void amc_update_irq(IPodTouchAMCState *s)
{
    bool level = s->irq_armed &&
                 (s->int_mask[0] != 0 || s->int_mask[1] != 0);

    qemu_set_irq(s->irq, level);
}

/* Returns the controller index for a per-controller register, or -1. */
static int amc_ctrl_of(hwaddr addr, hwaddr base)
{
    if (addr == base) {
        return 0;
    }
    if (addr == base + AMC_CTRL_STRIDE) {
        return 1;
    }
    return -1;
}

static uint64_t ipod_touch_amc_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(opaque);
    uint32_t res;
    int c;

    if ((c = amc_ctrl_of(addr, AMC_INT_MASK)) >= 0) {
        res = s->int_mask[c];
    } else if ((c = amc_ctrl_of(addr, AMC_INT_STATUS)) >= 0 ||
               (c = amc_ctrl_of(addr, AMC_INT_RAWSTATUS)) >= 0) {
        /*
         * Every enabled source reads back as pending. The driver's wait loops
         * are all of the form "spin until (status & my_mask) != 0", so this is
         * the minimum that lets them finish; reporting only what we could
         * genuinely justify (nothing) is what hangs them.
         */
        res = s->int_mask[c];
    } else {
        res = AMC_REG(addr);
    }

    AMCT("R %04x -> %08x", (unsigned)addr, res);
    return res;
}

static void ipod_touch_amc_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(opaque);
    int c;

    AMCT("W %04x <- %08x", (unsigned)addr, (uint32_t)val);
    AMC_REG(addr) = (uint32_t)val;

    if ((c = amc_ctrl_of(addr, AMC_INT_ENABLE)) >= 0) {
        s->int_mask[c] |= (uint32_t)val;
    } else if ((c = amc_ctrl_of(addr, AMC_INT_DISABLE)) >= 0) {
        s->int_mask[c] &= ~(uint32_t)val;
    } else if (addr == AMC_INT_ACK) {
        s->irq_armed = false;      /* acknowledged -- drop the line */
    } else if (addr == AMC_CMD && s->state_handshake) {
        /*
         * IT_AMC_STATE=1, experimental -- NOT part of the proven freeze fix.
         *
         * At VA 0xc0612854 the driver writes command 4 here and then polls
         * AMC + 0x114 up to ten times for (state & 7) == 7 before giving up;
         * at VA 0xc06129f0 it requires (state & 7) == 0 before issuing command
         * 0x10. Both conditions cannot be met by a register that only ever
         * reads back what was written, so answer them: a command completes
         * immediately, and the completion is consumed by the next command.
         */
        AMC_REG(AMC_STATE) = (val == 0x04) ? AMC_STATE_DONE : 0;
    } else {
        s->irq_armed = true;       /* some work was started */
    }

    amc_update_irq(s);
}

static const MemoryRegionOps amc_ops = {
    .read = ipod_touch_amc_read,
    .write = ipod_touch_amc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_amc_reset(DeviceState *dev)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->int_mask, 0, sizeof(s->int_mask));
    s->irq_armed = false;
    s->state_handshake = getenv("IT_AMC_STATE") != NULL;
    amc_update_irq(s);
}

static void ipod_touch_amc_init(Object *obj)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &amc_ops, s, "amc", AMC_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void ipod_touch_amc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_touch_amc_reset;
}

static const TypeInfo ipod_touch_amc_info = {
    .name          = TYPE_IPOD_TOUCH_AMC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchAMCState),
    .instance_init = ipod_touch_amc_init,
    .class_init    = ipod_touch_amc_class_init,
};

static void ipod_touch_amc_register_types(void)
{
    type_register_static(&ipod_touch_amc_info);
}

type_init(ipod_touch_amc_register_types)
