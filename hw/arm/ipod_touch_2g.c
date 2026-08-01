#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/boot.h"
#include "exec/address-spaces.h"
#include "hw/misc/unimp.h"
#include "hw/irq.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "sysemu/reset.h"
#include "hw/platform-bus.h"
#include "hw/block/flash.h"
#include "hw/qdev-clock.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_2g.h"
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "target/arm/cpregs.h"
#include "qemu/error-report.h"
#include "ui/input.h"

// The D1759 PMU raises this GPIO IRQ when a wake button (hold/menu) is pressed.
// It lands in the same GPIO interrupt group the button code already drives.
#define PMU_WAKE_IRQ 0x61

#define VMSTATE_IT2G_CPREG(name) \
        VMSTATE_UINT64(IT2G_CPREG_VAR_NAME(name), IPodTouchMachineState)

#define IT2G_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access, p_reset) \
    {                                                                              \
        .cp = 15,                                              \
        .name = #p_name, .opc0 = p_op0, .crn = p_crn, .crm = p_crm,                \
        .opc1 = p_op1, .opc2 = p_op2, .access = p_access, .resetvalue = p_reset,   \
        .state = ARM_CP_STATE_AA32, .type = ARM_CP_OVERRIDE,                       \
        .fieldoffset = offsetof(IPodTouchMachineState, IT2G_CPREG_VAR_NAME(p_name))           \
                       - offsetof(ARMCPU, env)                                     \
    }

#define IT2G_CPREG_DEF_QEMU_CALL \
    {                            \
        .cp = 15,                \
        .name = "QEMU_CALL",     \
        .opc0 = 0,               \
        .opc1 = 3,               \
        .crn = 15,               \
        .crm = 15,               \
        .opc2 = 0,               \
        .access = PL0_RW,        \
        .resetvalue = 0,         \
        .state = ARM_CP_STATE_AA32, \
        .type = ARM_CP_IO,       \
        .fieldoffset = offsetof(IPodTouchMachineState, IT2G_CPREG_VAR_NAME(QEMU_CALL)) \
                       - offsetof(ARMCPU, env), \
        .readfn = qemu_call_status, \
        .writefn = qemu_call,     \
    }

const int S5L8900_GPIO_IRQS[5] = { S5L8900_GPIO_G0_IRQ, S5L8900_GPIO_G1_IRQ, S5L8900_GPIO_G2_IRQ, S5L8900_GPIO_G3_IRQ, S5L8900_GPIO_G4_IRQ };

static void allocate_ram(MemoryRegion *top, const char *name, uint32_t addr, uint32_t size)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion(top, addr, sec);
}

/*
 * Host wall-clock source for the boot-time clock patch. The 2G has no RTC iOS
 * reads at boot (getGMTTimeOfDay returns 0 -> the calendar starts at 1900), so we
 * patch _PEGetGMTTimeOfDay to read this cp15 register, which returns host UTC
 * seconds. XNU seeds its calendar from it once and its tick advances from there;
 * hand it UTC, not localtime -- iOS applies its own timezone. This is a sibling
 * of the QEMU_CALL reg (opc2=0); opc2=1 keeps it separate from the socket tunnel.
 */
static uint64_t host_gmt_seconds(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return (uint64_t)(uint32_t)time(NULL);
}
static void host_gmt_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t v)
{
    /* read-only clock source; ignore writes */
}

static const ARMCPRegInfo it2g_cp_reginfo_tcg[] = {
    IT2G_CPREG_DEF(REG0, 0, 0, 7, 6, 0, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 15, 2, 4, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 7, 14, 0, PL1_RW, 0),
    IT2G_CPREG_DEF(REG1, 0, 0, 7, 10, 0, PL1_RW, 0),
    IT2G_CPREG_DEF_QEMU_CALL,
    { .cp = 15, .name = "HOST_GMT_SECONDS",
      .opc0 = 0, .opc1 = 3, .crn = 15, .crm = 15, .opc2 = 1,
      .access = PL0_RW, .state = ARM_CP_STATE_AA32, .type = ARM_CP_IO,
      .readfn = host_gmt_seconds, .writefn = host_gmt_write },
};

static void ipod_touch_cpu_setup(MachineState *machine, MemoryRegion **sysmem, ARMCPU **cpu, AddressSpace **nsas)
{
    Object *cpuobj = object_new(machine->cpu_type);
    *cpu = ARM_CPU(cpuobj);
    CPUState *cs = CPU(*cpu);

    *sysmem = get_system_memory();

    object_property_set_link(cpuobj, "memory", OBJECT(*sysmem), &error_abort);

    object_property_set_bool(cpuobj, "has_el3", false, NULL);

    object_property_set_bool(cpuobj, "has_el2", false, NULL);

    object_property_set_bool(cpuobj, "realized", true, &error_fatal);

    *nsas = cpu_get_address_space(cs, ARMASIdx_NS);

    define_arm_cp_regs(*cpu, it2g_cp_reginfo_tcg);

    object_unref(cpuobj);
}

/*
 * Put the bootrom back at the reset vector.
 *
 * "vrom" is plain RAM at address 0, filled from the bootrom file once during
 * ipod_touch_memory_setup(). Address 0 is also the ARM exception vector page,
 * which the running guest happily writes over. So by the time anything asks for
 * a reset, the bytes at VROM_MEM_BASE are whatever iOS left there, and the CPU
 * restarts into that instead of the bootrom -- the machine never boots again.
 *
 * That is why the guest's post-fsck reboot went nowhere: the watchdog really
 * did call qemu_system_reset_request(), and the CPU really was reset, but it
 * resumed executing junk. A bare "system_reset" from the monitor against a
 * healthy machine failed exactly the same way, which is what showed this is a
 * machine-model problem rather than anything the guest did.
 *
 * Re-staging it on every reset costs one file read per boot and makes the reset
 * vector mean what it says.
 */
static void ipod_touch_load_bootrom(IPodTouchMachineState *nms)
{
    uint8_t *file_data = NULL;
    gsize fsize;

    if (g_file_get_contents(nms->bootrom_path, (char **)&file_data, &fsize, NULL)) {
        address_space_rw(nms->nsas, VROM_MEM_BASE, MEMTXATTRS_UNSPECIFIED,
                         file_data, fsize, 1);
        g_free(file_data);
    }
}

/*
 * IT_DIRECT_IBOOT / IT_DIRECT_LLB: boot-chain substitution (explicitly
 * authorised for the 3.1.3 bring-up).
 *
 * iOS 3.0+ personalises the signed boot chain per-device, and the S5L8720
 * bootrom rejects the 7E18 LLB no matter how we forge the PKE check -- it
 * recomputes the image hash itself and drops to the DFU wait loop. So instead
 * of satisfying the bootrom we skip it, exactly as devos50 (iPod touch 1G, no
 * bootrom dump) and DJHartley's iEmu (-option-rom unencrypted iBoot) did: load
 * a *decrypted* iBoot straight into its own RAM region and enter it.
 *
 * The decrypted images are raw (they begin with the ARM vector table). Their
 * intended load address is the absolute value baked into the vector table at
 * offset 0x20: 7E18 iBoot -> 0x0ff00000 (== IBOOT_MEM_BASE), 7E18 LLB ->
 * 0x22000000 (== LLB region). We honour those.
 *
 * IT_DIRECT_LLB, if set, is staged first (it is what normally initialises DRAM
 * on real hardware); on QEMU DRAM is always-present RAM so iBoot alone is
 * usually enough, but this lets us reproduce the full LLB->iBoot handoff if
 * iBoot turns out to depend on state LLB leaves behind.
 */
#define LLB_LOAD_BASE 0x22000000

