/*
 * ARM PrimeCell PL192 Vector Interrupt Controller
 *
 * Copyright (c) 2009 Samsung Electronics.
 * Contributed by Kirill Batuzov <batuzovk@ispras.ru>
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "hw/hw.h"
#include "qapi/error.h"
#include "hw/intc/pl192.h"
#include "hw/arm/ipod_touch_guard.h"
#include "migration/vmstate.h"

extern CPUState *getMainCpuEnv(void);


static void pl192_update(PL192State *);

static void pl192_raise(PL192State *s, int is_fiq)
{
    if (is_fiq) {
        if (s->fiq) {
            /* Raise parent FIQ */
            qemu_irq_raise(s->fiq);
        } else {
            if (s->daisy) {
                /* FIQ is directly propagated through daisy chain */
                pl192_raise(s->daisy, is_fiq);
            } else {
                fprintf(stderr, "pl192i cannot raise FIQ. This usually means that "
                         "initialization was done incorrectly.\n");
            }
        }
    } else {
        if (s->irq) {
            /* Raise parent IRQ */
            qemu_irq_raise(s->irq);
        } else {
            if (s->daisy) {
                /* Setup daisy input of the next chained contorller and force
                   it to update it's state */
                s->daisy->daisy_vectaddr = s->address;
                s->daisy->daisy_callback = s;
                s->daisy->daisy_input = 1;
                pl192_update(s->daisy);
            } else {
                // TODO Needs urgent fixing!
                hw_error("pl192: cannot raise IRQ. This usually means that initialization was done incorrectly.\n");
            }
        }
    }
}

static void pl192_lower(PL192State *s, int is_fiq)
{
    /* Lower parrent interrupt if there is one */
    if (is_fiq && s->fiq) {
        qemu_irq_lower(s->fiq);
    }
    if (!is_fiq && s->irq) {
        qemu_irq_lower(s->irq);
    }
    /* Propagate to the previous controller in chain if needed */
    if (s->daisy) {
        if (!is_fiq) {
            s->daisy->daisy_input = 0;
            pl192_update(s->daisy);
        } else {
            pl192_lower(s->daisy, is_fiq);
        }
    }
}

/* Find interrupt of the highest priority */
static uint32_t pl192_priority_sorter(PL192State *s)
{
    int i;
    uint32_t prio_irq[PL192_PRIO_LEVELS];

    for (i = 0; i < PL192_PRIO_LEVELS; i++) {
        prio_irq[i] = PL192_NO_IRQ;
    }
    if (s->daisy_input) {
        prio_irq[s->daisy_priority] = PL192_DAISY_IRQ;
    }
    for (i = PL192_INT_SOURCES - 1; i >= 0; i--) {
        if (s->irq_status & (1u << i)) {
            prio_irq[s->vect_priority[i]] = i;
        }
    }
    for (i = 0; i < PL192_PRIO_LEVELS; i++) {
        if ((s->sw_priority_mask & (1u << i)) &&
            prio_irq[i] <= PL192_DAISY_IRQ) {
            return prio_irq[i];
        }
    }
    return PL192_NO_IRQ;
}

static void pl192_update(PL192State *s)
{
    /* TODO: does SOFTINT affects IRQ_STATUS??? */
    s->irq_status = (s->rawintr | s->softint) & s->intenable & ~s->intselect;
    s->fiq_status = (s->rawintr | s->softint) & s->intenable & s->intselect;
    if (s->fiq_status) {
        pl192_raise(s, 1);
    } else {
        pl192_lower(s, 1);
    }
    if (s->irq_status || s->daisy_input) {
        s->current_highest = pl192_priority_sorter(s);
        if (s->current_highest < PL192_INT_SOURCES) {
            s->address = s->vect_addr[s->current_highest];
        } else {
            s->address = s->daisy_vectaddr;
        }
        if (s->current_highest != s->current) {
            if (s->current_highest < PL192_INT_SOURCES) {
                if (s->vect_priority[s->current_highest] >= s->priority) {
                    return ;
                }
            }
            if (s->current_highest == PL192_DAISY_IRQ) {
                if (s->daisy_priority >= s->priority) {
                    return ;
                }
            }
            if (s->current_highest <= PL192_DAISY_IRQ) {
                pl192_raise(s, 0);
            } else {
                pl192_lower(s, 0);
            }
        }
    } else {
        s->current_highest = PL192_NO_IRQ;
        pl192_lower(s, 0);
    }
}

/* Set priority level when an interrupt have been acknoledged by CPU.
   Also save interrupt id and priority to stack so it can be restored
   lately. */
