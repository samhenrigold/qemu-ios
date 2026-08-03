/*
 * Arm PrimeCell PL080/PL081 DMA controller
 *
 * Copyright (c) 2006 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/dma/pl080.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"

#define PL080_CONF_E    0x1
#define PL080_CONF_M1   0x2
#define PL080_CONF_M2   0x4

#define PL080_CCONF_H   0x40000
#define PL080_CCONF_A   0x20000
#define PL080_CCONF_L   0x10000
#define PL080_CCONF_ITC 0x08000
#define PL080_CCONF_IE  0x04000
#define PL080_CCONF_E   0x00001

#define PL080_CCTRL_I   0x80000000
#define PL080_CCTRL_DI  0x08000000
#define PL080_CCTRL_SI  0x04000000
#define PL080_CCTRL_D   0x02000000
#define PL080_CCTRL_S   0x01000000

static const VMStateDescription vmstate_pl080_channel = {
    .name = "pl080_channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(src, pl080_channel),
        VMSTATE_UINT32(dest, pl080_channel),
        VMSTATE_UINT32(lli, pl080_channel),
        VMSTATE_UINT32(ctrl, pl080_channel),
        VMSTATE_UINT32(conf, pl080_channel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pl080 = {
    .name = "pl080",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(tc_int, PL080State),
        VMSTATE_UINT8(tc_mask, PL080State),
        VMSTATE_UINT8(err_int, PL080State),
        VMSTATE_UINT8(err_mask, PL080State),
        VMSTATE_UINT32(conf, PL080State),
        VMSTATE_UINT32(sync, PL080State),
        VMSTATE_UINT32(req_single, PL080State),
        VMSTATE_UINT32(req_burst, PL080State),
        VMSTATE_UINT8(tc_int, PL080State),
        VMSTATE_UINT8(tc_int, PL080State),
        VMSTATE_UINT8(tc_int, PL080State),
        VMSTATE_STRUCT_ARRAY(chan, PL080State, PL080_MAX_CHANNELS,
                             1, vmstate_pl080_channel, pl080_channel),
        VMSTATE_INT32(running, PL080State),
        VMSTATE_END_OF_LIST()
    }
};

static const unsigned char pl080_id[] =
{ 0x80, 0x10, 0x04, 0x0a, 0x0d, 0xf0, 0x05, 0xb1 };

static const unsigned char pl081_id[] =
{ 0x81, 0x10, 0x04, 0x0a, 0x0d, 0xf0, 0x05, 0xb1 };

/*
 * IT_DMAC_TRACE=1 -- log every PL080 register access with the guest PC.
 *
 * Why this exists: on the iPod touch 2G, AppleARMIISAudio builds and submits a
 * well-formed 61440-byte DMA request to AppleARMPL080DMAC and no channel is
 * ever pointed at the I2S TX FIFO (0x3ca00010). IT_DMA_TRACE only shows channel
 * STARTS, so it cannot distinguish "the DMAC kext never looked at the hardware"
 * from "it looked, did not like an answer, and gave up". This shows the whole
 * conversation, with the PC that made each access, which is the only way to
 * tell those apart.
 *
 * IT_DMAC_TRACE_FROM=<hex> suppresses output until the first access at or after
 * that offset (use 0 for everything). Boot alone produces tens of thousands of
 * NAND transfers on DMAC0, so the interesting window has to be findable.
 */
static const char *pl080_regname(hwaddr offset)
{
    if (offset >= 0x100 && offset < 0x200) {
        static const char *cn[8] = { "SrcAddr", "DestAddr", "LLI", "Control",
                                     "Config", "?5", "?6", "?7" };
        return cn[(offset >> 2) & 7];
    }
    switch (offset >> 2) {
    case 0:  return "IntStatus";
    case 1:  return "IntTCStatus";
    case 2:  return "IntTCClear";
    case 3:  return "IntErrStatus";
    case 4:  return "IntErrClear";
    case 5:  return "RawIntTCStatus";
    case 6:  return "RawIntErrStatus";
    case 7:  return "EnbldChns";
    case 8:  return "SoftBReq";
    case 9:  return "SoftSReq";
    case 10: return "SoftLBReq";
    case 11: return "SoftLSReq";
    case 12: return "Config";
    case 13: return "Sync";
    default: return "?";
    }
}

bool it_dmac_trace_on(void);
bool it_dmac_trace_on(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("IT_DMAC_TRACE") != NULL;
    }
    return cached;
}