/*
 * IT_INJECT_DT: 3.1.3 device-tree bring-up (Option B3).
 *
 * The 7E18 iBoot loads and decrypts the kernelcache from HFS, but then fails to
 * load the device tree: its load_and_set_device_tree() (VA 0x0ff0f498) calls
 * image_load() (VA 0x0ff1998c) on the NOR 'dtre' image, which is rejected at
 * signature validation before any GID decrypt is attempted -- and a global
 * security-state change (forge/demote) to permit it breaks the kernelcache.
 *
 * Instead we hand iBoot an already-decrypted device tree. load_and_set_device_tree
 * (VA 0x0ff0f498) sets its DT-address global (g_dt_addr @ 0x0ff27560) to
 * 0x0BF00000 and its DT-size global (g_dt_size @ 0x0ff27564) to the enumerated
 * image size *before* the image_load() call; on image_load() success it returns
 * those to the caller, which then dt_deserialize()s the blob at g_dt_addr into
 * iBoot's node list (the list UpdateDeviceTree/AllocateMemoryRange walk).
 *
 * iBoot zeroes DRAM (both the insecure 0x08000000 and secure 0x0B000000 banks)
 * during early init, so a device tree dropped at 0x0BF00000 at reset is gone long
 * before the DT load. But the "llb"/SRAM region at 0x22000000 is NOT cleared (it
 * is where the SecureROM/LLB run on real hardware) and iBoot keeps it mapped. So
 * we stage the decrypted serialized device tree at 0x22000000 and rewrite the
 * failing image_load() call site (VA 0x0ff0f4da) into a 16-byte thunk that copies
 * the blob into place right when the DT is loaded (after the kernelcache
 * decompress that would otherwise clobber it):
 *
 *     movs r0,#0xbf ; lsls r0,r0,#20      ; r0 = 0x0BF00000 (dst = g_dt_addr)
 *     movs r1,#0x22 ; lsls r1,r1,#24      ; r1 = 0x22000000 (src = staging)
 *     ldr  r2,[r5]                        ; r2 = g_dt_size  (len, already set)
 *     blx  0x0ff1b474                     ; iBoot memcpy(dst, src, len)
 *     b    0x0ff0f4ea                     ; fall into the success/out-param path
 *
 * memcpy preserves r4/r5/r6/r8, so the function's success tail returns g_dt_addr
 * and g_dt_size to the caller exactly as a real image_load would. This touches
 * ONLY the dtre path; the kernelcache still validates and decrypts normally, and
 * a global security-state change (forge/demote) -- which breaks the kernelcache --
 * is avoided. Gated entirely behind IT_INJECT_DT; 2.1.1 is untouched.
 */
#define DT_STAGING_BASE     0x22000000   /* uncleared SRAM/"llb" region */
#define IBOOT_DT_LOAD_PATCH 0xf4da       /* VA offset of the `bl image_load` */

/*
 * Top of the "insecure" DRAM bank (0x08000000 + 0x3000000). Measured to be
 * zeroed by iBoot and then left untouched through kernel load, and it is inside
 * the kernel's static map (required, see ipod_touch_stage_ramdisk).
 */
#define IT_RAMDISK_DEFAULT_BASE 0x0A000000

/*
 * IT_RAMDISK: stage a filesystem image into guest DRAM so the 3.1.3 kernel can
 * use it as an md0 memory device (see /chosen/memory-map "RAMDisk" in the
 * injected device tree). XNU consumes the entry as
 *     mdevadd(-1, ml_static_ptovirt(paddr) >> 12, len >> 12, 0)
 * and ml_static_ptovirt() is only valid inside the kernel's static DRAM
 * mapping, so the image MUST live in DRAM -- it cannot be parked in a private
 * region outside it.
 *
 * The catch is that iBoot zeroes DRAM during early init, so anything staged at
 * machine-init time is wiped before the kernel runs. IT_RAMDISK_BASE lets us
 * probe where (if anywhere) a blob survives; the value is also what the DT's
 * RAMDisk entry must point at. Purely diagnostic/bring-up, gated on the env var.
 */
static void ipod_touch_ramdisk_stage_now(void *opaque)
{
    IPodTouchMachineState *nms = (IPodTouchMachineState *)opaque;
    const char *rd_path = getenv("IT_RAMDISK");
    const char *rd_base_s = getenv("IT_RAMDISK_BASE");
    uint8_t *rd_data = NULL;
    gsize rd_size;
    uint32_t rd_base = rd_base_s ? (uint32_t)strtoul(rd_base_s, NULL, 0)
                                 : IT_RAMDISK_DEFAULT_BASE;

    if (!g_file_get_contents(rd_path, (char **)&rd_data, &rd_size, NULL)) {
        fprintf(stderr, "[IT_RAMDISK] could not read '%s'\n", rd_path);
        return;
    }

    address_space_rw(nms->nsas, rd_base, MEMTXATTRS_UNSPECIFIED,
                     rd_data, rd_size, 1);
    g_free(rd_data);

    fprintf(stderr, "[IT_RAMDISK] staged '%s' (%llu bytes) at 0x%08x\n",
            rd_path, (unsigned long long)rd_size, rd_base);
}

static void ipod_touch_stage_ramdisk(IPodTouchMachineState *nms)
{
    const char *rd_delay_s = getenv("IT_RAMDISK_DELAY_MS");
    uint64_t delay_ms = rd_delay_s ? strtoull(rd_delay_s, NULL, 0) : 15000;
    QEMUTimer *t;

    if (!getenv("IT_RAMDISK")) {
        return;
    }

    /*
     * Staging cannot happen at machine-init time: iBoot zeroes DRAM during
     * early init, so the image would be wiped long before the kernel looks at
     * it (measured -- a blob written at reset reads back as zeroes, and the low
     * bank is reused by iBoot for img3 buffers). Defer the copy instead. The
     * usable window is wide: iBoot finishes zeroing in the first second or so,
     * and the kernel does not touch the RAMDisk range until it mounts root tens
     * of seconds later, so a one-shot timer is sufficient and needs no guest
     * patching. IT_RAMDISK_DELAY_MS tunes it.
     */
    t = timer_new_ms(QEMU_CLOCK_VIRTUAL, ipod_touch_ramdisk_stage_now, nms);
    timer_mod(t, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + delay_ms);
    fprintf(stderr, "[IT_RAMDISK] staging scheduled at T+%llu ms\n",
            (unsigned long long)delay_ms);
}

static void ipod_touch_inject_device_tree(IPodTouchMachineState *nms)
{
    const char *dt_path = getenv("IT_INJECT_DT");
    uint8_t *dt_data = NULL;
    gsize dt_size;
    /* 16-byte thunk (see comment above): memcpy(0x0BF00000, 0x22000000, g_dt_size)
     * then branch to the success path. */
    static const uint8_t patch[16] = {
        0xbf, 0x20, 0x00, 0x05,   /* movs r0,#0xbf ; lsls r0,r0,#20  */
        0x22, 0x21, 0x09, 0x06,   /* movs r1,#0x22 ; lsls r1,r1,#24  */
        0x2a, 0x68,               /* ldr  r2,[r5]                    */
        0x0b, 0xf0, 0xc6, 0xef,   /* blx  0x0ff1b474 (memcpy)        */
        0xff, 0xe7,               /* b    0x0ff0f4ea                 */
    };

    if (!dt_path) {
        return;
    }

    if (!g_file_get_contents(dt_path, (char **)&dt_data, &dt_size, NULL)) {
        fprintf(stderr, "[IT_INJECT_DT] could not read '%s'\n", dt_path);
        return;
    }

    address_space_rw(nms->nsas, DT_STAGING_BASE, MEMTXATTRS_UNSPECIFIED,
                     dt_data, dt_size, 1);
    g_free(dt_data);

    address_space_rw(nms->nsas, IBOOT_MEM_BASE + IBOOT_DT_LOAD_PATCH,
                     MEMTXATTRS_UNSPECIFIED, (uint8_t *)patch, sizeof(patch), 1);

    fprintf(stderr, "[IT_INJECT_DT] staged device tree '%s' (%llu bytes) at "
            "0x%08x and patched iBoot dtre image_load\n",
            dt_path, (unsigned long long)dt_size, DT_STAGING_BASE);
}

static void ipod_touch_load_direct_boot(IPodTouchMachineState *nms)
{
    const char *iboot_path = getenv("IT_DIRECT_IBOOT");
    const char *llb_path = getenv("IT_DIRECT_LLB");
    uint8_t *file_data = NULL;
    gsize fsize;

    if (llb_path && g_file_get_contents(llb_path, (char **)&file_data, &fsize, NULL)) {
        address_space_rw(nms->nsas, LLB_LOAD_BASE, MEMTXATTRS_UNSPECIFIED,
                         file_data, fsize, 1);
        g_free(file_data);
        file_data = NULL;
        fprintf(stderr, "[IT_DIRECT] staged LLB '%s' (%llu bytes) at 0x%08x\n",
                llb_path, (unsigned long long)fsize, LLB_LOAD_BASE);
    }

    if (iboot_path && g_file_get_contents(iboot_path, (char **)&file_data, &fsize, NULL)) {
        address_space_rw(nms->nsas, IBOOT_MEM_BASE, MEMTXATTRS_UNSPECIFIED,
                         file_data, fsize, 1);
        g_free(file_data);
        fprintf(stderr, "[IT_DIRECT] staged iBoot '%s' (%llu bytes) at 0x%08x\n",
                iboot_path, (unsigned long long)fsize, IBOOT_MEM_BASE);
        /*
         * iBoot's miu_init reads SYSIC[0x44] bits[31:24] as the boot security
         * epoch and panics ("Epoch Mismatch") unless it equals the epoch baked
         * into the image (4 for the S5L8720 / iPod touch 2G). On real hardware
         * that top byte is a read-only fused value the SecureROM never writes;
         * the low bits are the POWER_ID power-control scratch. We skip the ROM,
         * so the SYSIC model synthesises the epoch top byte on read whenever
         * IT_DIRECT_IBOOT is set -- see ipod_touch_sysic_read(). Nothing to do
         * here.
         */

        /* Hand iBoot a pre-decrypted device tree (3.1.3 bring-up). Must run
         * after the iBoot image is staged so the code patch lands on top of it. */
        ipod_touch_inject_device_tree(nms);
        ipod_touch_stage_ramdisk(nms);
    }
}