static inline void pl192_mask_priority(PL192State *s)
{
    /*
     * The guard here checked stack_i against PL192_INT_SOURCES (32), but
     * priority_stack[] and irq_stack[] are both [PL192_PRIO_LEVELS + 1] = 17.
     * Every read of VECTADDR acknowledges and pushes; a write pops. Eighteen
     * acknowledgements without a matching write therefore wrote past both
     * arrays into the adjacent fields of PL192State. 17 is the architectural
     * maximum nesting depth, so the arrays are the right size and the bound was
     * simply wrong -- saturate the depth instead of enlarging them, and do not
     * abort: an over-deep nest is a guest/driver error, not an emulator one.
     */
    s->stack_i++;
    s->stack_i = IT_IDX("pl192", s->stack_i, PL192_PRIO_LEVELS + 1);
    if (s->current == PL192_DAISY_IRQ) {
        s->priority = s->daisy_priority;
    } else {
        s->priority = s->vect_priority[s->current];
    }
    s->priority_stack[s->stack_i] = s->priority;
    s->irq_stack[s->stack_i] = s->current;
}

/* Set priority level when interrupt have been successfully processed by CPU.
   Also restore previous interrupt id and priority level. */
static inline void pl192_unmask_priority(PL192State *s)
{
    if (s->stack_i < 1) {
        return; // simply ignore this event
        //hw_error("pl192: internal error (mask stack insufficient)\n");
    }
    s->stack_i--;
    s->priority = s->priority_stack[s->stack_i];
    s->current = s->irq_stack[s->stack_i];
}

/* IRQ was acknoledged by CPU. Update controller state accordingly */
static uint32_t pl192_irq_ack(PL192State *s)
{
    int is_daisy = (s->current_highest == PL192_DAISY_IRQ);
    uint32_t res = s->address;

    if (s->current_highest == PL192_NO_IRQ) {
        return res;
    }
    s->current = s->current_highest;
    pl192_mask_priority(s);
    if (is_daisy) {
        /* The parent acknowledges the selected child vector too. Leaving the
         * child at NO_IRQ indexes vect_priority[33] and installs priority 33
         * instead of the active interrupt's priority. */
        s->daisy_callback->current = s->daisy_callback->current_highest;
        pl192_mask_priority(s->daisy_callback);
    }
    pl192_update(s);
    return res;
}

/* IRQ was processed by CPU. Update controller state accrodingly */
static void pl192_irq_fin(PL192State *s)
{
    int is_daisy = (s->current == PL192_DAISY_IRQ);

    pl192_unmask_priority(s);
    if (is_daisy) {
        pl192_unmask_priority(s->daisy_callback);
        /* Re-select the child vector before the parent uses its cached daisy
         * address. Another pending source may have been priority-masked while
         * the previous interrupt was active. */
        pl192_update(s->daisy_callback);
    }
    pl192_update(s);

	/* hmm is this right?*/
	/*
    if (s->current == PL192_NO_IRQ && (s->current_highest >= PL192_INT_SOURCES)) {
        pl192_lower(s, 0);
    }
	*/
}

static uint64_t pl192_read(void *opaque, hwaddr offset, unsigned size)
{
    PL192State *s = (PL192State *) opaque;

    if (offset & 3) {
        fprintf(stderr, "pl192: bad read offset (1) " HWADDR_FMT_plx "\n", offset);
        return 0;
    }

    if (offset >= 0xfe0 && offset < 0x1000) {
        unsigned char pl192_id[] = { 0x92, 0x11, 0x04, 0x00, 0x0D, 0xF0, 0x05, 0xB1 };
        return pl192_id[(offset - 0xfe0) >> 2];
    }
    if (offset >= 0x100 && offset < 0x180) {
        return s->vect_addr[(offset - 0x100) >> 2];
    }
    if (offset >= 0x200 && offset < 0x280) {
        return s->vect_priority[(offset - 0x200) >> 2];
    }

    switch (offset) {
        case PL192_IRQSTATUS:
			//fprintf(stderr, "%s: irqstatus 0x%08x\n", __FUNCTION__, s->irq_status);
            return s->irq_status;
        case PL192_FIQSTATUS:
            return s->fiq_status;
        case PL192_RAWINTR:
            return s->rawintr;
        case PL192_INTSELECT:
            return s->intselect;
        case PL192_INTENABLE:
            return s->intenable;
        case PL192_SOFTINT:
            return s->softint;
        case PL192_PROTECTION:
            return s->protection;
        case PL192_SWPRIORITYMASK:
            return s->sw_priority_mask;
        case PL192_PRIORITYDAISY:
            return s->daisy_priority;
        case PL192_INTENCLEAR:
			return 0;
        case PL192_SOFTINTCLEAR:
            fprintf(stderr, "pl192: attempt to read write-only register (offset = "
                     HWADDR_FMT_plx ")\n", offset);
        case PL192_VECTADDR:
            return pl192_irq_ack(s);
        /* Workaround for kernel code using PL190 */
        case PL190_ITCR:
        case PL190_VECTADDR:
        case PL190_DEFVECTADDR:
            return 0;
        default:
            fprintf(stderr, "pl192: bad read offset (2) " HWADDR_FMT_plx "\n", offset);
            return 0;
    }
}