static void pl080_trace(PL080State *s, hwaddr offset, uint32_t val, bool write)
{
    uint64_t pc = 0;

    if (!it_dmac_trace_on()) {
        return;
    }
    /* This file is target-independent (libcommon), so the ARM register file is
     * not reachable here; CPUClass::get_pc is the generic way to ask. */
    if (current_cpu && CPU_GET_CLASS(current_cpu)->get_pc) {
        pc = CPU_GET_CLASS(current_cpu)->get_pc(current_cpu);
    }
    fprintf(stderr, "[dmac%d] %c %03x %-14s %08x  pc=%08x\n",
            s->trace_id, write ? 'W' : 'R', (unsigned)offset,
            pl080_regname(offset), val, (uint32_t)pc);
}

/*
 * The interrupt masks are not registers: on a PL080 each channel's own
 * Configuration word carries them (ITC for terminal count, IE for errors), so
 * the masked status registers have to be derived from the live channel state
 * every time they are looked at.
 *
 * This model used to keep tc_mask as a sticky cache, seeded with
 * "s->tc_mask = s->tc_int" so that a pending bit could never be masked out
 * again. That made a terminal count permanent: a driver that finishes with a
 * channel by writing Configuration = 0 -- which on real hardware clears ITC and
 * therefore drops the masked status and the interrupt line -- left this model
 * asserting the line with nothing able to lower it but an IntTCClear the
 * driver had no reason to write. On the iPod touch 2G that stuck bit is why
 * DMAC0's interrupt could not be delivered to the kernel at all: whichever
 * correctly-routed line it was put on was held high forever and the CPU never
 * left the handler.
 */
static void pl080_refresh_masks(PL080State *s)
{
    int c;

    s->tc_mask = 0;
    s->err_mask = 0;
    for (c = 0; c < s->nchannels; c++) {
        if (s->chan[c].conf & PL080_CCONF_ITC) {
            s->tc_mask |= 1 << c;
        }
        if (s->chan[c].conf & PL080_CCONF_IE) {
            s->err_mask |= 1 << c;
        }
    }
}

static void pl080_update(PL080State *s)
{
    bool tclevel;
    bool errlevel;

    pl080_refresh_masks(s);
    tclevel = (s->tc_int & s->tc_mask);
    errlevel = (s->err_int & s->err_mask);

    /*
     * IT_DMAC_TRACE also shows every transition of the combined interrupt line,
     * with the pending mask. Both iPod touch DMACs are wired to the same VIC
     * number and s5l8900_get_irq() hands out the SAME qemu_irq for it, so
     * whichever controller drives it last wins -- and the register trace alone
     * cannot show that, because the loser's transition leaves no register
     * access behind.
     */
    if (it_dmac_trace_on() && s->last_level != (int)(errlevel || tclevel)) {
        s->last_level = errlevel || tclevel;
        fprintf(stderr, "[dmac%d] IRQ %s  tc_int=%02x tc_mask=%02x\n",
                s->trace_id, s->last_level ? "HIGH" : "low ",
                s->tc_int, s->tc_mask);
    }
    qemu_set_irq(s->interr, errlevel);
    qemu_set_irq(s->inttc, tclevel);
    qemu_set_irq(s->irq, errlevel || tclevel);
}

