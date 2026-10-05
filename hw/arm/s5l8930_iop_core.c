/*
 * S5L8930 IOP as a real second core (the "IOP core", LightTouchMac
 * docs/fidelity-ledger.md K48 #33). Runs the EmbeddedIOP firmware the kernel
 * uploads (ARM7M kext: FW_BASE/FW_SIZE, CTRL=1) unmodified, so the mailbox
 * protocol, the FMI/SDIO command ABIs and the NAND/DMA programming come from
 * Apple's code instead of hw/arm/s5l8930_iop.c's HLE.
 *
 * What the firmware told us (docs/ipad1/iop-core.md):
 * - ARM-mode ARMv5 code with an ARM946-style cp15: protection regions c6,cN
 *   {0,1}, cachable/write-buffer c2/c3, access permissions c5, WFI c7,c0,4,
 *   plus v6 ID registers and thread-ID/implementation-defined registers it
 *   reads once. QEMU's arm946 plus the extras below.
 * - Its address 0 is the firmware image (physical FW_BASE in DRAM); DRAM as a
 *   whole is at 0xc0000000 (the 'cnfg' memory map {0xc0000000, 0x40000000,
 *   0x40000000}); the peripherals are at the AP's addresses (PMGR timers
 *   0xbf102008.., AP VIC0 SOFTINT 0xbf200018 to signal the AP, its own VICs at
 *   0xbf300000, the IOP block 0x86300000, CDMA 0x87000000, AES 0x87800000,
 *   FMI 0x812/0x813xxxxx, SDHC 0x80000000, SHA 0x80100000).
 * - Interrupt numbering on its VICs mirrors the AP's (the AP->IOP doorbell is
 *   SOFTINT bit 3 on VIC0, the same number the AP's IRQ_IOP has), so every
 *   device line the board gives the AP is split to the IOP's VIC too.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/intc/pl192.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "exec/memory.h"
#include "hw/arm/s5l8930.h"
#include "system/reset.h"
#include "migration/vmstate.h"

#define TYPE_S5L8930_IOP_CORE "s5l8930.iop-core"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8930IOPCoreState, S5L8930_IOP_CORE)

#define IOP_DRAM_WINDOW     0xc0000000
#define IOP_PERIPH_BASE     0x80000000
#define IOP_PERIPH_SIZE     0x40000000
#define IOP_VIC_COUNT       4
#define IOP_VIC_STRIDE      0x10000

struct S5L8930IOPCoreState {
    DeviceState parent_obj;

    MemoryRegion *dram;         /* link: the board's DRAM */
    MemoryRegion *sysmem;       /* link: the AP's system memory (peripherals) */
    MemoryRegion mem;           /* the IOP's own address space */
    MemoryRegion dram_window;
    MemoryRegion periph;
    MemoryRegion fw;            /* firmware image at 0, made at run() */
    bool fw_mapped;
    ARMCPU *cpu;
    DeviceState *vic[IOP_VIC_COUNT];
    uint32_t fw_base, fw_size;
    DeviceState *iop;           /* the AP-side IOP block (ring trace) */
    MemoryRegion ap_vic_tap;    /* the IOP's view of AP VIC0: SOFTINT = IOP->AP doorbell */
    MemoryRegion vic0_tap;      /* the AP's view of IOP VIC0: SOFTINT = AP->IOP doorbell */
};

void s5l8930_iop_core_set_iop(DeviceState *dev, DeviceState *iop)
{
    S5L8930_IOP_CORE(dev)->iop = iop;
}

static uint64_t ap_vic_tap_read(void *opaque, hwaddr off, unsigned size)
{
    return address_space_ldl_le(&address_space_memory, S5L8930_VIC_BASE(0) + off,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void ap_vic_tap_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S5L8930IOPCoreState *s = opaque;

    if (off == 0x18 && s->iop) {        /* SOFTINT: the firmware has answered something */
        s5l8930_iop_trace_rings(s->iop);
    }
    address_space_stl_le(&address_space_memory, S5L8930_VIC_BASE(0) + off, val,
                         MEMTXATTRS_UNSPECIFIED, NULL);
}

/* AP -> IOP doorbell: the ring trace sees what the AP has just handed over. */
static uint64_t vic0_tap_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8930IOPCoreState *s = opaque;
    uint64_t v = 0;

    memory_region_dispatch_read(&PL192(s->vic[0])->iomem, off, &v, MO_32, MEMTXATTRS_UNSPECIFIED);
    return v;
}