static void pl192_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    PL192State *s = (PL192State *) opaque;

    if ((offset == PL192_INTENABLE || offset == PL192_INTENCLEAR) &&
        getenv("VIC_TRACE")) {
        uint64_t pc = current_cpu ?
            CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;
        fprintf(stderr, "VIC_CTL %s reg=%x val=%08x pc=%08" PRIx64
                " raw=%x current=%u highest=%u priority=%u depth=%d time=%" PRId64 "\n",
                memory_region_name(&s->iomem), (unsigned)offset, (unsigned)value,
                pc, s->rawintr, s->current, s->current_highest, s->priority,
                s->stack_i, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    if (offset & 3) {
        hw_error("pl192: bad write offset (1) " HWADDR_FMT_plx "\n", offset);
    }

    if (offset >= 0xfe0 && offset < 0x1000) {
        hw_error("pl192: attempt to write to a read-only register (offset = "
                 HWADDR_FMT_plx ")\n", offset);
    }
    if (offset >= 0x100 && offset < 0x180) {
        s->vect_addr[(offset - 0x100) >> 2] = value;
        pl192_update(s);
        return;
    }
    if (offset >= 0x200 && offset < 0x280) {
        s->vect_priority[(offset - 0x200) >> 2] = value & 0xf;
        pl192_update(s);
        return;
    }

    switch (offset) {
        case PL192_IRQSTATUS:
            /* This is a readonly register, but linux tries to write to it
               anyway.  Ignore the write.  */
            return;
        case PL192_FIQSTATUS:
        case PL192_RAWINTR:
            hw_error("pl192: attempt to write to a read-only register (offset = "
                     HWADDR_FMT_plx ")\n", offset);
            break;
        case PL192_INTSELECT:
            s->intselect = value;
            break;
        case PL192_INTENABLE:
            s->intenable |= value;
            if (getenv("VIC_TRACE")) {
                fprintf(stderr, "[VIC] %s enable  += 0x%08x  now 0x%08x\n",
                        memory_region_name(&s->iomem), (uint32_t)value,
                        s->intenable);
            }
            break;
        case PL192_INTENCLEAR:
            s->intenable &= ~value;
            if (getenv("VIC_TRACE")) {
                fprintf(stderr, "[VIC] %s disable -= 0x%08x  now 0x%08x\n",
                        memory_region_name(&s->iomem), (uint32_t)value,
                        s->intenable);
            }
            break;
        case PL192_SOFTINT:
            s->softint |= value;
            break;
        case PL192_SOFTINTCLEAR:
            s->softint &= ~value;
            break;
        case PL192_PROTECTION:
            /* TODO: implement protection */
            s->protection = value & 1;
            break;
        case PL192_SWPRIORITYMASK:
            s->sw_priority_mask = value & 0xffff;
            break;
        case PL192_PRIORITYDAISY:
            s->daisy_priority = value & 0xf;
            break;
        case PL192_VECTADDR:
            pl192_irq_fin(s);
            return;
        case PL190_ITCR:
        case PL190_VECTADDR:
        case PL190_DEFVECTADDR:
            /* NB: This thing is not present here, but linux wants to write it */
            /* Ignore written value */
            return;
        default:
            fprintf(stderr, "pl192: bad write offset (2) " HWADDR_FMT_plx "\n", offset);
            return;
    }

    pl192_update(s);
}

static void pl192_irq_handler(void *opaque, int irq, int level)
{
    PL192State *s = (PL192State *) opaque;

    if (level) {
        s->rawintr |= 1u << irq;
    } else {
        s->rawintr &= ~(1u << irq);
    }
    pl192_update(opaque);
}

static void pl192_reset(DeviceState *d)
{
    PL192State *s = PL192(d);
    int i;

    for (i = 0; i < PL192_INT_SOURCES; i++) {
        s->vect_priority[i] = 0xf;
    }
    s->sw_priority_mask = 0xffff;
    s->daisy_priority = 0xf;
    s->current = PL192_NO_IRQ;
    s->current_highest = PL192_NO_IRQ;
    s->stack_i = 0;
    s->priority_stack[0] = 0x10;
    s->irq_stack[0] = PL192_NO_IRQ;
    s->priority = 0x10;

    /*
     * Programmable registers, all documented reset-to-zero. Without these a
     * second boot started with the previous kernel's enable mask and vector
     * table still loaded.
     *
     * rawintr is deliberately NOT cleared: it mirrors the level of the
     * incoming device lines, and device reset order is unspecified. A device
     * whose own reset does not re-drive its line would have a genuinely
     * asserted interrupt erased here. irq_status/fiq_status are derived, so
     * pl192_update recomputes them from rawintr below.
     */
    s->intenable = 0;
    s->intselect = 0;
    s->softint = 0;
    s->protection = 0;
    s->address = 0;
    for (i = 0; i < PL192_INT_SOURCES; i++) {
        s->vect_addr[i] = 0;
    }

    /*
     * Step A of the pl080 lesson applied to the VIC: reset recomputed none of
     * the outputs, so whatever level irq/fiq were last driven to survived into
     * the next boot. Clearing state is not the same as re-deriving the line.
     * Register clears are deliberately NOT part of this change -- one variable.
     */
    pl192_update(s);
}

/*
 * The daisy pointers, the memory region and the parent irq/fiq lines are all
 * wired by the machine before any snapshot is loaded, so only the registers
 * travel. post_load re-runs pl192_update: restoring irq_status is not the same
 * as re-driving the CPU's IRQ line, which on the destination is still at
 * whatever reset left it. That is the same trap that made a reset leave a
 * phantom interrupt behind, seen from the other side.
 */
static int pl192_post_load(void *opaque, int version_id)
{
    pl192_update(PL192(opaque));
    return 0;
}

static const VMStateDescription vmstate_pl192 = {
    .name = "pl192",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pl192_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(irq_status, PL192State),
        VMSTATE_UINT32(fiq_status, PL192State),
        VMSTATE_UINT32(rawintr, PL192State),
        VMSTATE_UINT32(intselect, PL192State),
        VMSTATE_UINT32(intenable, PL192State),
        VMSTATE_UINT32(softint, PL192State),
        VMSTATE_UINT32(protection, PL192State),
        VMSTATE_UINT32(sw_priority_mask, PL192State),
        VMSTATE_UINT32_ARRAY(vect_addr, PL192State, PL192_INT_SOURCES),
        VMSTATE_UINT32_ARRAY(vect_priority, PL192State, PL192_INT_SOURCES),
        VMSTATE_UINT32(address, PL192State),
        VMSTATE_UINT32(current, PL192State),
        VMSTATE_UINT32(current_highest, PL192State),
        VMSTATE_INT32(stack_i, PL192State),
        VMSTATE_UINT32_ARRAY(priority_stack, PL192State, PL192_PRIO_LEVELS + 1),
        VMSTATE_UINT8_ARRAY(irq_stack, PL192State, PL192_PRIO_LEVELS + 1),
        VMSTATE_UINT32(priority, PL192State),
        VMSTATE_UINT32(daisy_vectaddr, PL192State),
        VMSTATE_UINT32(daisy_priority, PL192State),
        VMSTATE_UINT8(daisy_input, PL192State),
        VMSTATE_END_OF_LIST()
    }
};