static void ipod_touch_cpu_reset(void *opaque)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE((MachineState *)opaque);
    ARMCPU *cpu = nms->cpu;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    ipod_touch_load_bootrom(nms);

    if (getenv("IT_DIRECT_IBOOT")) {
        /* Boot-chain substitution: enter the decrypted iBoot directly, skipping
         * the bootrom + LLB signature/personalisation checks. */
        ipod_touch_load_direct_boot(nms);
        cpu_set_pc(CPU(cpu), getenv("IT_DIRECT_LLB") ? LLB_LOAD_BASE : IBOOT_MEM_BASE);
        return;
    }

    //env->regs[0] = nms->kbootargs_pa;
    //cpu_set_pc(CPU(cpu), 0xc00607ec);
    cpu_set_pc(CPU(cpu), VROM_MEM_BASE);
    //env->regs[0] = 0x9000000;
    //cpu_set_pc(CPU(cpu), LLB_BASE + 0x100);
    //cpu_set_pc(CPU(cpu), VROM_MEM_BASE);
}

static void ipod_touch_memory_setup(MachineState *machine, MemoryRegion *sysmem, AddressSpace *nsas)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(machine);

    allocate_ram(sysmem, "insecure_ram", INSECURE_RAM_MEM_BASE, 0x3000000);
    allocate_ram(sysmem, "secure_ram", SECURE_RAM_MEM_BASE, 0x4B04000);
    allocate_ram(sysmem, "iboot", IBOOT_MEM_BASE, 0x100000);
    allocate_ram(sysmem, "llb", 0x22000000, 0x100000);
    allocate_ram(sysmem, "sram1", SRAM1_MEM_BASE, 0x100000);
    allocate_ram(sysmem, "framebuffer", FRAMEBUFFER_MEM_BASE, 0x400000);
    allocate_ram(sysmem, "edgeic", EDGEIC_MEM_BASE, 0x1000);
    allocate_ram(sysmem, "swi", SWI_MEM_BASE, 0x1000);
    allocate_ram(sysmem, "h264", H264_MEM_BASE, 0x4000);

    /* The bootrom itself is (re)staged by ipod_touch_load_bootrom(), which also
     * runs on every reset -- see the note there. */
    allocate_ram(sysmem, "vrom", 0x0, 0x20000);
    ipod_touch_load_bootrom(nms);
}

static char *ipod_touch_get_bootrom_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->bootrom_path);
}

static void ipod_touch_set_bootrom_path(Object *obj, const char *value, Error **errp)
{
    gboolean bootrom_exists = g_file_test(value, G_FILE_TEST_EXISTS);
    if(!bootrom_exists) {
        error_report("bootrom at path \"%s\" must exist", value);
        exit(1);
    }
    
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->bootrom_path, value, sizeof(nms->bootrom_path));
}

static char *ipod_touch_get_nor_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nor_path);
}

static void ipod_touch_set_nor_path(Object *obj, const char *value, Error **errp)
{
    gboolean nor_exists = g_file_test(value, G_FILE_TEST_EXISTS);
    if(!nor_exists) {
        error_report("NOR at path \"%s\" must exist", value);
        exit(1);
    }

    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->nor_path, value, sizeof(nms->nor_path));
}

static bool ipod_touch_get_wifi(Object *obj, Error **errp)
{
    return IPOD_TOUCH_MACHINE(obj)->wifi;
}

static void ipod_touch_set_wifi(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_MACHINE(obj)->wifi = value;
}

static char *ipod_touch_get_boot_args(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->boot_args);
}

static void ipod_touch_set_boot_args(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->boot_args, value, sizeof(nms->boot_args));
}

static char *ipod_touch_get_nand_path(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nand_path);
}

static void ipod_touch_set_nand_path(Object *obj, const char *value, Error **errp)
{
    gboolean nand_exists = g_file_test(value, G_FILE_TEST_IS_DIR);
    if(!nand_exists) {
        error_report("NAND at path \"%s\" must be a directory", value);
        exit(1);
    }
    
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->nand_path, value, sizeof(nms->nand_path));
}

static char *ipod_touch_get_nand_overlay(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->nand_overlay);
}

static void ipod_touch_set_nand_overlay(Object *obj, const char *value, Error **errp)
{
    /* Create the overlay directory (and its cs0..cs3 subdirs) on demand so the
     * user only has to name a path. Writes land here; the base 'nand' image is
     * never modified. */
    if (g_mkdir_with_parents(value, 0755) != 0 &&
        !g_file_test(value, G_FILE_TEST_IS_DIR)) {
        error_report("NAND overlay at path \"%s\" could not be created", value);
        exit(1);
    }

    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->nand_overlay, value, sizeof(nms->nand_overlay));
}

static char *ipod_touch_get_usb_tcp_addr(Object *obj, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    return g_strdup(nms->usb_tcp_addr);
}

static void ipod_touch_set_usb_tcp_addr(Object *obj, const char *value, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    g_strlcpy(nms->usb_tcp_addr, value, sizeof(nms->usb_tcp_addr));
}

static bool ipod_touch_get_usb_attached(Object *obj, Error **errp)
{
    return IPOD_TOUCH_MACHINE(obj)->usb_attached;
}

static void ipod_touch_set_usb_attached(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_MACHINE(obj)->usb_attached = value;
}

static bool ipod_touch_get_usb_patch_mux_gate(Object *obj, Error **errp)
{
    return IPOD_TOUCH_MACHINE(obj)->usb_patch_mux_gate;
}

static void ipod_touch_set_usb_patch_mux_gate(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_MACHINE(obj)->usb_patch_mux_gate = value;
}

static bool ipod_touch_get_mbx_irq(Object *obj, Error **errp)
{
    return IPOD_TOUCH_MACHINE(obj)->mbx_irq;
}

static void ipod_touch_set_mbx_irq(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_MACHINE(obj)->mbx_irq = value;
}

/*
 * Accelerometer controls, forwarded to the LIS302DL. These live on the machine
 * (a stable /machine QOM path) so a host can drive rotation/shake over QMP:
 *   qom-set path=/machine property=accel-orientation value=3   (0-6, UIDeviceOrientation)
 *   qom-set path=/machine property=accel-shake value=true
 *   qom-set path=/machine property=accel-x value=64
 * The i2c device itself cannot be given a fixed path (it is parented to the bus).
 */
static void ipod_touch_get_accel_orientation(Object *obj, Visitor *v, const char *name,
                                             void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t val = nms->lis302dl_state ? nms->lis302dl_state->orientation : 0;
    visit_type_int(v, name, &val, errp);
}

static void ipod_touch_set_accel_orientation(Object *obj, Visitor *v, const char *name,
                                             void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t val;
    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    if (nms->lis302dl_state) {
        lis302dl_apply_orientation(nms->lis302dl_state, (uint32_t)val);
    }
}

static void ipod_touch_set_accel_axis(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    int64_t val;
    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    /* name is "accel-x" / "accel-y" / "accel-z" */
    if (nms->lis302dl_state) {
        lis302dl_set_axis_value(nms->lis302dl_state, name[strlen(name) - 1], (int)val);
    }
}

static void ipod_touch_set_accel_shake(Object *obj, Visitor *v, const char *name,
                                       void *opaque, Error **errp)
{
    IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(obj);
    bool val;
    if (!visit_type_bool(v, name, &val, errp)) {
        return;
    }
    if (val && nms->lis302dl_state) {
        lis302dl_shake(nms->lis302dl_state);
    }
}

