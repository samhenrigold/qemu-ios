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
 *
 * IT_AMC_STATE=1 goes further and answers the handshake. The whole AMC stack
 * starts lazily on the first sound -- a boot with no sound played produces zero
 * AMC accesses -- and what it then runs is the driver's SELF TEST, not a stream.
 * (The kick at 0x984 has lr = 0xc060c6d4, inside the self-test routine, and the
 * 0x938-0x97c descriptor is byte-identical on every cycle and holds no buffer
 * addresses.) Measured against a real sound, Clock > Timer > When Timer Ends >
 * tap a sound, the driver:
 *
 *   - starts engines in banks 0x000 and 0x180 (command 4 -> state 7, 0x10, 0x60)
 *   - fills the descriptor, writes the command at 0x99c, kicks 0x984 then 0x988
 *   - takes the completion interrupt, reads 0xa98/0xb18, acknowledges bit 2 at
 *     0xc48, and tears the job down again
 *   - does that four times, then settles into an IOAudio2 workloop tick writing
 *     the 4-bit selector at 0x1000 (a symptom, not the blocker: ~14.5k writes on
 *     the real arm1176, versus ~870k under -cpu max, which NOPs WFI)
 *
 * and brings the I2S controller up (`enable <- 1`) for the first time, then
 * halts TX. Nothing plays, and the guest's own kernel log says why -- read it
 * with imgtools/klog.py, since -serial only ever carries iBoot:
 *
 *     Assertion failed in ".../AppleAMCDriver_r2.cpp" at line 934   (x4)
 *                                                        line 1095  (x4)
 *                                                        line 1751  (x4)
 *                                                        line 2678  (x4)
 *     AppleEmbeddedAudioDevice: could not start DMA: device is not ready
 *
 * Line 934 is `produced_bytes == frames * 2` -- `cmp r1, r3, lsl #1` against
 * halfword [[r5+0x48c]+4] -- and its failure path returns 0xe00002bc
 * (kIOReturnError). So the self test fails because this model produces no
 * bytes, the embedded-audio layer then refuses to start DMA, and that is
 * precisely why no PL080 channel is ever pointed at the I2S TX FIFO at
 * 0x3ca00010 and no PCM is ever produced.
 *
 * The way forward, and it is narrower than it looks: those assertions compare
 * COUNTS ONLY, never audio content. Passing the self test therefore does not
 * require emulating the AAC/MP3 decoder -- it requires the per-channel position
 * registers at 0xa44 + n*0x14 (accessor VA 0xc0611864, consumer 0xc060e36c) to
 * report the *exact* expected count. An earlier probe that advanced them by an
 * arbitrary 64 or 1024 changed nothing, which is unsurprising against an
 * equality test, and says nothing about whether the right value would work.
 */

#include "hw/arm/ipod_touch_amc.h"
#include "hw/core/cpu.h"
#include "cpu.h"

#define AMC_REG(off) (s->regs[(off) / 4])

/*
 * IT_AMC_PC=<hex offset> logs the guest PC and LR for accesses to that register,
 * which is how you find out *which* driver loop is hammering it. Capped so a
 * spin does not fill the disk.
 */
static void amc_log_caller(hwaddr addr, uint32_t val)
{
    static int64_t want = -2;
    static int budget = 64;
    CPUARMState *env;

    if (want == -2) {
        const char *v = getenv("IT_AMC_PC");
        want = v ? strtoll(v, NULL, 16) : -1;
    }
    if (want < 0 || (hwaddr)want != addr || budget <= 0 || !current_cpu) {
        return;
    }
    budget--;
    env = &ARM_CPU(current_cpu)->env;
    /* Walk the r7 frame chain: each frame is { saved r7, saved lr }. */
    fprintf(stderr, "[AMC] %04x <- %08x  pc=%08x lr=%08x  stack:",
            (unsigned)addr, val, env->regs[15], env->regs[14]);
    uint32_t fp = env->regs[7];
    for (int i = 0; i < 8 && fp; i++) {
        uint32_t frame[2] = { 0, 0 };
        if (cpu_memory_rw_debug(current_cpu, fp, (uint8_t *)frame,
                                sizeof(frame), false) != 0) {
            break;
        }
        fprintf(stderr, " %08x", frame[1]);
        if (frame[0] <= fp) {
            break;              /* not a plausible frame chain any more */
        }
        fp = frame[0];
    }
    fprintf(stderr, "\n");

    /*
     * IT_AMC_DEREF=<hex>: at the probed access, treat r5 as the AppleAMC_r2
     * instance and dump the object at [r5 + <hex>]. The self test's final
     * assertions (AppleAMCDriver_r2.cpp lines 934/935) compare the bytes the
     * engine produced against halfword [[r5+0x48c]+4] << 1, so that is where
     * the expected count lives.
     */
    const char *deref = getenv("IT_AMC_DEREF");
    if (deref) {
        uint32_t off = strtoul(deref, NULL, 16), ptr = 0;
        uint8_t buf[32];
        if (cpu_memory_rw_debug(current_cpu, env->regs[5] + off,
                                (uint8_t *)&ptr, 4, false) == 0 && ptr &&
            cpu_memory_rw_debug(current_cpu, ptr, buf, sizeof(buf), false) == 0) {
            fprintf(stderr, "[AMC] r5=%08x [r5+%x]=%08x ->", env->regs[5],
                    off, ptr);
            for (unsigned i = 0; i < sizeof(buf); i++) {
                fprintf(stderr, " %02x", buf[i]);
            }
            fprintf(stderr, "\n");
        }
    }
}

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
    /*
     * IT_AMC_STATE: the driver never writes the enable register (0xa8c) -- it
     * only ever *disables* everything at init -- so int_mask stays 0 and the
     * line could never rise. That leaves the completion interrupt permanently
     * unserviced, which is what strands the stream: the engine's workloop tick
     * keeps running but nothing ever advances it. Under the handshake, an armed
     * job raises the line regardless of the mask and the acknowledge drops it.
     */
    bool level = s->irq_armed &&
                 (s->state_handshake ||
                  s->int_mask[0] != 0 || s->int_mask[1] != 0);

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