static const MemoryRegionOps pl192_ops = {
    .read = pl192_read,
    .write = pl192_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

DeviceState *pl192_manual_init(char *mem_name, ...)
{
    DeviceState *dev = qdev_new(TYPE_PL192);
    PL192State *s = PL192(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    va_list va;
    qemu_irq irq;

    memory_region_init_io(&s->iomem, OBJECT(s), &pl192_ops, s, mem_name, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_in(dev, pl192_irq_handler, PL192_INT_SOURCES);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
    sysbus_realize_and_unref(sbd, &error_fatal);

    va_start(va, mem_name);
    int n = 0;
    while (1) {
        irq = va_arg(va, qemu_irq);
        if (!irq) {
            break;
        }
        sysbus_connect_irq(sbd, n, irq);
        n++;
    }

    return dev;
}

static void pl192_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    PL192State *s = PL192(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    //memory_region_init_io(&s->iomem, obj, &pl192_ops, s, "pl192", 0x1000);
    //sysbus_init_mmio(sbd, &s->iomem);
    //qdev_init_gpio_in(dev, pl192_irq_handler, PL192_INT_SOURCES);
    //sysbus_init_irq(sbd, s->irq);
    //sysbus_init_irq(sbd, s->fiq);
}

static void pl192_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, pl192_reset);
    dc->vmsd = &vmstate_pl192;
}

static const TypeInfo pl192_info = {
    .name          = TYPE_PL192,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PL192State),
    .instance_init = pl192_init,
    .class_init    = pl192_class_init,
};

static void pl192_register_types(void)
{
    type_register_static(&pl192_info);
}

type_init(pl192_register_types)