static void ipod_touch_instance_init(Object *obj)
{
    object_property_add_str(obj, "bootrom", ipod_touch_get_bootrom_path, ipod_touch_set_bootrom_path);
    object_property_set_description(obj, "bootrom", "Path to the S5L8720 bootrom binary");

	object_property_add_str(obj, "nor", ipod_touch_get_nor_path, ipod_touch_set_nor_path);
    object_property_set_description(obj, "nor", "Path to the S5L8720 NOR image");

    object_property_add_str(obj, "nand", ipod_touch_get_nand_path, ipod_touch_set_nand_path);
    object_property_set_description(obj, "nand", "Path to the NAND files");

    object_property_add_str(obj, "nandrw", ipod_touch_get_nand_overlay, ipod_touch_set_nand_overlay);
    object_property_set_description(obj, "nandrw", "Path to a writable NAND overlay directory (copy-on-write); NAND writes are stored here and read back on top of the read-only 'nand' image");

    object_property_add_bool(obj, "wifi", ipod_touch_get_wifi, ipod_touch_set_wifi);
    object_property_set_description(obj, "wifi",
        "Present a BCM4325 on the SDIO bus. Off by default: the dongle "
        "emulation is incomplete, so the driver attaches and then gets stuck");

    /*
     * Left empty on purpose. The FMSS poke that used to inject a default set
     * of boot-args wrote them into a fixed address inside 5F138 iBoot's BSS
     * and has been removed, so 2.1.1 now boots with an empty
     * gBootArgs.commandLine unless boot-args= is passed. Seeding this property
     * with the old default instead looked like it stopped the stock NOR from
     * reaching its serial banner, so nor_set_boot_args() wants verifying
     * before it takes over as the default path.
     */
    object_property_add_str(obj, "boot-args", ipod_touch_get_boot_args, ipod_touch_set_boot_args);
    object_property_set_description(obj, "boot-args",
        "Replace the boot-args variable in the NOR's nvram, in memory only. "
        "Useful for kernel debug flags such as \"io=0xffff\"");

    object_property_add_str(obj, "usb-tcp-addr", ipod_touch_get_usb_tcp_addr, ipod_touch_set_usb_tcp_addr);
    object_property_set_description(obj, "usb-tcp-addr",
        "host:port of a USB-over-TCP host bridge (e.g. usbmuxd's QEMU backend). "
        "Empty disables the link");

    /* On by default: the emulated device is effectively tethered to the host. */
    IPOD_TOUCH_MACHINE(obj)->usb_attached = true;
    /* On by default: this gates the verified MBX MMU request/ack mirror
     * (ipod_touch_mbx.c), which stops the ~21M-read disable-loop spin that
     * froze OpenGL ES apps. The 2D boot path does not touch the MMU handshake,
     * so defaulting it on is safe there. */
    IPOD_TOUCH_MACHINE(obj)->mbx_irq = true;
    object_property_add_bool(obj, "mbx-irq", ipod_touch_get_mbx_irq, ipod_touch_set_mbx_irq);
    object_property_set_description(obj, "mbx-irq",
        "Raise a completion interrupt for the unemulated MBX GPU so an app that "
        "submits work does not wait for it forever. Cannot make anything render");

    object_property_add_bool(obj, "usb-attached", ipod_touch_get_usb_attached, ipod_touch_set_usb_attached);
    object_property_set_description(obj, "usb-attached",
        "Report a USB cable as present to the PMU. iOS leaves the whole USB device "
        "stack parked until it sees this");

    object_property_add_bool(obj, "usb-patch-mux-gate", ipod_touch_get_usb_patch_mux_gate,
                             ipod_touch_set_usb_patch_mux_gate);
    object_property_set_description(obj, "usb-patch-mux-gate",
        "Patch the kernel so the USB stack goes on bus even though the PTP interface "
        "function never registers a driver. Firmware-build-specific (2.1.1 / 5F138)");

    /* Accelerometer (LIS302DL) host controls; see the getters/setters above. */
    object_property_add(obj, "accel-orientation", "int",
                        ipod_touch_get_accel_orientation,
                        ipod_touch_set_accel_orientation, NULL, NULL);
    object_property_set_description(obj, "accel-orientation",
        "Set the reported device orientation: 1=portrait, 2=portrait-upside-down, "
        "3=landscape-home-right, 4=landscape-home-left, 5=face-up, 6=face-down");
    object_property_add(obj, "accel-x", "int", NULL, ipod_touch_set_accel_axis, NULL, NULL);
    object_property_add(obj, "accel-y", "int", NULL, ipod_touch_set_accel_axis, NULL, NULL);
    object_property_add(obj, "accel-z", "int", NULL, ipod_touch_set_accel_axis, NULL, NULL);
    object_property_add(obj, "accel-shake", "bool", NULL, ipod_touch_set_accel_shake, NULL, NULL);
}

static inline qemu_irq s5l8900_get_irq(IPodTouchMachineState *s, int n)
{
    return s->irq[n / S5L8720_VIC_SIZE][n % S5L8720_VIC_SIZE];
}

static uint32_t s5l8720_usb_hwcfg[] = {
    0,
    0x7a8f60d0,
    0x082000e8,
    0x01f08024
};

static void ipod_touch_key_event(void *opaque, int keycode)
{
    bool do_irq = false;
    int gpio_group = 0, gpio_selector = 0;
    uint32_t button_gpio = 0;

    IPodTouchMultitouchState *s = (IPodTouchMultitouchState *)opaque;
    if(keycode == KEY_P_DOWN || keycode == KEY_P_UP) {
        // power button
        gpio_group = GPIO_BUTTON_POWER_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_POWER_IRQ % NUM_GPIO_PINS;
        button_gpio = GPIO_BUTTON_POWER;

        if(keycode == KEY_P_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_POWER)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_POWER);
            do_irq = true;
        }
        else if(keycode == KEY_P_UP && !gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_POWER)) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_POWER);
            do_irq = true;
        }
    }
    else if(keycode == KEY_H_DOWN || keycode == KEY_H_UP) {
        // home button
        gpio_group = GPIO_BUTTON_HOME_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_HOME_IRQ % NUM_GPIO_PINS;
        button_gpio = GPIO_BUTTON_HOME;

        if(keycode == KEY_H_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_HOME)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_HOME);
            do_irq = true;
        }
        else if(keycode == KEY_H_UP && !gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_HOME)) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_HOME);
            do_irq = true;
        }
    }
    else if(keycode == KEY_MIN_DOWN || keycode == KEY_MIN_UP) {
        // volume down button
        gpio_group = GPIO_BUTTON_VOLDOWN_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_VOLDOWN_IRQ % NUM_GPIO_PINS;
        button_gpio = GPIO_BUTTON_VOLDOWN;

        if(keycode == KEY_MIN_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN);
            do_irq = true;
        }
        else if(keycode == KEY_MIN_UP && !gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN)) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLDOWN);
            do_irq = true;
        }
    }
    else if(keycode == KEY_PLUS_DOWN || keycode == KEY_PLUS_UP) {
        // volume up button
        gpio_group = GPIO_BUTTON_VOLUP_IRQ / NUM_GPIO_PINS;
        gpio_selector = GPIO_BUTTON_VOLUP_IRQ % NUM_GPIO_PINS;
        button_gpio = GPIO_BUTTON_VOLUP;

        if(keycode == KEY_PLUS_DOWN && gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP)) {
            gpio_set_on(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP);
            do_irq = true;
        }
        else if(keycode == KEY_PLUS_UP && !gpio_is_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP)) {
            gpio_set_off(s->gpio_state->gpio_state, GPIO_BUTTON_VOLUP);
            do_irq = true;
        }
    }
    else return;

    // Only raise a GPIO interrupt on a genuine press/release edge. Without this
    // guard, SDL key auto-repeat (which resends KEY_*_DOWN while a key is held)
    // would fire a fresh interrupt each time even though the button state did
    // not change, spamming the interrupt controller with phantom presses.
    if(do_irq) {
        // Mirror the physical pin level into the sysic. GPIO_INTLEVEL was read
        // by the guest (sysic read handler) but never written anywhere, so it
        // always returned 0 regardless of the real pin state. Reflect the
        // current button level here so it is at least consistent with the pad
        // registers and the latched interrupt status.
        if(gpio_is_on(s->gpio_state->gpio_state, button_gpio)) {
            s->sysic->gpio_int_level[gpio_group] |= (1 << gpio_selector);
        } else {
            s->sysic->gpio_int_level[gpio_group] &= ~(1 << gpio_selector);
        }
        s->sysic->gpio_int_status[gpio_group] |= (1 << gpio_selector);
        qemu_irq_raise(s->sysic->gpio_irqs[gpio_group]);

        // The hold (power) and menu (home) buttons are also wake sources routed
        // through the PMU, which is a nested interrupt controller on the SoC.
        // The awake-state path above goes through the GPIO controller; the wake
        // path goes through the PMU. On a press the PMU latches the button's
        // EVENT_C interrupt bit (reg 0x03) and raises its own interrupt (GPIO
        // IRQ 0x61); iOS reads the EVENT block, decodes the bit, and reads the
        // live STAT reg 0x19 to confirm the button and re-enable the display.
        // hold and menu occupy the same bit positions in EVENT_C and STAT.
        if (button_gpio == GPIO_BUTTON_POWER || button_gpio == GPIO_BUTTON_HOME) {
            uint8_t bit = (button_gpio == GPIO_BUTTON_POWER)
                              ? PMU_STAT_HOLD : PMU_STAT_MENU;
            bool pressed = gpio_is_on(s->gpio_state->gpio_state, button_gpio);

            // reg 0x19 tracks the live button level (set while held, cleared on
            // release); the EVENT_C interrupt is edge-latched on the press only.
            pcf50633_set_stat(PCF50633(s->pmu), bit, pressed);
            if (pressed) {
                pcf50633_latch_wake_event(PCF50633(s->pmu), bit);

                int pmu_group = PMU_WAKE_IRQ / NUM_GPIO_PINS;
                int pmu_selector = PMU_WAKE_IRQ % NUM_GPIO_PINS;
                s->sysic->gpio_int_status[pmu_group] |= (1 << pmu_selector);
                qemu_irq_raise(s->sysic->gpio_irqs[pmu_group]);
            }
        }
    }
}