/*
 * If addr is an engine's command register, return that engine's bank base;
 * otherwise -1. See the bank list in the header.
 */
static const uint32_t amc_banks[] = { 0x00, 0x20, 0x40, 0x60,
                                      0x100, 0x180, 0x200, 0x230 };

static int amc_bank_cmd(hwaddr addr)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(amc_banks); i++) {
        if (addr == amc_banks[i] + AMC_BANK_CMD) {
            return amc_banks[i];
        }
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
        /*
         * IT_AMC_STATE: the driver's final wait before a stream can move is
         *
         *     do { IOSleep(0); } while ((read(0xa98) & pending_mask) == 0);
         *
         * and it reaches it having only ever written the *disable* register
         * (0xa90 <- 0x07ffffff, 0xb10 <- 0xffffffff) -- it never enables a
         * source, so int_mask is 0 and the mask-mirroring above answers 0 and
         * the loop never ends. We do not know which bit it is waiting on, so
         * report every source pending; whatever the mask is, it is satisfied.
         */
        if (s->state_handshake && res == 0 && c == 0) {
            /* Controller 0 only: reporting controller 1 pending as well made
             * the driver service a source that does not exist. */
            res = s->irq_armed ? 0xffffffff : 0;
        }
    } else {
        res = AMC_REG(addr);
    }

    AMCT("R %04x -> %08x", (unsigned)addr, res);
    amc_log_caller(addr, res);
    return res;
}

static void ipod_touch_amc_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    IPodTouchAMCState *s = IPOD_TOUCH_AMC(opaque);
    int c;

    AMCT("W %04x <- %08x", (unsigned)addr, (uint32_t)val);
    amc_log_caller(addr, (uint32_t)val);
    AMC_REG(addr) = (uint32_t)val;

    if ((c = amc_ctrl_of(addr, AMC_INT_ENABLE)) >= 0) {
        s->int_mask[c] |= (uint32_t)val;
    } else if ((c = amc_ctrl_of(addr, AMC_INT_DISABLE)) >= 0) {
        s->int_mask[c] &= ~(uint32_t)val;
    } else if (addr == AMC_INT_ACK) {
        s->irq_armed = false;      /* acknowledged -- drop the line */
    } else if (s->state_handshake && amc_bank_cmd(addr) >= 0) {
        /*
         * IT_AMC_STATE=1, experimental -- NOT part of the proven freeze fix.
         *
         * At VA 0xc0612854 the driver writes command 4 to an engine's command
         * register and then polls that engine's state register up to ten times
         * for (state & 7) == 7 before giving up; at VA 0xc06129f0 it requires
         * (state & 7) == 0 before issuing command 0x10. Neither can be met by a
         * register that only reads back what was written, so answer them: a
         * start command completes immediately and any later command clears it.
         *
         * Measured: with only engine 4 (bank 0x100) answered, the driver got
         * its 7 first try and advanced to commands 0x10 and 0x60, then stalled
         * polling engine 5 (bank 0x180, state at 0x194) 24 times. Hence the
         * full bank table rather than a single hard-coded pair.
         */
        AMC_REG(amc_bank_cmd(addr) + AMC_BANK_STATE) =
            (val == AMC_CMD_START) ? AMC_STATE_DONE : 0;
    } else if (!s->state_handshake) {
        s->irq_armed = true;       /* some work was started */
    } else if (addr == AMC_JOB_GO || addr == AMC_JOB_GO2 || addr == AMC_JOB_CMD) {
        /*
         * Only a job start arms the line. Arming on *any* write (which is all
         * the freeze fix needed) turns the completion interrupt into a storm
         * once the line can actually rise: the handler's own register writes
         * (0x804, 0x858) re-arm it the instant it acknowledges at 0xc48.
         *
         * The job is the 0x938-0x97c descriptor block followed by 0x99c and
         * then 0x984/0x988; those three are the only writes that mean "go".
         */
        s->irq_armed = true;
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