static void pl080_run(PL080State *s)
{
    int c;
    int flow;
    pl080_channel *ch;
    int swidth;
    int dwidth;
    int xsize;
    int n;
    int src_id;
    int dest_id;
    int size;
    uint8_t buff[4];
    uint32_t req;

    /*
     * Pending (tc_int) bits stay set until the driver acks them via
     * IntTCClear -- that is the real latch, and it is in tc_int. What is NOT
     * latched is the masking: see pl080_refresh_masks(). A driver that
     * programs a channel with ITC clear, goes idle, and only then sets ITC
     * still sees the completed transfer's interrupt, because the Config write
     * that sets ITC runs pl080_update().
     */
    if ((s->conf & PL080_CONF_E) == 0)
        return;

    /* If we are already in the middle of a DMA operation then indicate that
       there may be new DMA requests and return immediately.  */
    if (s->running) {
        s->running++;
        return;
    }
    s->running = 1;
    while (s->running) {
        for (c = 0; c < s->nchannels; c++) {
            ch = &s->chan[c];
again:
            /* Test if thiws channel has any pending DMA requests.  */
            if ((ch->conf & (PL080_CCONF_H | PL080_CCONF_E))
                    != PL080_CCONF_E)
                continue;
            flow = (ch->conf >> 11) & 7;
            if (flow >= 4) {
                hw_error(
                    "pl080_run: Peripheral flow control not implemented\n");
            }
            src_id = (ch->conf >> 1) & 0x1f;
            dest_id = (ch->conf >> 6) & 0x1f;
            size = ch->ctrl & 0xfff;
            req = s->req_single | s->req_burst;
            switch (flow) {
            case 0:
                break;
            /*
             * Flow control. Historically this model ignored the peripheral
             * request lines entirely for cases 1 and 2 -- nothing in this tree
             * drives them, so honouring them would have stalled every channel
             * forever. The cost was that a memory->peripheral descriptor was
             * drained at infinite speed inside the Config write: the iPod
             * touch's audio path programmed an 18-period, 72 KB ring and this
             * model copied all of it to the I2S TX FIFO in zero guest time,
             * so the guest's audio stack -- which fills that ring ahead of the
             * play position, in real time, off the terminal-count interrupts --
             * had written nothing yet and every sample delivered was silence.
             *
             * A peripheral that really drives its line opts in via
             * pl080_attach_paced_peripheral(); only then is it gated. Everyone
             * else (NAND, UART, SPI) behaves exactly as before.
             */
            case 1:
                if ((s->paced_req & (1u << dest_id))
                        && (req & (1u << dest_id)) == 0) {
                    size = 0;
                }
                break;
            case 2:
                if ((s->paced_req & (1u << src_id))
                        && (req & (1u << src_id)) == 0) {
                    size = 0;
                }
                break;
            case 3:
                if ((req & (1u << src_id)) == 0
                        || (req & (1u << dest_id)) == 0)
                    size = 0;
                break;
            }
            if (!size) {
                continue;
            }

            /* Transfer one element.  */
            /* ??? Should transfer multiple elements for a burst request.  */
            /* ??? Unclear what the proper behavior is when source and
               destination widths are different.  */
            swidth = 1 << ((ch->ctrl >> 18) & 7);
            dwidth = 1 << ((ch->ctrl >> 21) & 7);
            for (n = 0; n < dwidth; n+= swidth) {
                address_space_read(&s->downstream_as, ch->src,
                                   MEMTXATTRS_UNSPECIFIED, buff + n, swidth);
                if (ch->ctrl & PL080_CCTRL_SI)
                    ch->src += swidth;
            }
            xsize = (dwidth < swidth) ? swidth : dwidth;
            /* ??? This may pad the value incorrectly for dwidth < 32.  */
            for (n = 0; n < xsize; n += dwidth) {
                address_space_write(&s->downstream_as, ch->dest + n,
                                    MEMTXATTRS_UNSPECIFIED, buff + n, dwidth);
                if (ch->ctrl & PL080_CCTRL_DI)
                    ch->dest += swidth;
            }

            //printf("Transfer size: %d, destination: 0x%08x\n", size, ch->dest);
            size--;
            ch->ctrl = (ch->ctrl & 0xfffff000) | size;
            if (size == 0) {
                /* Transfer complete.  */
                if (ch->lli) {
                    ch->src = address_space_ldl_le(&s->downstream_as,
                                                   ch->lli,
                                                   MEMTXATTRS_UNSPECIFIED,
                                                   NULL);
                    ch->dest = address_space_ldl_le(&s->downstream_as,
                                                    ch->lli + 4,
                                                    MEMTXATTRS_UNSPECIFIED,
                                                    NULL);
                    ch->ctrl = address_space_ldl_le(&s->downstream_as,
                                                    ch->lli + 12,
                                                    MEMTXATTRS_UNSPECIFIED,
                                                    NULL);
                    ch->lli = address_space_ldl_le(&s->downstream_as,
                                                   ch->lli + 8,
                                                   MEMTXATTRS_UNSPECIFIED,
                                                   NULL);
                } else {
                    ch->conf &= ~PL080_CCONF_E;
                }
                if (ch->ctrl & PL080_CCTRL_I) {
                    //printf("Setting interrupt status of channel %d\n", c);
                    s->tc_int |= 1 << c;
                }
            }
            goto again;
        }
        if (--s->running)
            s->running = 1;
    }
    pl080_update(s);
}

void pl080_attach_paced_peripheral(PL080State *s, int id)
{
    if (id >= 0 && id < 32) {
        s->paced_req |= 1u << id;
    }
}