/*
 * Host keyboard -> guest text input.
 *
 * The legacy ipod_touch_key_event above drives the four hardware buttons from
 * bare letter keys (P/H/-/+), which collides with typing. This modern handler
 * replaces it: it moves the buttons behind the host Command modifier so every
 * plain key is free for text, and queues bare printable keys as unichars for
 * injection into the guest's text-input system. The button/PMU-wake logic is
 * reused unchanged - a Command combo just calls ipod_touch_key_event with the
 * scancode that combo used to be.
 */
static IPodTouchMachineState *s_kbd_nms;
static IPodTouchMultitouchState *s_kbd_mt;

static uint16_t qcode_to_unichar(int q, bool shift)
{
	switch (q) {
	case Q_KEY_CODE_A: return shift ? 'A' : 'a';
	case Q_KEY_CODE_B: return shift ? 'B' : 'b';
	case Q_KEY_CODE_C: return shift ? 'C' : 'c';
	case Q_KEY_CODE_D: return shift ? 'D' : 'd';
	case Q_KEY_CODE_E: return shift ? 'E' : 'e';
	case Q_KEY_CODE_F: return shift ? 'F' : 'f';
	case Q_KEY_CODE_G: return shift ? 'G' : 'g';
	case Q_KEY_CODE_H: return shift ? 'H' : 'h';
	case Q_KEY_CODE_I: return shift ? 'I' : 'i';
	case Q_KEY_CODE_J: return shift ? 'J' : 'j';
	case Q_KEY_CODE_K: return shift ? 'K' : 'k';
	case Q_KEY_CODE_L: return shift ? 'L' : 'l';
	case Q_KEY_CODE_M: return shift ? 'M' : 'm';
	case Q_KEY_CODE_N: return shift ? 'N' : 'n';
	case Q_KEY_CODE_O: return shift ? 'O' : 'o';
	case Q_KEY_CODE_P: return shift ? 'P' : 'p';
	case Q_KEY_CODE_Q: return shift ? 'Q' : 'q';
	case Q_KEY_CODE_R: return shift ? 'R' : 'r';
	case Q_KEY_CODE_S: return shift ? 'S' : 's';
	case Q_KEY_CODE_T: return shift ? 'T' : 't';
	case Q_KEY_CODE_U: return shift ? 'U' : 'u';
	case Q_KEY_CODE_V: return shift ? 'V' : 'v';
	case Q_KEY_CODE_W: return shift ? 'W' : 'w';
	case Q_KEY_CODE_X: return shift ? 'X' : 'x';
	case Q_KEY_CODE_Y: return shift ? 'Y' : 'y';
	case Q_KEY_CODE_Z: return shift ? 'Z' : 'z';
	case Q_KEY_CODE_1: return shift ? '!' : '1';
	case Q_KEY_CODE_2: return shift ? '@' : '2';
	case Q_KEY_CODE_3: return shift ? '#' : '3';
	case Q_KEY_CODE_4: return shift ? '$' : '4';
	case Q_KEY_CODE_5: return shift ? '%' : '5';
	case Q_KEY_CODE_6: return shift ? '^' : '6';
	case Q_KEY_CODE_7: return shift ? '&' : '7';
	case Q_KEY_CODE_8: return shift ? '*' : '8';
	case Q_KEY_CODE_9: return shift ? '(' : '9';
	case Q_KEY_CODE_0: return shift ? ')' : '0';
	case Q_KEY_CODE_MINUS: return shift ? '_' : '-';
	case Q_KEY_CODE_EQUAL: return shift ? '+' : '=';
	case Q_KEY_CODE_BRACKET_LEFT:  return shift ? '{' : '[';
	case Q_KEY_CODE_BRACKET_RIGHT: return shift ? '}' : ']';
	case Q_KEY_CODE_BACKSLASH: return shift ? '|' : '\\';
	case Q_KEY_CODE_SEMICOLON: return shift ? ':' : ';';
	case Q_KEY_CODE_APOSTROPHE: return shift ? '"' : '\'';
	case Q_KEY_CODE_GRAVE_ACCENT: return shift ? '~' : '`';
	case Q_KEY_CODE_COMMA: return shift ? '<' : ',';
	case Q_KEY_CODE_DOT: return shift ? '>' : '.';
	case Q_KEY_CODE_SLASH: return shift ? '?' : '/';
	case Q_KEY_CODE_SPC: return ' ';
	case Q_KEY_CODE_RET: return '\n';
	case Q_KEY_CODE_BACKSPACE: return 0x08;
	case Q_KEY_CODE_TAB: return '\t';
	default: return 0;
	}
}

static void ipod_touch_kbd_enqueue(IPodTouchMachineState *nms, uint16_t ch)
{
	unsigned next = (nms->kbd_tail + 1) % ARRAY_SIZE(nms->kbd_ring);
	if (next == nms->kbd_head) {
		return; /* ring full - drop, rather than overwrite unread input */
	}
	nms->kbd_ring[nms->kbd_tail] = ch;
	nms->kbd_tail = next;
}

/* Command+combo -> the button scancode the legacy handler already understands. */
/*
 * Command+Left / Command+Right turn the device a quarter turn, the way you would
 * physically rotate it.
 *
 * UIDeviceOrientation's numbering is not rotational, so this steps through the
 * physical order rather than incrementing. Holding the device face-on and turning
 * it clockwise moves the home button bottom -> left -> top -> right, that is
 * Portrait(1) -> LandscapeRight(4) -> PortraitUpsideDown(2) -> LandscapeLeft(3),
 * and counter-clockwise is the same cycle reversed.
 */
static void ipod_touch_kbd_rotate(bool clockwise)
{
	/* indexed by the current orientation; [0] is the unset/face-up case */
	static const uint32_t cw[5]  = { 1, 4, 3, 1, 2 };
	static const uint32_t ccw[5] = { 1, 3, 4, 2, 1 };
	IPodTouchMachineState *nms = s_kbd_nms;
	uint32_t cur;

	if (!nms || !nms->lis302dl_state) {
		return;
	}

	cur = nms->lis302dl_state->orientation;
	if (cur > 4) {
		cur = 0; /* never set, or face up/down: start from portrait */
	}
	lis302dl_apply_orientation(nms->lis302dl_state,
	                           clockwise ? cw[cur] : ccw[cur]);
}

static void ipod_touch_kbd_button(int qcode, bool shift, bool down)
{
	int base = -1;
	switch (qcode) {
	case Q_KEY_CODE_L: base = KEY_P; break;          /* Command+L      -> power */
	case Q_KEY_CODE_H: if (shift) base = KEY_H; break;/* Command+Shift+H-> home  */
	case Q_KEY_CODE_MINUS: base = KEY_MIN; break;    /* Command+-      -> vol dn */
	case Q_KEY_CODE_EQUAL: base = KEY_PLUS; break;   /* Command+=      -> vol up */
	case Q_KEY_CODE_LEFT:                            /* Command+Left   -> rotate ccw */
	case Q_KEY_CODE_RIGHT:                           /* Command+Right  -> rotate cw  */
		if (down) {
			ipod_touch_kbd_rotate(qcode == Q_KEY_CODE_RIGHT);
		}
		return;
	default: break;
	}
	if (base < 0 || !s_kbd_mt) {
		return;
	}
	ipod_touch_key_event(s_kbd_mt, down ? base : (base | KEY_UP));
}