static void vic0_tap_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S5L8930IOPCoreState *s = opaque;

    if (off == 0x18 && s->iop) {
        s5l8930_iop_trace_rings(s->iop);
    }
    memory_region_dispatch_write(&PL192(s->vic[0])->iomem, off, val, MO_32, MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps vic0_tap_ops = {
    .read = vic0_tap_read,
    .write = vic0_tap_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps ap_vic_tap_ops = {
    .read = ap_vic_tap_read,
    .write = ap_vic_tap_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* cp15 the firmware touches that QEMU's arm946 lacks: reads once at boot (IDs,
 * thread IDs, an implementation-defined c15 register), a ninth protection
 * region, CPACR, and the v6-style WFI encoding. */
static const ARMCPRegInfo iop_cp_reginfo[] = {
    { .name = "IOP_ID_PFR", .cp = 15, .opc1 = 0, .crn = 0, .crm = 1, .opc2 = CP_ANY,
      .access = PL1_R, .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
    { .name = "IOP_ID_ISAR", .cp = 15, .opc1 = 0, .crn = 0, .crm = 2, .opc2 = CP_ANY,
      .access = PL1_R, .type = ARM_CP_CONST | ARM_CP_OVERRIDE, .resetvalue = 0 },
    { .name = "IOP_CPACR", .cp = 15, .opc1 = 0, .crn = 1, .crm = 0, .opc2 = 2,
      .access = PL1_RW, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .name = "IOP_TPIDR", .cp = 15, .opc1 = 0, .crn = 13, .crm = 0, .opc2 = CP_ANY,
      .access = PL1_RW, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .name = "IOP_C15", .cp = 15, .opc1 = 0, .crn = 15, .crm = CP_ANY, .opc2 = CP_ANY,
      .access = PL1_RW, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .name = "IOP_PRBS8", .cp = 15, .opc1 = 0, .crn = 6, .crm = 8, .opc2 = CP_ANY,
      .access = PL1_RW, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
    { .name = "IOP_WFI", .cp = 15, .opc1 = 0, .crn = 7, .crm = 0, .opc2 = 4,
      .access = PL1_W, .type = ARM_CP_WFI | ARM_CP_OVERRIDE },
    { .name = "IOP_TLBIALL", .cp = 15, .opc1 = 0, .crn = 8, .crm = CP_ANY, .opc2 = CP_ANY,
      .access = PL1_W, .type = ARM_CP_NOP | ARM_CP_OVERRIDE },
};

qemu_irq s5l8930_iop_core_irq(DeviceState *dev, int irq)
{
    S5L8930IOPCoreState *s = S5L8930_IOP_CORE(dev);

    return qdev_get_gpio_in(s->vic[irq / 32], irq % 32);
}

void s5l8930_iop_core_run(DeviceState *dev, uint32_t fw_base, uint32_t fw_size)
{
    S5L8930IOPCoreState *s = S5L8930_IOP_CORE(dev);
    CPUState *cs = CPU(s->cpu);

    if (s->fw_mapped) {
        memory_region_del_subregion(&s->mem, &s->fw);
        object_unparent(OBJECT(&s->fw));
        s->fw_mapped = false;
    }
    if (fw_base < S5L8930_DRAM_BASE || fw_base + fw_size > S5L8930_DRAM_BASE + memory_region_size(s->dram)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: firmware 0x%08x+0x%x outside DRAM\n",
                      __func__, fw_base, fw_size);
        return;
    }
    memory_region_init_alias(&s->fw, OBJECT(s), "iop.fw", s->dram,
                             fw_base - S5L8930_DRAM_BASE, fw_size);
    memory_region_add_subregion_overlap(&s->mem, 0, &s->fw, 1);
    s->fw_mapped = true;
    s->fw_base = fw_base;
    s->fw_size = fw_size;
    qemu_log_mask(LOG_UNIMP, "iop-core: run firmware 0x%08x+0x%x\n", fw_base, fw_size);
    cpu_reset(cs);
    cs->halted = 0;
    qemu_cpu_kick(cs);
}

void s5l8930_iop_core_stop(DeviceState *dev)
{
    S5L8930IOPCoreState *s = S5L8930_IOP_CORE(dev);
    CPUState *cs = CPU(s->cpu);

    cpu_reset(cs);              /* start-powered-off: halted again */
    qemu_log_mask(LOG_UNIMP, "iop-core: stopped\n");
}

/*
 * A system reset (the PMU's restart) stops the core like the kext's CTRL = 0
 * and takes the old firmware image off address 0. The device has no bus, so
 * the machine's reset walk never gets here on its own.
 */
static void s5l8930_iop_core_reset(void *opaque)
{
    S5L8930IOPCoreState *s = opaque;

    cpu_reset(CPU(s->cpu));
    if (s->fw_mapped) {
        memory_region_del_subregion(&s->mem, &s->fw);
        object_unparent(OBJECT(&s->fw));
        s->fw_mapped = false;
    }
}

static void s5l8930_iop_core_realize(DeviceState *dev, Error **errp)
{
    S5L8930IOPCoreState *s = S5L8930_IOP_CORE(dev);
    Object *cpuobj;
    int i;

    if (!s->dram || !s->sysmem) {
        error_setg(errp, "iop-core needs its dram and sysmem links");
        return;
    }
    memory_region_init(&s->mem, OBJECT(s), "iop.mem", 1ULL << 32);
    memory_region_init_alias(&s->dram_window, OBJECT(s), "iop.dram-window", s->dram, 0,
                             memory_region_size(s->dram));
    memory_region_add_subregion(&s->mem, IOP_DRAM_WINDOW, &s->dram_window);
    memory_region_init_alias(&s->periph, OBJECT(s), "iop.periph", s->sysmem, IOP_PERIPH_BASE,
                             IOP_PERIPH_SIZE);
    memory_region_add_subregion(&s->mem, IOP_PERIPH_BASE, &s->periph);
    memory_region_init_io(&s->ap_vic_tap, OBJECT(s), &ap_vic_tap_ops, s, "iop.ap-vic0", 0x1000);
    memory_region_add_subregion_overlap(&s->mem, S5L8930_VIC_BASE(0), &s->ap_vic_tap, 1);

    cpuobj = object_new(ARM_CPU_TYPE_NAME("arm946"));
    object_property_set_link(cpuobj, "memory", OBJECT(&s->mem), &error_abort);
    object_property_set_bool(cpuobj, "start-powered-off", true, &error_abort);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    s->cpu = ARM_CPU(cpuobj);
    define_arm_cp_regs(s->cpu, iop_cp_reginfo);
    qemu_register_reset(s5l8930_iop_core_reset, s);

    /* The IOP's VICs live on the shared bus: the AP rings the doorbell through
     * VIC0's SOFTINT, the firmware programs enables and vector addresses. */
    s->vic[0] = pl192_manual_init((char *)"iop-vic0",
                                  qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ),
                                  qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ),
                                  NULL);
    memory_region_add_subregion(s->sysmem, S5L8930_IOP_VIC_BASE, &PL192(s->vic[0])->iomem);
    memory_region_init_io(&s->vic0_tap, OBJECT(s), &vic0_tap_ops, s, "iop.vic0-tap", 0x1000);
    memory_region_add_subregion_overlap(s->sysmem, S5L8930_IOP_VIC_BASE, &s->vic0_tap, 1);
    for (i = 1; i < IOP_VIC_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("iop-vic%d", i);
        s->vic[i] = pl192_manual_init(name, NULL);
        memory_region_add_subregion(s->sysmem, S5L8930_IOP_VIC_BASE + i * IOP_VIC_STRIDE,
                                    &PL192(s->vic[i])->iomem);
        PL192(s->vic[i])->daisy = PL192(s->vic[i - 1]);
    }
}

/* The CPU and the VICs migrate themselves; the firmware's place at address 0 is this device's. */
static int iop_core_pre_load(void *opaque)
{
    S5L8930IOPCoreState *s = opaque;

    if (s->fw_mapped) {
        memory_region_del_subregion(&s->mem, &s->fw);
        object_unparent(OBJECT(&s->fw));
        s->fw_mapped = false;
    }
    return 0;
}

static int iop_core_post_load(void *opaque, int version_id)
{
    S5L8930IOPCoreState *s = opaque;

    if (s->fw_mapped) {
        if (s->fw_base < S5L8930_DRAM_BASE ||
            (uint64_t)s->fw_base + s->fw_size > S5L8930_DRAM_BASE + memory_region_size(s->dram)) {
            return -EINVAL;
        }
        memory_region_init_alias(&s->fw, OBJECT(s), "iop.fw", s->dram,
                                 s->fw_base - S5L8930_DRAM_BASE, s->fw_size);
        memory_region_add_subregion_overlap(&s->mem, 0, &s->fw, 1);
    }
    return 0;
}

static const VMStateDescription vmstate_iop_core = {
    .name = TYPE_S5L8930_IOP_CORE,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = iop_core_pre_load,
    .post_load = iop_core_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(fw_mapped, S5L8930IOPCoreState),
        VMSTATE_UINT32(fw_base, S5L8930IOPCoreState),
        VMSTATE_UINT32(fw_size, S5L8930IOPCoreState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s5l8930_iop_core_properties[] = {
    DEFINE_PROP_LINK("dram", S5L8930IOPCoreState, dram, TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("sysmem", S5L8930IOPCoreState, sysmem, TYPE_MEMORY_REGION, MemoryRegion *),
};

static void s5l8930_iop_core_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8930_iop_core_realize;
    dc->vmsd = &vmstate_iop_core;
    device_class_set_props(dc, s5l8930_iop_core_properties);
    dc->user_creatable = false;
}

static const TypeInfo s5l8930_iop_core_info = {
    .name = TYPE_S5L8930_IOP_CORE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(S5L8930IOPCoreState),
    .class_init = s5l8930_iop_core_class_init,
};

static void s5l8930_iop_core_register(void)
{
    type_register_static(&s5l8930_iop_core_info);
}

type_init(s5l8930_iop_core_register)