void pl080_set_dma_request(PL080State *s, int id, bool level)
{
    uint32_t bit;

    if (id < 0 || id >= 32) {
        return;
    }
    bit = 1u << id;
    if (level) {
        if (s->req_single & bit) {
            return;
        }
        s->req_single |= bit;
        pl080_run(s);
    } else {
        s->req_single &= ~bit;
    }
}

static uint64_t pl080_do_read(void *opaque, hwaddr offset,
                              unsigned size)
{
    PL080State *s = (PL080State *)opaque;
    uint32_t i;
    uint32_t mask;

    /* IntStatus/IntTCStatus/IntErrorStatus are masked by the channels' own
     * Configuration words, so the masks have to be current here too. */
    pl080_refresh_masks(s);

    if (offset >= 0xfe0 && offset < 0x1000) {
        if (s->nchannels == 8) {
            return pl080_id[(offset - 0xfe0) >> 2];
        } else {
            return pl081_id[(offset - 0xfe0) >> 2];
        }
    }
    if (offset >= 0x100 && offset < 0x200) {
        i = (offset & 0xe0) >> 5;
        if (i >= s->nchannels)
            goto bad_offset;
        switch ((offset >> 2) & 7) {
        case 0: /* SrcAddr */
            return s->chan[i].src;
        case 1: /* DestAddr */
            return s->chan[i].dest;
        case 2: /* LLI */
            return s->chan[i].lli;
        case 3: /* Control */
            return s->chan[i].ctrl;
        case 4: /* Configuration */
            return s->chan[i].conf;
        default:
            goto bad_offset;
        }
    }
    switch (offset >> 2) {
    case 0: /* IntStatus */
        return (s->tc_int & s->tc_mask) | (s->err_int & s->err_mask);
    case 1: /* IntTCStatus */
        return (s->tc_int & s->tc_mask);
    case 3: /* IntErrorStatus */
        return (s->err_int & s->err_mask);
    case 5: /* RawIntTCStatus */
        return s->tc_int;
    case 6: /* RawIntErrorStatus */
        return s->err_int;
    case 7: /* EnbldChns */
        mask = 0;
        for (i = 0; i < s->nchannels; i++) {
            if (s->chan[i].conf & PL080_CCONF_E)
                mask |= 1 << i;
        }
        return mask;
    case 8: /* SoftBReq */
    case 9: /* SoftSReq */
    case 10: /* SoftLBReq */
    case 11: /* SoftLSReq */
        /* ??? Implement these. */
        return 0;
    case 12: /* Configuration */
        return s->conf;
    case 13: /* Sync */
        return s->sync;
    default:
    bad_offset:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl080_read: Bad offset %x\n", (int)offset);
        return 0;
    }
}

static uint64_t pl080_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t val = pl080_do_read(opaque, offset, size);
    pl080_trace((PL080State *)opaque, offset, (uint32_t)val, false);
    return val;
}

static void pl080_write(void *opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
    PL080State *s = (PL080State *)opaque;
    int i;

    pl080_trace(s, offset, (uint32_t)value, true);

    //fprintf(stderr, "%s: writing 0x%08x to 0x%08x\n", __func__, value, offset);

    if (offset >= 0x100 && offset < 0x200) {
        i = (offset & 0xe0) >> 5;
        if (i >= s->nchannels)
            goto bad_offset;
        switch ((offset >> 2) & 7) {
        case 0: /* SrcAddr */
            //printf("%s: setting source address of channel %d to 0x%08x\n", __func__, i, value);
            s->chan[i].src = value;
            break;
        case 1: /* DestAddr */
            //printf("%s: setting destination address of channel %d to 0x%08x\n", __func__, i, value);
            s->chan[i].dest = value;
            break;
        case 2: /* LLI */
            s->chan[i].lli = value;
            break;
        case 3: /* Control */
            //printf("%s: setting control of channel %d to 0x%08x (transfer size: %d)\n", __func__, i, value, value & 0xfff);
            s->chan[i].ctrl = value;
            break;
        case 4: /* Configuration */
            //printf("%s: setting configuration of channel %d to 0x%08x\n", __func__, i, value);
            s->chan[i].conf = value;
            /* IT_DMA_TRACE=1: one line per channel start, which is how you see
             * whether anything is ever pointed at a peripheral FIFO. */
            static int trace = -1;
            if (trace < 0) {
                trace = getenv("IT_DMA_TRACE") != NULL;
            }
            if (trace) {
                fprintf(stderr, "[dma] ch%d src=%08x dst=%08x ctrl=%08x "
                        "conf=%08x\n", i, s->chan[i].src, s->chan[i].dest,
                        s->chan[i].ctrl, (uint32_t)value);
            }
            pl080_run(s);
            break;
        }
        /*
         * A Configuration write changes the per-channel interrupt masks, so the
         * line has to be re-evaluated even when pl080_run() returned early
         * (controller globally disabled, or re-entered). This branch used to
         * return without touching the line at all.
         */
        pl080_update(s);
        return;
    }
    switch (offset >> 2) {
    case 2: /* IntTCClear */
        s->tc_int &= ~value;
        break;
    case 4: /* IntErrorClear */
        s->err_int &= ~value;
        break;
    case 8: /* SoftBReq */
    case 9: /* SoftSReq */
    case 10: /* SoftLBReq */
    case 11: /* SoftLSReq */
        /* ??? Implement these.  */
        qemu_log_mask(LOG_UNIMP, "pl080_write: Soft DMA not implemented\n");
        break;
    case 12: /* Configuration */
        s->conf = value;
        if (s->conf & (PL080_CONF_M1 | PL080_CONF_M2)) {
            qemu_log_mask(LOG_UNIMP,
                          "pl080_write: Big-endian DMA not implemented\n");
        }
        pl080_run(s);
        break;
    case 13: /* Sync */
        s->sync = value;
        break;
    default:
    bad_offset:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pl080_write: Bad offset %x\n", (int)offset);
    }
    pl080_update(s);
}

