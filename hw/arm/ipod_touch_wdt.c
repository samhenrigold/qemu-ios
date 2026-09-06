#include "hw/arm/ipod_touch_wdt.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

/* IT_WDT_TRACE: one line per watchdog kick. Cached; see the call site. */
static bool wdt_trace(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_WDT_TRACE") != NULL;
    }
    return on;
}

/*
 * 7E18 AppleARMWatchDogTimer writes 0x001f4a00 to arm/kick the watchdog
 * (c056d364/c056d37c), zero to disable it, and exactly 0x00100000 to
 * request a reset (c056d350). Testing only bit 20 made every normal kick
 * reboot the machine. The nonzero timeout must not mean immediate reset.
 * ponytail: timed expiry remains unmodeled; add a virtual-clock timer once
 * the timeout field and clock are established from hardware evidence.
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
            if (val == WDT_RESET_COMMAND) {
                if (wdt_trace()) {
                    if (current_cpu) {
                        ARMCPU *ac = ARM_CPU(current_cpu);
                        fprintf(stderr, "%s: reset command (val=0x%08x) from "
                                "PC=0x%08x LR=0x%08x\n", __func__,
                                (uint32_t)val, ac->env.regs[15], ac->env.regs[14]);
                    } else {
                        fprintf(stderr, "%s: reset command (val=0x%08x)\n",
                                __func__, (uint32_t)val);
                    }
                }
                if (s->noreset) {
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

/* The watchdog must come out of a reset DISARMED. Carrying ctrl across meant
 * the device it had just reset was still holding a loaded watchdog. */
static void ipod_touch_wdt_reset(DeviceState *dev)
{
    IPodTouchWDTState *s = IPOD_TOUCH_WDT(dev);

    s->ctrl = 0;
    s->cnt = 0;
}

static const VMStateDescription vmstate_ipod_touch_wdt = {
    .name = "ipod_touch_wdt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, IPodTouchWDTState),
        VMSTATE_UINT32(cnt, IPodTouchWDTState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_wdt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_wdt;
    device_class_set_legacy_reset(dc, ipod_touch_wdt_reset);
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