static void ipod_touch_kbd_event(DeviceState *dev, QemuConsole *src,
                                 InputEvent *evt)
{
	IPodTouchMachineState *nms = s_kbd_nms;
	InputKeyEvent *k = evt->u.key.data;
	int q = qemu_input_key_value_to_qcode(k->key);
	bool down = k->down;

	if (!nms) {
		return;
	}

	switch (q) {
	case Q_KEY_CODE_META_L:
	case Q_KEY_CODE_META_R:
		nms->kbd_cmd = down;
		return;
	case Q_KEY_CODE_SHIFT:
	case Q_KEY_CODE_SHIFT_R:
		nms->kbd_shift = down;
		return;
	default:
		break;
	}

	if (nms->kbd_cmd) {
		/* buttons live behind Command now */
		ipod_touch_kbd_button(q, nms->kbd_shift, down);
		return;
	}

	if (down) {
		uint16_t ch = qcode_to_unichar(q, nms->kbd_shift);
		if (ch) {
			ipod_touch_kbd_enqueue(nms, ch);
			/* Injection into _GSPostSyntheticKeyEvent is wired up separately;
			 * for now the queue is the interface the drain will consume. */
			if (getenv("IT_KBD_TRACE")) {
				fprintf(stderr, "[KBD] queued 0x%04x '%c'\n", ch,
				        (ch >= 0x20 && ch < 0x7f) ? ch : '.');
			}
		}
	}
}

static const QemuInputHandler ipod_touch_kbd_handler = {
	.name  = "iPod Touch Keyboard",
	.mask  = INPUT_EVENT_MASK_KEY,
	.event = ipod_touch_kbd_event,
};

/*
 * Scripted "slide to power off".
 *
 * iPhone OS 2.1.1 has no remote shutdown: lockdownd has neither a reboot
 * request nor a diagnostics relay, and nothing in the guest runs as root that
 * we can drive. The one path to a *clean* shutdown -- the one that unmounts the
 * root volume and so flushes HFS+'s in-memory catalog to flash -- is the user
 * gesture: hold the hold button until SpringBoard raises its power-off sheet,
 * then drag the slider. So the machine performs that gesture itself.
 *
 * QEMU's own system_powerdown is the trigger, which is exactly what it means
 * elsewhere: ACPI machines send the guest a power-button event and let the OS
 * shut itself down. Here the "power button event" is a synthesised press plus
 * the slide the guest insists on.
 *
 * Everything is timed on QEMU_CLOCK_VIRTUAL so the sequence is deterministic
 * under host load: SpringBoard's hold-to-power-off threshold is measured in
 * guest time, so the hold must be too.
 *
 * The slider geometry is in display pixels on the 320x480 panel, measured from
 * a screendump of the sheet: the knob sits at the left of a track running the
 * width of the screen, vertically centred at y=68.
 */
#define PWROFF_HOLD_MS      3500   /* > SpringBoard's hold threshold           */
#define PWROFF_SETTLE_MS    1500   /* sheet slides in and settles              */
#define PWROFF_DRAG_STEPS   24
#define PWROFF_DRAG_STEP_MS 80
#define PWROFF_KNOB_X       65
#define PWROFF_KNOB_Y       68
#define PWROFF_TRACK_END_X  295

enum {
	PWROFF_IDLE = 0,
	PWROFF_PRESSED,
	PWROFF_SETTLING,
	PWROFF_DRAGGING,
	PWROFF_DONE,
};

/* Same effect as a mouse event on the display, but in panel pixels. */
static void ipod_touch_synth_touch(IPodTouchMachineState *nms,
                                   int px, int py, int state)
{
	IPodTouchLCDState *lcd = nms->lcd_state;
	IPodTouchMultitouchState *mt;

	if (!lcd || !lcd->mt) {
		return;
	}
	mt = lcd->mt;

	mt->prev_touch_x = mt->touch_x;
	mt->prev_touch_y = mt->touch_y;
	mt->touch_x = (float)px / 320.0f;
	mt->touch_y = 1.0f - (float)py / 480.0f;

	if (state && !mt->touch_down) {
		ipod_touch_multitouch_on_touch(mt);
	} else if (!state && mt->touch_down) {
		ipod_touch_multitouch_on_release(mt);
	}
	/* While the finger is down the digitizer's own 10Hz timer keeps emitting
	 * TOUCH_MOVED frames from touch_x/touch_y, so updating them is the move. */
}

static void ipod_touch_powerdown_arm(IPodTouchMachineState *nms, int ms)
{
	timer_mod(nms->pwroff_timer,
	          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
	              (int64_t)ms * SCALE_MS);
}

static void ipod_touch_powerdown_tick(void *opaque)
{
	IPodTouchMachineState *nms = opaque;
	bool trace = getenv("IT_PWROFF_TRACE") != NULL;

	switch (nms->pwroff_phase) {
	case PWROFF_PRESSED:
		/* Hold expired: release the button. The sheet is already up. */
		if (s_kbd_mt) {
			ipod_touch_key_event(s_kbd_mt, KEY_P_UP);
		}
		nms->pwroff_phase = PWROFF_SETTLING;
		ipod_touch_powerdown_arm(nms, PWROFF_SETTLE_MS);
		break;

	case PWROFF_SETTLING:
		ipod_touch_synth_touch(nms, PWROFF_KNOB_X, PWROFF_KNOB_Y, 1);
		nms->pwroff_phase = PWROFF_DRAGGING;
		nms->pwroff_step = 0;
		ipod_touch_powerdown_arm(nms, PWROFF_DRAG_STEP_MS);
		break;

	case PWROFF_DRAGGING: {
		int i = ++nms->pwroff_step;
		int x = PWROFF_KNOB_X +
		        (PWROFF_TRACK_END_X - PWROFF_KNOB_X) * i / PWROFF_DRAG_STEPS;

		if (i < PWROFF_DRAG_STEPS) {
			ipod_touch_synth_touch(nms, x, PWROFF_KNOB_Y, 1);
			ipod_touch_powerdown_arm(nms, PWROFF_DRAG_STEP_MS);
		} else {
			ipod_touch_synth_touch(nms, PWROFF_TRACK_END_X,
			                       PWROFF_KNOB_Y, 0);
			nms->pwroff_phase = PWROFF_DONE;
			if (trace) {
				fprintf(stderr, "[PWROFF] slider released; "
				                "waiting for the guest to halt\n");
			}
		}
		break;
	}

	default:
		break;
	}
}

static void ipod_touch_powerdown_req(Notifier *n, void *opaque)
{
	IPodTouchMachineState *nms = s_kbd_nms;

	if (!nms || !nms->pwroff_timer) {
		return;
	}
	if (nms->pwroff_phase != PWROFF_IDLE) {
		return;   /* a sequence is already running */
	}
	if (getenv("IT_PWROFF_TRACE")) {
		fprintf(stderr, "[PWROFF] holding the hold button\n");
	}
	if (s_kbd_mt) {
		ipod_touch_key_event(s_kbd_mt, KEY_P_DOWN);
	}
	nms->pwroff_phase = PWROFF_PRESSED;
	ipod_touch_powerdown_arm(nms, PWROFF_HOLD_MS);
}

static Notifier ipod_touch_powerdown_notifier = {
	.notify = ipod_touch_powerdown_req,
};

