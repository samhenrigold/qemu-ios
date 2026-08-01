#include "hw/arm/ipod_touch_wdt.h"
#include "sysemu/runstate.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

/*
 * S5L8720 watchdog timer at 0x3C800000.
 *
 * The only behaviour the guest actually needs modelled is reset: after an
 * unclean shutdown the boot runs fsck, repairs the volume, and then asks to
 * restart ("MACH Reboot"). AppleARMWatchDogTimer's PEHaltRestart handler does
 * that by setting bit 0x100000 in the control register. With no device here
 * that write was a silent no-op, so the guest simply halted and needed a manual
 * relaunch. Turning the write into a QEMU reset request lets the machine
 * re-run its init and boot again on its own.
 */

static uint64_t ipod_touch_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchWDTState *s = (IPodTouchWDTState *)opaque;
    switch (addr) {
        case WDT_CTRL:
            return s->ctrl;
        case WDT_CNT:
            return s->cnt;
        default:
            return 0;
    }
}

static void ipod_touch_wdt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchWDTState *s = (IPodTouchWDTState *)opaque;

    switch (addr) {
        case WDT_CTRL:
            s->ctrl = (uint32_t)val;
            if (val & WDT_RESET_BIT) {
                if (current_cpu) {
                    ARMCPU *ac = ARM_CPU(current_cpu);
                    fprintf(stderr, "%s: reset bit set (val=0x%08x) from PC=0x%08x LR=0x%08x\n",
                            __func__, (uint32_t)val, ac->env.regs[15], ac->env.regs[14]);
                } else {
                    fprintf(stderr, "%s: reset bit set (val=0x%08x)\n", __func__, (uint32_t)val);
                }
                if (getenv("IT_WDT_NORESET")) {
                    /* Diagnostic: don't actually reset, so the machine wedges at
                     * the reset site and QMP can inspect it. */
                    break;
                }
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }
            break;
        case WDT_CNT:
            s->cnt = (uint32_t)val;
            break;
        default:
            break;
    }
}

static const MemoryRegionOps ipod_touch_wdt_ops = {
    .read = ipod_touch_wdt_read,
    .write = ipod_touch_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_wdt_init(Object *obj)
{
    IPodTouchWDTState *s = IPOD_TOUCH_WDT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_wdt_ops, s, TYPE_IPOD_TOUCH_WDT, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void ipod_touch_wdt_class_init(ObjectClass *klass, void *data)
{
}

static const TypeInfo ipod_touch_wdt_type_info = {
    .name = TYPE_IPOD_TOUCH_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchWDTState),
    .instance_init = ipod_touch_wdt_init,
    .class_init = ipod_touch_wdt_class_init,
};

static void ipod_touch_wdt_register_types(void)
{
    type_register_static(&ipod_touch_wdt_type_info);
}

type_init(ipod_touch_wdt_register_types)