static const MemoryRegionOps pl080_ops = {
    .read = pl080_read,
    .write = pl080_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void pl080_reset(DeviceState *dev)
{
    PL080State *s = PL080(dev);
    int i;

    s->tc_int = 0;
    s->tc_mask = 0;
    s->err_int = 0;
    s->err_mask = 0;
    s->conf = 0;
    s->sync = 0;
    s->req_single = 0;
    s->req_burst = 0;
    s->running = 0;

    for (i = 0; i < s->nchannels; i++) {
        s->chan[i].src = 0;
        s->chan[i].dest = 0;
        s->chan[i].lli = 0;
        s->chan[i].ctrl = 0;
        s->chan[i].conf = 0;
    }

    /*
     * Clearing tc_int/err_int is not enough: the outgoing IRQ lines keep
     * whatever level they were last driven to, so a reset taken with a
     * terminal-count interrupt pending left the line asserted with no state
     * to justify it. The interrupt controller then sees a raw-high line that
     * nothing can clear, and firmware which enables that IRQ after reset
     * spins in its handler forever.
     *
     * Found on the iPod touch 2G, where system_reset re-entered iBoot and
     * wedged at 100% CPU in IRQ mode with VIC0 line 17 (DMAC1) raw-asserted.
     */
    pl080_update(s);
}

static void pl080_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PL080State *s = PL080(obj);

    memory_region_init_io(&s->iomem1, OBJECT(s), &pl080_ops, s, "pl080", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem1);
    memory_region_init_io(&s->iomem2, OBJECT(s), &pl080_ops, s, "pl080", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem2);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->interr);
    sysbus_init_irq(sbd, &s->inttc);
    s->nchannels = 8;

    /* Instantiation order only; the iPod touch machine makes DMAC0 first. */
    static int next_id;
    s->trace_id = next_id++;
}

static void pl080_realize(DeviceState *dev, Error **errp)
{
    PL080State *s = PL080(dev);

    if (!s->downstream) {
        error_setg(errp, "PL080 'downstream' link not set");
        return;
    }

    address_space_init(&s->downstream_as, s->downstream, "pl080-downstream");
}

static void pl081_init(Object *obj)
{
    PL080State *s = PL080(obj);

    s->nchannels = 2;
}

static const Property pl080_properties[] = {
    DEFINE_PROP_LINK("downstream", PL080State, downstream,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void pl080_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->vmsd = &vmstate_pl080;
    dc->realize = pl080_realize;
    device_class_set_props(dc, pl080_properties);
    device_class_set_legacy_reset(dc, pl080_reset);
}

static const TypeInfo pl080_info = {
    .name          = TYPE_PL080,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PL080State),
    .instance_init = pl080_init,
    .class_init    = pl080_class_init,
};

static const TypeInfo pl081_info = {
    .name          = TYPE_PL081,
    .parent        = TYPE_PL080,
    .instance_init = pl081_init,
};

/* The PL080 and PL081 are the same except for the number of channels
   they implement (8 and 2 respectively).  */
static void pl080_register_types(void)
{
    type_register_static(&pl080_info);
    type_register_static(&pl081_info);
}

type_init(pl080_register_types)