static void ipod_touch_machine_init(MachineState *machine)
{
	IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(machine);
	MemoryRegion *sysmem;
    AddressSpace *nsas;
    ARMCPU *cpu;

    ipod_touch_cpu_setup(machine, &sysmem, &cpu, &nsas);

    // setup clock
    nms->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(nms->sysclk, 12000000ULL);

    nms->cpu = cpu;
    nms->nsas = nsas;

    // setup VICs
    nms->irq = g_malloc0(sizeof(qemu_irq *) * 2);
    DeviceState *dev = pl192_manual_init("vic0", qdev_get_gpio_in(DEVICE(nms->cpu), ARM_CPU_IRQ), qdev_get_gpio_in(DEVICE(nms->cpu), ARM_CPU_FIQ), NULL);
    PL192State *s = PL192(dev);
    nms->vic0 = s;
    memory_region_add_subregion(sysmem, VIC0_MEM_BASE, &nms->vic0->iomem);
    nms->irq[0] = g_malloc0(sizeof(qemu_irq) * 32);
    for (int i = 0; i < 32; i++) { nms->irq[0][i] = qdev_get_gpio_in(dev, i); }

    dev = pl192_manual_init("vic1", NULL);
    s = PL192(dev);
    nms->vic1 = s;
    memory_region_add_subregion(sysmem, VIC1_MEM_BASE, &nms->vic1->iomem);
    nms->irq[1] = g_malloc0(sizeof(qemu_irq) * 32);
    for (int i = 0; i < 32; i++) { nms->irq[1][i] = qdev_get_gpio_in(dev, i); }

    // // chain VICs together
    nms->vic1->daisy = nms->vic0;

    // init clock 0
    dev = qdev_new("ipodtouch.clock");
    IPodTouchClockState *clock0_state = IPOD_TOUCH_CLOCK(dev);
    nms->clock0 = clock0_state;
    memory_region_add_subregion(sysmem, CLOCK0_MEM_BASE, &clock0_state->iomem);

    // init clock 1
    dev = qdev_new("ipodtouch.clock");
    IPodTouchClockState *clock1_state = IPOD_TOUCH_CLOCK(dev);
    nms->clock1 = clock1_state;
    memory_region_add_subregion(sysmem, CLOCK1_MEM_BASE, &clock1_state->iomem);

    // init the timer
    dev = qdev_new("ipodtouch.timer");
    IPodTouchTimerState *timer_state = IPOD_TOUCH_TIMER(dev);
    nms->timer1 = timer_state;
    memory_region_add_subregion(sysmem, TIMER1_MEM_BASE, &timer_state->iomem);
    SysBusDevice *busdev = SYS_BUS_DEVICE(dev);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TIMER1_IRQ));
    //sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TIMER1_IRQ - 1));
    timer_state->sysclk = nms->sysclk;

    // init sysic
    dev = qdev_new("ipodtouch.sysic");
    IPodTouchSYSICState *sysic_state = IPOD_TOUCH_SYSIC(dev);
    nms->sysic = sysic_state;
    memory_region_add_subregion(sysmem, SYSIC_MEM_BASE, &sysic_state->iomem);
    busdev = SYS_BUS_DEVICE(dev);
    for(int grp = 0; grp < GPIO_NUMINTGROUPS_2; grp++) {
        sysbus_connect_irq(busdev, grp, s5l8900_get_irq(nms, S5L8900_GPIO_IRQS[grp]));
    }
    /* Unrealized devices are never parented into the QOM tree, so their reset
     * handlers never run -- see the MIPI DSI note. */
    sysbus_realize(busdev, &error_fatal);

    // init GPIO
    dev = qdev_new("ipodtouch.gpio");
    IPodTouchGPIOState *gpio_state = IPOD_TOUCH_GPIO(dev);
    nms->gpio_state = gpio_state;
    memory_region_add_subregion(sysmem, GPIO_MEM_BASE, &gpio_state->iomem);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init SDIO
    dev = qdev_new("ipodtouch.sdio");
    IPodTouchSDIOState *sdio_state = IPOD_TOUCH_SDIO(dev);
    nms->sdio_state = sdio_state;
    sdio_state->card_present = nms->wifi;
    memory_region_add_subregion(sysmem, SDIO_MEM_BASE, &sdio_state->iomem);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_SDIO_IRQ));
    if (nms->wifi) {
        /* Bridge the dongle's 802.3 channel to "-netdev ...,id=wifi0". */
        ipod_touch_sdio_setup_net(sdio_state);
    }

    dev = exynos4210_uart_create(UART0_MEM_BASE, 256, 0, serial_hd(0), nms->irq[0][24]);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    dev = exynos4210_uart_create(UART1_MEM_BASE, 256, 1, serial_hd(1), nms->irq[0][25]);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    dev = exynos4210_uart_create(UART2_MEM_BASE, 256, 2, serial_hd(2), nms->irq[0][26]);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    dev = exynos4210_uart_create(UART3_MEM_BASE, 256, 3, serial_hd(3), nms->irq[0][27]);
    if (!dev) {
        hw_error("Failed to create UART0 device!");
    }

    // dev = exynos4210_uart_create(UART4_MEM_BASE, 256, 4, serial_hd(4), nms->irq[0][28]);
    // if (!dev) {
    //     printf("Failed to create uart4 device!\n");
    //     abort();
    // }

    // init spis
    set_spi_base(0);
    dev = sysbus_create_simple("ipodtouch.spi", SPI0_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI0_IRQ));
    IPodTouchSPIState *spi0_state = IPOD_TOUCH_SPI(dev);
    spi0_state->nor->nor_path = nms->nor_path;
    spi0_state->nor->boot_args = nms->boot_args;
    nms->spi0_state = spi0_state;

    set_spi_base(1);
    dev = sysbus_create_simple("ipodtouch.spi", SPI1_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI1_IRQ));
    IPodTouchSPIState *spi1_state = IPOD_TOUCH_SPI(dev);
    nms->spi1_state = spi1_state;

    set_spi_base(2);
    sysbus_create_simple("ipodtouch.spi", SPI2_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI2_IRQ));

    set_spi_base(3);
    sysbus_create_simple("ipodtouch.spi", SPI3_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI3_IRQ));

    set_spi_base(4);
    dev = sysbus_create_simple("ipodtouch.spi", SPI4_MEM_BASE, s5l8900_get_irq(nms, S5L8720_SPI4_IRQ));
    IPodTouchSPIState *spi4_state = IPOD_TOUCH_SPI(dev);
    spi4_state->mt->sysic = sysic_state;
    spi4_state->mt->gpio_state = gpio_state;
    nms->spi4_state = spi4_state;

    // init the chip ID module
    dev = qdev_new("ipodtouch.chipid");
    IPodTouchChipIDState *chipid_state = IPOD_TOUCH_CHIPID(dev);
    nms->chipid_state = chipid_state;
    memory_region_add_subregion(sysmem, CHIPID_MEM_BASE, &chipid_state->iomem);

    // init the TVOut instance
    dev = qdev_new("ipodtouch.tvout");
    IPodTouchTVOutState *tvout_state = IPOD_TOUCH_TVOUT(dev);
    nms->tvout_state = tvout_state;
    memory_region_add_subregion(sysmem, TVOUT_MIXER1_MEM_BASE, &tvout_state->mixer1_iomem);
    memory_region_add_subregion(sysmem, TVOUT_MIXER2_MEM_BASE, &tvout_state->mixer2_iomem);
    memory_region_add_subregion(sysmem, TVOUT_SDO_MEM_BASE, &tvout_state->sdo_iomem);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_TVOUT_SDO_IRQ));

    // init the unknown1 module
    dev = qdev_new("ipodtouch.unknown1");
    IPodTouchUnknown1State *unknown1_state = IPOD_TOUCH_UNKNOWN1(dev);
    memory_region_add_subregion(sysmem, UNKNOWN1_MEM_BASE, &unknown1_state->iomem);

    // init the watchdog timer (models reset so the guest can reboot itself)
    dev = qdev_new("ipodtouch.wdt");
    IPodTouchWDTState *wdt_state = IPOD_TOUCH_WDT(dev);
    memory_region_add_subregion(sysmem, WDT_MEM_BASE, &wdt_state->iomem);

    // back the MPVD register window so the power-state path does not fault
    dev = qdev_new("ipodtouch.mpvd");
    IPodTouchMPVDState *mpvd_state = IPOD_TOUCH_MPVD(dev);
    memory_region_add_subregion(sysmem, MPVD_MEM_BASE, &mpvd_state->iomem);

    // init USB OTG
    dev = ipod_touch_init_usb_otg(s5l8900_get_irq(nms, S5L8720_USB_OTG_IRQ), s5l8720_usb_hwcfg);
    synopsys_usb_state *usb_otg = S5L8900USBOTG(dev);
    nms->usb_otg = usb_otg;
    if (nms->usb_tcp_addr[0]) {
        char *dup = g_strdup(nms->usb_tcp_addr);
        char *colon = strrchr(dup, ':');
        if (colon) {
            *colon = '\0';
            usb_otg->server_port = atoi(colon + 1);
        }
        if (!usb_otg->server_port) {
            usb_otg->server_port = 1235;
        }
        usb_otg->server_host = g_strdup(dup[0] ? dup : "127.0.0.1");
        g_free(dup);
    }
    /*
     * Unlike every other sysbus device here, this one was never realized, so
     * its reset handler never ran and none of the register defaults applied -
     * FIFO sizes and GRSTCTL all read back as zero. AHBIDLE reading clear is
     * what made AppleSynopsysOTG2::_coreInit panic with "AHB not idle".
     */
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, USBOTG_MEM_BASE, &nms->usb_otg->iomem);

    // init two pl080 DMAC0 devices
    dev = qdev_new("pl080");
    PL080State *pl080_1 = PL080(dev);
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    memory_region_add_subregion(sysmem, DMAC0_MEM_BASE, &pl080_1->iomem1);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    /*
     * DMAC0 completion IRQ. The 3.1.3 (7E18) kernel's AppleARMPL080DMAC enables
     * VIC line 17 for DMAC0 and blocks the NAND storage stack on it (root would
     * never mount otherwise -- verified: with line 16 the kernel waits forever
     * on "IOMedia Partition 1"; with 17 it mounts disk0s1 and jettisons the boot
     * kexts). 2.1.1 uses DMAC0 in PIO and never enables either line, so keep its
     * historical wiring (16) and only shift under IT_DIRECT_IBOOT.
     */
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms,
                       getenv("IT_DIRECT_IBOOT") ? 0x11 : S5L8720_DMAC0_IRQ));

    dev = qdev_new("pl080");
    PL080State *pl080_2 = PL080(dev);
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    memory_region_add_subregion(sysmem, DMAC1_0_MEM_BASE, &pl080_2->iomem1);
    memory_region_add_subregion(sysmem, DMAC1_1_MEM_BASE, &pl080_2->iomem2);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_DMAC1_IRQ));

    // Init I2C0
    dev = qdev_new("ipodtouch.i2c");
    IPodTouchI2CState *i2c_state = IPOD_TOUCH_I2C(dev);
    i2c_state->base = 0;
    nms->i2c0_state = i2c_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, I2C0_MEM_BASE, &i2c_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_I2C0_IRQ));

    // init the PMU
    I2CSlave * pmu = i2c_slave_create_simple(i2c_state->bus, "pcf50633", 0x73);
    spi4_state->mt->pmu = PCF50633(pmu);
    PCF50633(pmu)->usb_cable = nms->usb_attached;
    ipod_touch_mbx_set_patch_usb_gate(nms->usb_patch_mux_gate);

    // init the accelerometer. Keep the handle so the machine's QMP properties
    // (accel-orientation / accel-x/y/z / accel-shake, added in instance_init)
    // can drive it, e.g.  qom-set path=/machine property=accel-orientation value=3
    I2CSlave *accelerometer = i2c_slave_create_simple(i2c_state->bus, "lis302dl", 0x1D);
    nms->lis302dl_state = LIS302DL(accelerometer);

    // init the audio codec (disabled because unused)
    // I2CSlave *audio_codec = i2c_slave_create_simple(i2c_state->bus, "cs42l58", 0x4A);

    // Init I2C1
    dev = qdev_new("ipodtouch.i2c");
    i2c_state = IPOD_TOUCH_I2C(dev);
    nms->i2c1_state = i2c_state;
    i2c_state->base = 1;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, I2C1_MEM_BASE, &i2c_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_I2C1_IRQ));
    
    // Init the light sensor
    I2CSlave *isl29003dl = i2c_slave_create_simple(i2c_state->bus, "isl29003dl", 0x44);

    // init the Mikey
    I2CSlave *cd327mikey = i2c_slave_create_simple(i2c_state->bus, "cd3272mikey", 0x39);

    // init the FMSS flash controller
    dev = qdev_new("ipodtouch.fmss");
    IPodTouchFMSSState *fmss_state = IPOD_TOUCH_FMSS(dev);
    fmss_state->nand_path = nms->nand_path;
    fmss_state->nand_overlay = nms->nand_overlay[0] ? nms->nand_overlay : NULL;
    nms->fmss_state = fmss_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, FMSS_MEM_BASE, &fmss_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_FMSS_IRQ));

    // init the USB module
    dev = qdev_new("ipodtouch.usbphys");
    IPodTouchUSBPhysState *usb_phys_state = IPOD_TOUCH_USB_PHYS(dev);
    nms->usb_phys_state = usb_phys_state;
    memory_region_add_subregion(sysmem, USBPHYS_MEM_BASE, &usb_phys_state->iomem);

    ipod_touch_memory_setup(machine, sysmem, nsas);

    // init the MIPI SDI controller
    dev = qdev_new("ipodtouch.mipidsi");
    IPodTouchMIPIDSIState *mipi_dsi_state = IPOD_TOUCH_MIPI_DSI(dev);
    nms->mipi_dsi_state = mipi_dsi_state;
    memory_region_add_subregion(sysmem, MIPI_DSI_MEM_BASE, &mipi_dsi_state->iomem);
    /* Has to be realized, not just created: an unrealized device is never
     * parented into the QOM tree, so qemu_devices_reset() never reaches it and
     * its DeviceClass reset handler is dead code. Without this the DSI link
     * kept the previous boot's panel-ID latch across a warm reset and the panel
     * was never brought up on the second boot. */
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    // init LCD
    dev = qdev_new("ipodtouch.lcd");
    IPodTouchLCDState *lcd_state = IPOD_TOUCH_LCD(dev);
    lcd_state->sysmem = sysmem;
    lcd_state->mt = spi4_state->mt;
    nms->lcd_state = lcd_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, DISPLAY_MEM_BASE, &lcd_state->iomem);
    sysbus_realize(busdev, &error_fatal);
    sysbus_connect_irq(busdev, 0, s5l8900_get_irq(nms, S5L8720_LCD_IRQ));

    // init scaler / CSC
    dev = qdev_new("ipodtouch.scalercsc");
    IPodTouchScalerCSCState *scaler_csc_state = IPOD_TOUCH_SCALER_CSC(dev);
    nms->scaler_csc_state = scaler_csc_state;
    busdev = SYS_BUS_DEVICE(dev);
    memory_region_add_subregion(sysmem, SCALER_CSC_MEM_BASE, &scaler_csc_state->iomem);
    sysbus_realize(busdev, &error_fatal);

    // init SHA1 engine
    dev = qdev_new("ipodtouch.sha1");
    IPodTouchSHA1State *sha1_state = IPOD_TOUCH_SHA1(dev);
    nms->sha1_state = sha1_state;
    memory_region_add_subregion(sysmem, SHA1_MEM_BASE, &sha1_state->iomem);

    // init AES engine
    dev = qdev_new("ipodtouch.aes");
    IPodTouchAESState *aes_state = IPOD_TOUCH_AES(dev);
    nms->aes_state = aes_state;
    memory_region_add_subregion(sysmem, AES_MEM_BASE, &aes_state->iomem);

    // init PKE engine
    dev = qdev_new("ipodtouch.pke");
    IPodTouchPKEState *pke_state = IPOD_TOUCH_PKE(dev);
    nms->pke_state = pke_state;
    memory_region_add_subregion(sysmem, PKE_MEM_BASE, &pke_state->iomem);

    // init the MBX
    dev = qdev_new("ipodtouch.mbx");
    IPodTouchMBXState *mbx_state = IPOD_TOUCH_MBX(dev);
    nms->mbx_state = mbx_state;
    mbx_state->irq_enabled = nms->mbx_irq;
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, s5l8900_get_irq(nms, S5L8720_MBX_IRQ));
    memory_region_add_subregion(sysmem, MBX1_MEM_BASE, &mbx_state->iomem1);
    memory_region_add_subregion(sysmem, MBX2_MEM_BASE, &mbx_state->iomem2);

    qemu_register_reset(ipod_touch_cpu_reset, nms);

    /*
     * Route the host keyboard through the modern input handler so we can see
     * the Command modifier (a 0xE0-prefixed extended scancode the legacy path
     * mangles) and tell button combos apart from text. See ipod_touch_kbd_event.
     */
    s_kbd_nms = nms;
    s_kbd_mt = spi4_state->mt;
    qemu_input_handler_register(DEVICE(nms->cpu), &ipod_touch_kbd_handler);

    /*
     * system_powerdown -> the slide-to-power-off gesture (see
     * ipod_touch_powerdown_tick). This is what gives the guest a chance to
     * unmount its filesystems before the machine stops.
     */
    nms->pwroff_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     ipod_touch_powerdown_tick, nms);
    nms->pwroff_phase = PWROFF_IDLE;
    qemu_register_powerdown_notifier(&ipod_touch_powerdown_notifier);
}

static void ipod_touch_machine_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);
    mc->desc = "iPod Touch";
    mc->init = ipod_touch_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
}

static const TypeInfo ipod_touch_machine_info = {
    .name          = TYPE_IPOD_TOUCH_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(IPodTouchMachineState),
    .class_size    = sizeof(IPodTouchMachineClass),
    .class_init    = ipod_touch_machine_class_init,
    .instance_init = ipod_touch_instance_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_machine_info);
}

type_init(ipod_touch_machine_types)
