/*
 * The S5L8900 machines: iPod touch 1G (N45AP) -- `-M iPod-Touch-1G` -- and the
 * original iPhone (M68AP) -- `-M iPhone-2G`. One machine; the boards differ in
 * the data of an S5L8900Board (n45_board, m68_board below).
 *
 * devos50's original qemu-ios target (branch ipod_touch_1g, 501c85c4f8),
 * brought onto this tree. The boot chain is his: the public S5L8900 bootrom
 * is staged at the VROM base only to serve iBoot's jump table, two of whose
 * slots (8900 image verify/decrypt) are pointed at stubs in the LLB window,
 * and the CPU is reset straight into a decrypted iBoot-204 at 0x18000000. The
 * NOR (IMG2 images + boot-args) is a parallel CFI flash on -drive if=pflash;
 * the NAND is a directory of bank<N>/<page>.page files behind the FMC/ADM.
 *
 *   qemu-system-arm -M iPod-Touch-1G,bootrom=...,iboot=...,nand=...,nand-overlay=... \
 *       -drive if=pflash,format=raw,file=nor.bin -serial mon:stdio \
 *       -display none -audio driver=none
 *
 * Shared models (ipodtouch.*) carry the S5L8900 differences as properties:
 * timer irqlatch=0xF8, clock s5l8900=on, lcd s5l8900=on (register layout),
 * chipid word1/word2, spi index/peripheral. S5L8900-only blocks are the
 * s5l8900_* models (FMC, NAND ECC, ADM, LCD panel) and the two small stubs
 * kept here (8900 engine hook, TV-out workaround); the MBX is the 2G's ipodtouch.mbx.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/arm/boot.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/block/flash.h"
#include "hw/dma/pl080.h"
#include "hw/arm/exynos4210.h"
#include "hw/arm/ipod_touch_1g.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "hw/arm/ipod_touch_usb_phys.h"
#include "hw/arm/ipod_touch_tvout.h"
#include "hw/arm/ipod_touch_chipid.h"
#include "hw/arm/ipod_touch_sdio.h"
#include "hw/arm/mrvl8686.h"
#include "net/net.h"
#include "qemu/config-file.h"
#include "qemu/option.h"
#include "hw/arm/ipod_touch_mbx.h"
#include "hw/arm/ipod_touch_i2s.h"
#include "hw/arm/ipod_touch_piezo.h"
#include "target/arm/cpregs.h"
#include "hw/arm/guest-services/general.h"
#include "hw/arm/guest-services/gles.h"
#include "hw/arm/ipod_touch_aes.h"
#include "hw/arm/ipod_touch_sha1.h"
#include "hw/arm/ipod_touch_buttons.h"
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "hw/arm/it_iboot.h"
#include "hw/arm/s5l8900_nand_ecc.h"
#include "hw/arm/s5l8900_lcd_panel.h"
#include "hw/arm/s5l8900_multitouch_z1.h"
#include "hw/misc/ios_baseband.h"
#include "hw/i2c/ipod_touch_i2c.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "crypto/cipher.h"
#include "ui/input.h"

static const int n45_gpio_irqs[GPIO_NUMINTGROUPS] = {
    N45_GPIO_G0_IRQ, N45_GPIO_G1_IRQ, N45_GPIO_G2_IRQ, N45_GPIO_G3_IRQ,
    N45_GPIO_G4_IRQ, N45_GPIO_G5_IRQ, N45_GPIO_G6_IRQ,
};

static const uint32_t s5l8900_usb_hwcfg[] = { 0, 0x7a8f60d0, 0x082000e8, 0x01f08024 };

/* n45ap 3A101a's device tree. */
static const S5L8900Board n45_board = {
    .name = "n45",
    .pmu_i2c = 1,
    /* i2s1: dma-parent dmac1, TX config 0x884 (peripheral 2), interrupts <0xaa> */
    .codec_i2s = { N45_IIS1_BASE, 1, N45_I2S1_DMA_REQ_ID, 0xaa, .host_output = false },
    .i2s_ram_bases = { N45_IIS0_BASE, N45_IIS2_BASE },
    .piezo = true,
    .touch = "multitouch", .touch_atn_irq = 0x9b, .touch_cs_gpio = -1,
    .home_gpio = N45_GPIO_BUTTON_HOME, .home_irq = N45_GPIO_BUTTON_HOME_IRQ,
    .power_gpio = N45_GPIO_BUTTON_POWER, .power_irq = N45_GPIO_BUTTON_POWER_IRQ,
    .nand_banks = 8,
    .pwroff_hold_ms = 6000, .pwroff_settle_ms = 1500,
};

/*
 * m68ap 1A543a's device tree: PMU and codec on i2c0; the codec's samples on
 * i2s0 (0x3ca00000, dmac0, TX config 0x800: peripheral 0, interrupts <0x86>),
 * i2s1 the baseband's voice port; no buzzer (the codec plays the clicks); the Zephyr1
 * (multi-touch,z1, interrupts <0xa3>, spi_cs0 GPIO 0x0705); buttons,m68; the
 * flash disk's reg 0x0f, four chip enables.
 */
static const S5L8900Board m68_board = {
    .name = "m68",
    .pmu_i2c = 0,
    .codec_i2s = { N45_IIS2_BASE, 0, 0, 0x86, .host_output = true },
    /*
     * i2s1 (audio-data,baseband): dmac1, TX config 0x884 (peripheral 2), interrupts <0xaa>, the N45 codec's
     * wiring. The speaker's path: 1.0's mediaserverd routes every system sound (clicks, lock, ringer) here.
     */
    .bb_i2s = { N45_IIS1_BASE, 1, N45_I2S1_DMA_REQ_ID, 0xaa, .host_output = true },
    .i2s_ram_bases = { N45_IIS0_BASE },
    .piezo = false,
    .touch = TYPE_S5L8900_MULTITOUCH_Z1, .touch_atn_irq = 0xa3, .touch_cs_gpio = 0x0705,
    .home_gpio = M68_GPIO_BUTTON_HOME, .home_irq = M68_GPIO_BUTTON_HOME_IRQ,
    .power_gpio = N45_GPIO_BUTTON_POWER, .power_irq = N45_GPIO_BUTTON_POWER_IRQ,
    .volup_gpio = M68_GPIO_BUTTON_VOLUP, .volup_irq = M68_GPIO_BUTTON_VOLUP_IRQ,
    .voldown_gpio = M68_GPIO_BUTTON_VOLDOWN, .voldown_irq = M68_GPIO_BUTTON_VOLDOWN_IRQ,
    .ring_gpio = M68_GPIO_RING_SWITCH, .ring_irq = M68_GPIO_RING_SWITCH_IRQ,
    .nand_banks = 4,
    /* DT accelerometer orientation 0 (none), where the N45's is 3: AppleLIS302DL negates x for bit 0 and y
     * for bit 1, so the N45's part sits turned 180 degrees about Z to the M68's. The model reads as the
     * N45's part; on the M68 each axis reads the negated device x and y. */
    .accel_mount = "-1,-2,3",
    /* 1.0 builds its "slide to power off" sheet slowly (up 13 s into a hold,
     * guest time, where 1.1's is up in 3-4 s); a release before it locks instead. */
    .pwroff_hold_ms = 20000, .pwroff_settle_ms = 3000,
};

static inline qemu_irq n45_irq(IPodTouch1GMachineState *s, int n)
{
    return s->irq[n / 32][n % 32];
}

static MemoryRegion *allocate_ram(MemoryRegion *top, const char *name,
                                  hwaddr addr, uint64_t size)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion(top, addr, sec);
    return sec;
}

/* ---- 8900 engine hook -------------------------------------------------- */

/*
 * iBoot-204 hands 8900-wrapped images (the kernelcache, from NOR) to two
 * bootrom routines through the ROM's jump table: verify, then decrypt in
 * place. The public bootrom dump has that code missing, so both slots are
 * redirected to stubs in the LLB window: verify returns 1; decrypt stores the
 * image address into this MMIO word, and the write does the AES-128-CBC with
 * the public key 0x837 that the real routine does. High-level emulation of
 * those two ROM routines (fidelity class H).
 */
typedef struct QEMU_PACKED {
    uint8_t magic[4];       /* "8900" */
    uint8_t version[3];     /* "1.0" */
    uint8_t format;         /* 0x03 encrypted, 0x04 plain */
    uint8_t unknown[4];
    uint32_t size_of_data;
    uint32_t footer_sig_off;
    uint32_t footer_cert_off;
    uint32_t footer_cert_len;
    uint8_t key1[32];
    uint8_t unknown_version[4];
    uint8_t key2[16];
    uint8_t padding[1968];
} Header8900;

static const uint8_t key_0x837[16] = {
    0x18, 0x84, 0x58, 0xA6, 0xD1, 0x50, 0x34, 0xDF,
    0xE3, 0x86, 0xF2, 0x3B, 0x61, 0xD4, 0x37, 0x74,
};

static uint64_t engine_8900_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void engine_8900_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouch1GMachineState *s = opaque;
    Header8900 hdr;
    Error *err = NULL;

    if (offset != 0) {
        return;
    }
    QEMU_BUILD_BUG_ON(sizeof(Header8900) != 0x800);
    if (address_space_read(s->nsas, value, MEMTXATTRS_UNSPECIFIED, &hdr,
                           sizeof(hdr)) != MEMTX_OK ||
        memcmp(hdr.magic, "8900", 4) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "[8900] no 8900 header at 0x%08" PRIx64 "\n", value);
        return;
    }
    uint32_t len = le32_to_cpu(hdr.size_of_data) & ~15u;
    if (hdr.format != 0x03) {
        return;   /* plain image: nothing to decrypt */
    }
    if (len > 64 * MiB) {
        qemu_log_mask(LOG_GUEST_ERROR, "[8900] implausible payload size %u\n", len);
        return;
    }
    g_autofree uint8_t *buf = g_malloc(len);
    if (address_space_read(s->nsas, value + sizeof(hdr), MEMTXATTRS_UNSPECIFIED,
                           buf, len) != MEMTX_OK) {
        return;
    }
    QCryptoCipher *cipher = qcrypto_cipher_new(QCRYPTO_CIPHER_ALGO_AES_128,
                                               QCRYPTO_CIPHER_MODE_CBC,
                                               key_0x837, sizeof(key_0x837), &err);
    uint8_t iv[16] = { 0 };
    if (!cipher || qcrypto_cipher_setiv(cipher, iv, sizeof(iv), &err) < 0 ||
        qcrypto_cipher_decrypt(cipher, buf, buf, len, &err) < 0) {
        error_report_err(err);
        qcrypto_cipher_free(cipher);
        return;
    }
    qcrypto_cipher_free(cipher);
    address_space_write(s->nsas, value + sizeof(hdr), MEMTXATTRS_UNSPECIFIED, buf, len);
    qemu_log_mask(LOG_UNIMP, "[8900] decrypted %u bytes at 0x%08" PRIx64 "\n", len, value);
}

static const MemoryRegionOps engine_8900_ops = {
    .read = engine_8900_read,
    .write = engine_8900_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* ---- TV-out workaround ------------------------------------------------- */

/*
 * devos50's per-build hack: a 4-byte MMIO window laid over a kernel data word
 * (0x08a25960 in 3A101a) that always reads 0, "so that TVOut can be correctly
 * deallocated without reverse engineering the entire TVOut protocol". Off by
 * default; tvout-workaround=<paddr> turns it on for a build that needs it.
 * Debt: a faithful AppleTVOut model makes it unnecessary.
 */
static uint64_t zero_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void drop_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
}

static const MemoryRegionOps zero_word_ops = {
    .read = zero_read,
    .write = drop_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* ---- USB wrangler quirk ------------------------------------------------ */

/*
 * iPhone OS 1.1's AppleS5L8900XUSBWrangler::start registers for the PHY's
 * publication with IOService::addNotification and stores the returned
 * notifier at this+0x6c only afterwards; addNotification invokes the handler
 * synchronously for a PHY that is already published, and the handler
 * (phyRegistered) calls notifier->remove() through the still-NULL field.
 *
 * Nothing in the hardware decides that order. Between the wrangler's start
 * and the PHY's registerService neither driver reads an OTG or PHY register
 * it waits on: the PHY's start is IODelay(200 us) plus clock/power-gate
 * calls into the power controller, the wrangler's only wait is its interrupt
 * source (the VIC), which waits for AppleARMCPU's initCPU, which waits for
 * the power controller's "function-cpu_idle", which waits for the clock
 * controller's matched state. The PHY needs one of those links, the
 * wrangler four; the IOKit config-thread pool runs the PHY's job first. The
 * order is the same at every guest CPU rate tried (-icount shift 1, 3, 7)
 * and with io=0, so there is no latency to model (smoke #1).
 *
 * Hiding the PHY (the previous quirk) left the wrangler without one, and its
 * setPowerState(0) dereferences the PHY: the sleep and shutdown paths
 * panicked. Instead, phyRegistered skips remove() when the notifier is not
 * stored yet, so the PHY is taken exactly as the asynchronous path would:
 *
 *   ldr r0,[r5,#0x6c]      ldr   r0,[r5,#0x6c]
 *   ldr r3,[r0]       ->   cmp   r0,#0
 *   mov lr,pc              ldrne r3,[r0]
 *   ldr pc,[r3,#N]         blne  <an existing "ldr pc,[r3,#N]">
 *   str r4,[r5,#0x68]      str   r4,[r5,#0x68]
 *
 * N is remove()'s vtable slot: 0x54 in 1.1, 0x94 in 1.0 (1A543a, the iPhone).
 *
 * Found by its instruction words in RAM once iBoot has loaded the
 * kernelcache (the first timer-4 configuration), so it only ever touches a
 * build that has this exact sequence. Guest patch (fidelity class P);
 * usb-wrangler-quirk=off runs the stock code.
 */
static void n45_usb_wrangler_quirk(void *opaque)
{
    IPodTouch1GMachineState *s = opaque;
    static const uint32_t sig[5] = { 0xe595006c, 0xe5903000, 0xe1a0e00f, 0xe593f000, 0xe5854068 };
    g_autofree uint32_t *ram = g_malloc(N45_RAM_SIZE);

    if (!s->usb_wrangler_quirk || s->usb_wrangler_quirk_done) {
        return;
    }
    if (address_space_read(s->nsas, N45_RAM_BASE, MEMTXATTRS_UNSPECIFIED, ram,
                           N45_RAM_SIZE) != MEMTX_OK) {
        return;
    }
    for (size_t i = 0; i + 5 <= N45_RAM_SIZE / 4; i++) {
        if (ram[i] != sig[0] || ram[i + 1] != sig[1] || ram[i + 2] != sig[2] ||
            (ram[i + 3] & 0xfffff000) != sig[3] || ram[i + 4] != sig[4]) {
            continue;
        }
        const uint32_t vcall = ram[i + 3];   /* ldr pc,[r3,#N] */
        size_t t = i;
        while (t > 0 && i - t < 0x1000 && ram[--t] != vcall) {
        }
        if (ram[t] != vcall) {
            break;
        }
        hwaddr at = N45_RAM_BASE + 4 * i;
        int32_t off = ((int32_t)(4 * t) - (int32_t)(4 * (i + 3) + 8)) >> 2;
        uint32_t patch[3] = { 0xe3500000, 0x15903000, 0x1b000000 | (off & 0xffffff) };
        address_space_write(s->nsas, at + 4, MEMTXATTRS_UNSPECIFIED, patch, sizeof(patch));
        s->usb_wrangler_quirk_done = true;   /* until then iBoot is still loading the kernel */
        qemu_log_mask(LOG_UNIMP, "[n45] USB wrangler phyRegistered guarded at 0x%08" HWADDR_PRIx
                      " (usb-wrangler-quirk)\n", at);
        return;
    }
}

/* ---- boot images ------------------------------------------------------- */

static size_t n45_stage(IPodTouch1GMachineState *s, const char *what, const char *path,
                        hwaddr base, size_t capacity)
{
    g_autofree char *data = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize size;

    if (!path || !path[0]) {
        error_report("iPod-Touch-1G: the %s= machine option is required", what);
        exit(1);
    }
    if (!g_file_get_contents(path, &data, &size, &gerr)) {
        error_report("iPod-Touch-1G: cannot read %s '%s': %s", what, path, gerr->message);
        exit(1);
    }
    if (!size || size > capacity) {
        error_report("iPod-Touch-1G: %s '%s' must be 1..%zu bytes (got %zu)",
                     what, path, capacity, size);
        exit(1);
    }
    if (address_space_write(s->nsas, base, MEMTXATTRS_UNSPECIFIED, data, size) != MEMTX_OK) {
        error_report("iPod-Touch-1G: cannot stage %s at 0x%" HWADDR_PRIx, what, base);
        exit(1);
    }
    return size;
}

static void n45_write32(IPodTouch1GMachineState *s, hwaddr addr, uint32_t v)
{
    uint32_t le = cpu_to_le32(v);
    address_space_write(s->nsas, addr, MEMTXATTRS_UNSPECIFIED, &le, 4);
}

/*
 * Whether this iBoot has the check at all: iBoot-159 (1.0) predates the
 * epoch, so finding none in it is no warning. Its panic string is the test.
 */
static bool n45_iboot_checks_epoch(IPodTouch1GMachineState *s, size_t size)
{
    static const char needle[] = "Epoch Mismatch";
    g_autofree uint8_t *image = g_malloc(size);

    return address_space_read(s->nsas, N45_IBOOT_BASE, MEMTXATTRS_UNSPECIFIED, image, size) != MEMTX_OK ||
           memmem(image, size, needle, sizeof(needle) - 1) != NULL;
}

/* Re-staged on every reset, so a warm reboot enters the same iBoot. */
static void n45_stage_boot_chain(IPodTouch1GMachineState *s)
{
    n45_stage(s, "bootrom", s->bootrom_path, N45_VROM_BASE, N45_VROM_SIZE);
    size_t iboot = n45_stage(s, "iboot", s->iboot_path, N45_IBOOT_BASE, N45_IBOOT_SIZE);

    /*
     * miu_init panics "Epoch Mismatch" unless POWER_ID[31:24] is the epoch compiled into this iBoot
     * (2 for 1.1-1.1.2, 3 for 1.1.3-1.1.5), which the LLB we skip would have latched: read it off the
     * staged image, as the 2G's direct-iboot does (it_iboot.c).
     */
    s->sysic->epoch = it_iboot_epoch(s->nsas, N45_IBOOT_BASE, iboot);
    if (!s->sysic->epoch && n45_iboot_checks_epoch(s, iboot)) {
        warn_report_once("%s: no security epoch found in iBoot '%s'; "
                         "iBoot will panic \"Epoch Mismatch\"",
                         object_get_typename(qdev_get_machine()), s->iboot_path);
    }

    /* Point the ROM's two 8900 jump-table slots at the LLB-window stubs. */
    n45_write32(s, N45_VROM_JT_8900_VERIFY, N45_LLB_BASE + 0x80);
    n45_write32(s, N45_VROM_JT_8900_DECRYPT, N45_LLB_BASE + 0x100);

    /* +0x80: verify -> MOVS R0,#1; BX LR */
    n45_write32(s, N45_LLB_BASE + 0x80, 0xe3b00001);
    n45_write32(s, N45_LLB_BASE + 0x84, 0xe12fff1e);
    /* +0x100: decrypt -> LDR r1,[pc,#0x100]; STR r0,[r1]; MOVS R0,#1; BX LR */
    n45_write32(s, N45_LLB_BASE + 0x100, 0xe59f1100);
    n45_write32(s, N45_LLB_BASE + 0x104, 0xe5810000);
    n45_write32(s, N45_LLB_BASE + 0x108, 0xe3b00001);
    n45_write32(s, N45_LLB_BASE + 0x10c, 0xe12fff1e);
    /* the literal that LDR reads: the 8900 engine's MMIO word */
    n45_write32(s, N45_LLB_BASE + 0x208, N45_ENGINE_8900_BASE);
}

/*
 * Guest services on the QEMU_CALL register (mcr p15, 3, rX, c15, c15, 0), as the 2G and iPad 1 have
 * it: the GL bridge the 1.x OpenGLES front end (contrib/it-gles/gles2x.c, OpenGLES-1x) calls, and
 * guest-package delivery. 1.x has no agent, keyboard or pasteboard service yet.
 */
static void n45_qemu_call(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    CPUState *cs = env_cpu(env);
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(qdev_get_machine());
    qemu_call_t q;
    int32_t err = 0;

    if (cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 0)) {
        return;
    }
    switch (q.call_number) {
    case QC_GLES:
        q.retval = qc_handle_gles(cs, &q.args.gles);
        break;
    case QC_GLES_PING:
        q.retval = QC_GLES_PING_MAGIC;
        break;
    default:
        if (!guest_pkg_call(&s->pkg, cs, &q, &err)) {
            q.retval = -1;
            err = QC_ERR_ENOSYS;
        }
    }
    q.error = err;
    cpu_memory_rw_debug(cs, value, (uint8_t *)&q, sizeof(q), 1);
}

static const ARMCPRegInfo n45_cp_reginfo[] = {
    { .name = "QEMU_CALL", .cp = 15, .opc1 = 3, .crn = 15, .crm = 15,
      .opc2 = 0, .access = PL0_RW, .state = ARM_CP_STATE_AA32,
      .type = ARM_CP_IO | ARM_CP_NO_RAW | ARM_CP_RAISES_EXC, /* gles_guest_rw */
      .readfn = qemu_call_status,
      .writefn = n45_qemu_call },
};

static void n45_cpu_reset(void *opaque)
{
    IPodTouch1GMachineState *s = opaque;

    gles_host_set_debug(s->gles_debug);
    gles_host_reset();
    cpu_reset(CPU(s->cpu));
    /* (The volume pads rest high by the GPIO block's own reset, rest-high-*: this
     * handler runs before the device resets, so pad levels set here were lost and
     * both volume buttons read held from boot.) */
    n45_stage_boot_chain(s);
    cpu_set_pc(CPU(s->cpu), N45_IBOOT_BASE);
}

/* ---- buttons ----------------------------------------------------------- */

/* Same chords as the 2G (imgtools/itqmp.py BUTTONS): Cmd+Shift+H home, Cmd+L power, Cmd+-/= volume. */
/*
 * A press shorter than the guest reacts to is no press: AppleM68Buttons reads the pads
 * from its work loop after the interrupt, and a host chord (HMP sendkey, ~100 ms of
 * host time, a few ms of guest time) was released by then, so both reads saw the
 * button up and Home never registered. A release earlier than this much guest time
 * after the press waits for it.
 */
#define BUTTON_MIN_PRESS_NS (150 * SCALE_MS)

static void n45_button(IPodTouch1GMachineState *s, uint32_t gpio, uint32_t gpio_irq, bool down);

static void n45_button_release_due(void *opaque)
{
    IPodTouch1GMachineState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int i = 0; i < ARRAY_SIZE(s->btn_gpio); i++) {
        if (s->btn_release[i] && now - s->btn_pressed_ns[i] >= BUTTON_MIN_PRESS_NS) {
            s->btn_release[i] = false;
            n45_button(s, s->btn_gpio[i], s->btn_irq[i], false);
        }
    }
}

static void n45_button(IPodTouch1GMachineState *s, uint32_t gpio, uint32_t gpio_irq, bool down)
{
    uint32_t *pads = s->gpio->gpio_state;
    int slot = -1;

    for (int i = 0; i < ARRAY_SIZE(s->btn_gpio); i++) {
        if (s->btn_gpio[i] == gpio || (slot < 0 && !s->btn_gpio[i])) {
            slot = i;
        }
    }
    if (gpio != s->board->ring_gpio && slot >= 0) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->btn_gpio[slot] = gpio;
        s->btn_irq[slot] = gpio_irq;
        if (down) {
            s->btn_pressed_ns[slot] = now;
            s->btn_release[slot] = false;
        } else if (now - s->btn_pressed_ns[slot] < BUTTON_MIN_PRESS_NS) {
            s->btn_release[slot] = true;
            timer_mod(s->btn_timer, s->btn_pressed_ns[slot] + BUTTON_MIN_PRESS_NS);
            return;
        }
    }
    bool active_low = gpio == s->board->volup_gpio || gpio == s->board->voldown_gpio;
    bool was = gpio_is_on(pads, gpio) != active_low;

    if (down == was) {
        return;
    }
    if (gpio == s->board->power_gpio && s->pmu) {
        pcf50633_set_exton1(s->pmu, down);
    }
    if (down != active_low) {
        gpio_set_on(pads, gpio);
    } else {
        gpio_set_off(pads, gpio);
    }
    /* The GPIO IC compares the pad with the polarity the driver programmed
     * (1.x flips it after each interrupt to catch press and release). */
    ipod_touch_sysic_set_pad(s->sysic, gpio_irq, gpio_is_on(pads, gpio));
}

/* The app bridge's buttons (qemu_ios_ui_button), on the chords' pads; the 1G has no volume buttons. */
void ipod_touch_1g_press_button(IPodTouchButton button, bool down)
{
    IPodTouch1GMachineState *s = (IPodTouch1GMachineState *)
        object_dynamic_cast(OBJECT(qdev_get_machine()), TYPE_IPOD_TOUCH_1G_MACHINE);
    const S5L8900Board *b = s ? s->board : NULL;

    if (!b) {
        return;
    }
    switch (button) {
    case IPOD_TOUCH_BUTTON_HOME:
        n45_button(s, b->home_gpio, b->home_irq, down);
        break;
    case IPOD_TOUCH_BUTTON_POWER:
        n45_button(s, b->power_gpio, b->power_irq, down);
        break;
    case IPOD_TOUCH_BUTTON_VOLUP:
        if (b->volup_gpio) {
            n45_button(s, b->volup_gpio, b->volup_irq, down);
        }
        break;
    case IPOD_TOUCH_BUTTON_VOLDOWN:
        if (b->voldown_gpio) {
            n45_button(s, b->voldown_gpio, b->voldown_irq, down);
        }
        break;
    }
}

/*
 * QMP system_powerdown -> the user's power-off gesture, as on the 2G and the iPad: the one clean
 * shutdown path (volumes unmounted, the FTL closed) ends in the PMU's power-off write, where QEMU
 * exits. Home first (quits a foreground app), hold Hold until SpringBoard raises "slide to power
 * off", then drag its knob along the track. QEMU_CLOCK_VIRTUAL throughout: SpringBoard's hold
 * threshold is guest time. The knob row is 1.1's sheet (IT_PWROFF_KNOB_Y overrides, as on the 2G).
 */
enum { PWROFF_IDLE, PWROFF_HOME, PWROFF_WAKE, PWROFF_HOLD, PWROFF_SETTLE, PWROFF_DRAG };
#define PWROFF_DRAG_STEPS 24
#define PWROFF_KNOB_X     65
#define PWROFF_KNOB_Y     68
#define PWROFF_TRACK_END  295

static void n45_pwroff_arm(IPodTouch1GMachineState *s, int ms)
{
    timer_mod(s->pwroff_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)ms * SCALE_MS);
}

static void n45_pwroff_touch(IPodTouch1GMachineState *s, int px, bool down)
{
    const char *e = getenv("IT_PWROFF_KNOB_Y");
    int py = e ? atoi(e) : PWROFF_KNOB_Y;

    ipod_touch_multitouch_set_finger(s->mt, 0, px / 320.0f, 1.0f - py / 480.0f, down);
}

static void n45_pwroff_tick(void *opaque)
{
    IPodTouch1GMachineState *s = opaque;

    switch (s->pwroff_phase) {
    case PWROFF_HOME:
        n45_button(s, s->board->home_gpio, s->board->home_irq, false);
        s->pwroff_phase = PWROFF_WAKE;
        n45_pwroff_arm(s, 2500);            /* the app quits, SpringBoard is in front */
        break;
    case PWROFF_WAKE:
        n45_button(s, s->board->power_gpio, s->board->power_irq, true);
        s->pwroff_phase = PWROFF_HOLD;
        /* Held until the sheet is up: 1.1 shows it 3-4 s into a hold, later on a boot's first
         * hold (the sheet is built on first use); a 3.5 s hold released before it and locked
         * the device instead (the matrix's second boot, smoke #21). Holding longer is harmless. */
        n45_pwroff_arm(s, s->board->pwroff_hold_ms);
        break;
    case PWROFF_HOLD:
        n45_button(s, s->board->power_gpio, s->board->power_irq, false);
        s->pwroff_phase = PWROFF_SETTLE;
        n45_pwroff_arm(s, s->board->pwroff_settle_ms);   /* the sheet slides in */
        break;
    case PWROFF_SETTLE:
        n45_pwroff_touch(s, PWROFF_KNOB_X, true);
        s->pwroff_phase = PWROFF_DRAG;
        s->pwroff_step = 0;
        n45_pwroff_arm(s, 80);
        break;
    case PWROFF_DRAG: {
        int i = ++s->pwroff_step;
        n45_pwroff_touch(s, PWROFF_KNOB_X + (PWROFF_TRACK_END - PWROFF_KNOB_X) * i / PWROFF_DRAG_STEPS,
                         i < PWROFF_DRAG_STEPS);
        if (i < PWROFF_DRAG_STEPS) {
            n45_pwroff_arm(s, 80);
        } else {
            s->pwroff_phase = PWROFF_IDLE;  /* the guest halts now; a missed slide can be requested again */
        }
        break;
    }
    }
}

static void n45_powerdown_req(Notifier *n, void *opaque)
{
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(qdev_get_machine());

    if (s->pwroff_phase != PWROFF_IDLE) {
        return;
    }
    n45_button(s, s->board->home_gpio, s->board->home_irq, true);
    s->pwroff_phase = PWROFF_HOME;
    n45_pwroff_arm(s, 150);                 /* a tap, released long before Hold goes down (smoke #19) */
}

static Notifier n45_powerdown_notifier = { .notify = n45_powerdown_req };

static void n45_kbd_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(qdev_get_machine());
    InputKeyEvent *k = evt->u.key.data;
    int q = qemu_input_key_value_to_qcode(k->key);

    switch (q) {
    case Q_KEY_CODE_META_L:
    case Q_KEY_CODE_META_R:
        s->kbd_cmd = k->down;
        return;
    case Q_KEY_CODE_SHIFT:
    case Q_KEY_CODE_SHIFT_R:
        s->kbd_shift = k->down;
        return;
    case Q_KEY_CODE_H:
        if (!k->down || (s->kbd_cmd && s->kbd_shift)) {
            n45_button(s, s->board->home_gpio, s->board->home_irq, k->down);
        }
        return;
    case Q_KEY_CODE_L:
        if (!k->down || s->kbd_cmd) {
            n45_button(s, s->board->power_gpio, s->board->power_irq, k->down);
        }
        return;
    case Q_KEY_CODE_MINUS:
    case Q_KEY_CODE_EQUAL:
        /* Cmd+- / Cmd+= : the M68's volume buttons, the 2G's chords */
        if (s->board->volup_gpio && (!k->down || s->kbd_cmd)) {
            bool up = q == Q_KEY_CODE_EQUAL;
            n45_button(s, up ? s->board->volup_gpio : s->board->voldown_gpio,
                       up ? s->board->volup_irq : s->board->voldown_irq, k->down);
        }
        return;
    default:
        return;
    }
}

static const QemuInputHandler n45_kbd_handler = {
    .name  = "iPod Touch 1G Buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = n45_kbd_event,
};

/*
 * The modem reports battery capacity (+XCIEV field 1) as the PMU's gauge reads it.
 * ponytail: sampled every 10 s of guest time; a PMU change notifier if that ever lags.
 */
static void m68_modem_battery(void *opaque)
{
    IPodTouch1GMachineState *s = opaque;

    if (s->pmu) {
        pcf50633_update_battery(s->pmu);
        object_property_set_int(OBJECT(s->modem), "battery-percent",
                                pcf50633_level_for_adc(s->pmu->adc_values[4]), &error_abort);
    }
    timer_mod(s->modem_battery_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10 * NANOSECONDS_PER_SECOND);
}

/* ---- machine ----------------------------------------------------------- */


/* One S5L8900 I2S controller (the 2G's model, the same AppleS5L8900XI2SController driver) at its DT wiring. */
static void n45_i2s_port(IPodTouch1GMachineState *s, MemoryRegion *sysmem, PL080State **dmac,
                         const S5L8900I2SPort *port)
{
    DeviceState *dev = qdev_new(TYPE_IPOD_TOUCH_I2S);

    IPOD_TOUCH_I2S(dev)->sysic = s->sysic;
    IPOD_TOUCH_I2S(dev)->dmac = dmac[port->dmac];
    IPOD_TOUCH_I2S(dev)->dma_req_id = port->dma_req;
    qdev_prop_set_uint32(dev, "ready-gpio-group", port->ready_irq / 32);
    qdev_prop_set_uint32(dev, "ready-gpio-bit", port->ready_irq % 32);
    qdev_prop_set_bit(dev, "host-output", port->host_output);
    pl080_attach_paced_peripheral(dmac[port->dmac], port->dma_req);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, port->base, &IPOD_TOUCH_I2S(dev)->iomem);
}

static void n45_machine_init(MachineState *machine)
{
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *dev;
    SysBusDevice *busdev;

    s->board = object_dynamic_cast(OBJECT(machine), TYPE_IPHONE_2G_MACHINE) ? &m68_board : &n45_board;

    Object *cpuobj = object_new(machine->cpu_type);
    s->cpu = ARM_CPU(cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(sysmem), &error_abort);
    object_property_set_bool(cpuobj, "has_el3", false, NULL);
    object_property_set_bool(cpuobj, "has_el2", false, NULL);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    s->nsas = cpu_get_address_space(CPU(s->cpu), ARMASIdx_NS);
    object_unref(cpuobj);
    define_arm_cp_regs(s->cpu, n45_cp_reginfo);

    s->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(s->sysclk, 12000000ULL);

    /* VICs */
    dev = pl192_manual_init((char *)"vic0", qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ),
                            qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ), NULL);
    s->vic0 = PL192(dev);
    memory_region_add_subregion(sysmem, N45_VIC0_BASE, &s->vic0->iomem);
    s->irq[0] = g_malloc0(sizeof(qemu_irq) * 32);
    for (int i = 0; i < 32; i++) {
        s->irq[0][i] = qdev_get_gpio_in(dev, i);
    }
    dev = pl192_manual_init((char *)"vic1", NULL);
    s->vic1 = PL192(dev);
    memory_region_add_subregion(sysmem, N45_VIC1_BASE, &s->vic1->iomem);
    s->irq[1] = g_malloc0(sizeof(qemu_irq) * 32);
    for (int i = 0; i < 32; i++) {
        s->irq[1][i] = qdev_get_gpio_in(dev, i);
    }
    s->vic1->daisy = s->vic0;

    /* RAM and the boot windows */
    MemoryRegion *ram = allocate_ram(sysmem, "ram", N45_RAM_BASE, N45_RAM_SIZE);
    allocate_ram(sysmem, "sram1", N45_SRAM1_BASE, 0x10000);
    allocate_ram(sysmem, "vrom", N45_VROM_BASE, N45_VROM_SIZE);
    MemoryRegion *iboot_ram = allocate_ram(sysmem, "iboot", N45_IBOOT_BASE, N45_IBOOT_SIZE);
    /*
     * The bus decodes DRAM again at +0x80000000 (the uncached view). iBoot
     * maps its heap there (VA 0x98xxxxxx -> PA 0x18xxxxxx) and iBoot-159 hands
     * those addresses to the PL080 as they are: its NAND page reads DMA from
     * the FMC FIFO straight into 0x98..., which read back as zeros without the
     * alias ("no signature or no production format"). The AES block's
     * addr-offset is the same decode, seen from that master.
     */
    MemoryRegion *alias = g_new(MemoryRegion, 2);
    memory_region_init_alias(&alias[0], NULL, "ram-uncached", ram, 0, N45_RAM_SIZE);
    memory_region_add_subregion(sysmem, N45_RAM_BASE + 0x80000000u, &alias[0]);
    memory_region_init_alias(&alias[1], NULL, "iboot-uncached", iboot_ram, 0, N45_IBOOT_SIZE);
    memory_region_add_subregion(sysmem, N45_IBOOT_BASE + 0x80000000u, &alias[1]);
    allocate_ram(sysmem, "llb-stubs", N45_LLB_BASE, 0x1000);
    allocate_ram(sysmem, "edgeic", N45_EDGEIC_BASE, 0x1000);
    /* Register windows the 1.x kernel touches but nothing models yet (debt). */
    allocate_ram(sysmem, "watchdog", N45_WATCHDOG_BASE, 0x10000);
    for (int i = 0; i < 2; i++) {
        hwaddr b = s->board->i2s_ram_bases[i];
        if (!b) {
            continue;
        }
        allocate_ram(sysmem, b == N45_IIS0_BASE ? "iis0" : b == N45_IIS1_BASE ? "iis1" : "iis2", b, 0x10000);
    }
    allocate_ram(sysmem, "mpvd", N45_MPVD_BASE, 0x70000);
    allocate_ram(sysmem, "h264bpd", N45_H264BPD_BASE, 0x1000);

    /* NOR: 1 MiB parallel CFI flash on -drive if=pflash */
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);
    if (!dinfo) {
        error_report("iPod-Touch-1G: the NOR image is required: -drive if=pflash,format=raw,file=nor.bin");
        exit(1);
    }
    if (!pflash_cfi02_register(N45_NOR_BASE, "nor", N45_NOR_SIZE, blk_by_legacy_dinfo(dinfo),
                               4096, 1, 2, 0x00bf, 0x273f, 0x0, 0x0, 0x555, 0x2aa, 0)) {
        error_report("iPod-Touch-1G: cannot register the NOR flash");
        exit(1);
    }

    /* clocks */
    dev = qdev_new("ipodtouch.clock");
    qdev_prop_set_bit(dev, "s5l8900", true);
    memory_region_add_subregion(sysmem, N45_CLOCK0_BASE, &IPOD_TOUCH_CLOCK(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    dev = qdev_new("ipodtouch.clock");
    qdev_prop_set_bit(dev, "s5l8900", true);
    memory_region_add_subregion(sysmem, N45_CLOCK1_BASE, &IPOD_TOUCH_CLOCK(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* timer */
    dev = qdev_new("ipodtouch.timer");
    qdev_prop_set_uint32(dev, "irqlatch", 0xF8);
    qdev_prop_set_uint32(dev, "freq-hz", 12000000);
    IPOD_TOUCH_TIMER(dev)->sysclk = s->sysclk;
    IPOD_TOUCH_TIMER(dev)->first_config_hook = n45_usb_wrangler_quirk;
    IPOD_TOUCH_TIMER(dev)->first_config_opaque = s;
    /* nclk: 3A101a programs Celestial's 1880 Hz key click as 6382 at /2. */
    qdev_prop_set_uint32(dev, "input-hz", 24000000);
    if (s->board->piezo) {
        /* The piezo on timer 1 (/arm-io/timer/buzzer). */
        DeviceState *piezo = qdev_new(TYPE_IPOD_TOUCH_PIEZO);
        qdev_realize_and_unref(piezo, NULL, &error_fatal);
        IPOD_TOUCH_TIMER(dev)->output_hook = ipod_touch_piezo_timer_output;
        IPOD_TOUCH_TIMER(dev)->output_opaque = piezo;
    }
    memory_region_add_subregion(sysmem, N45_TIMER1_BASE, &IPOD_TOUCH_TIMER(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_TIMER1_IRQ));

    /* system controller: 7 GPIO interrupt groups */
    dev = qdev_new("ipodtouch.sysic");
    qdev_prop_set_bit(dev, "direct-boot", true);   /* no LLB latched the epoch: n45_stage_boot_chain */
    qdev_prop_set_bit(dev, "s5l8900", true);
    s->sysic = IPOD_TOUCH_SYSIC(dev);
    memory_region_add_subregion(sysmem, N45_SYSIC_BASE, &s->sysic->iomem);
    busdev = SYS_BUS_DEVICE(dev);
    for (int grp = 0; grp < GPIO_NUMINTGROUPS; grp++) {
        sysbus_connect_irq(busdev, grp, n45_irq(s, n45_gpio_irqs[grp]));
    }
    sysbus_realize_and_unref(busdev, &error_fatal);

    /* GPIO pads */
    dev = qdev_new("ipodtouch.gpio");
    qdev_prop_set_uint32(dev, "fsel-offset", 0x320);
    if (s->board->volup_gpio) {
        /* The volume buttons are active low (buttons,m68 flags 0; AppleM68Buttons reads a low pad as held). */
        qdev_prop_set_uint32(dev, "rest-high-pad", GPIO2PAD(s->board->volup_gpio));
        /* the ring switch is a level on the same pad: silent rests high */
        qdev_prop_set_uint32(dev, "rest-high-mask", 1u << GPIO2PIN(s->board->volup_gpio) | 1u << GPIO2PIN(s->board->voldown_gpio) |
                             (s->ring_silent ? 1u << GPIO2PIN(s->board->ring_gpio) : 0));
    }
    s->gpio = IPOD_TOUCH_GPIO(dev);
    memory_region_add_subregion(sysmem, N45_GPIO_BASE, &s->gpio->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /*
     * SDIO host, and with wifi (the default) the Marvell 88W8686 the DT's
     * sdio node drives (AppleMRVL868x), its frames on -netdev ...,id=wifi0,
     * user networking when none is given, as on the iPad.
     */
    dev = qdev_new("ipodtouch.sdio");
    if (s->wifi) {
        static const BCMSDIOChip mrvl8686 = {
            .manfid = 0x02df, .prodid = 0x9103,   /* Marvell, 88W8686 */
            .vers1 = { "Marvell", "802.11 SDIO ID: 0B" },
            .functions = 1, .no_mac_funce = true, .fbr_iface = 0x07,   /* WLAN */
        };
        DeviceState *card = qdev_new(TYPE_MRVL8686);
        object_property_add_child(OBJECT(machine), "wifi-card", OBJECT(card));
        if (s->wifi_mac && s->wifi_mac[0]) {
            object_property_parse(OBJECT(card), "mac", s->wifi_mac, &error_fatal);
        }
        qdev_realize_and_unref(card, NULL, &error_fatal);
        if (!qemu_find_netdev("wifi0")) {
            QemuOpts *o = qemu_opts_parse_noisily(qemu_find_opts("netdev"), "type=user,id=wifi0", false);
            Error *err = NULL;
            if (o) {
                netdev_add(o, &err);
            }
            if (err) {
                warn_reportf_err(err, "Wi-Fi has no network: ");
            }
        }
        mrvl8686_setup_net(MRVL8686(card));
        ipod_touch_sdio_set_chip(IPOD_TOUCH_SDIO(dev), &mrvl8686);
        IPOD_TOUCH_SDIO(dev)->card_present = true;
        object_property_set_link(OBJECT(dev), "mrvl", OBJECT(card), &error_fatal);
    }
    memory_region_add_subregion(sysmem, N45_SDIO_BASE, &IPOD_TOUCH_SDIO(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_SDIO_IRQ));
    if (s->wifi) {
        /* After the host's realize: its input needs a canonical path. */
        qdev_connect_gpio_out(DEVICE(IPOD_TOUCH_SDIO(dev)->mrvl), 0,
                              qdev_get_gpio_in_named(dev, "card-irq", 0));
    }

    /*
     * The M68's baseband: the fake modem (hw/misc/ios_baseband*.c, docs/baseband/commcenter-1.0.md)
     * on UART1, its controls on the machine. baseband=off leaves UART1 a plain serial_hd(1).
     */
    Chardev *modem = NULL;
    if (s->board == &m68_board && s->baseband) {
        static const char *const controls[] = {
            "carrier", "mcc-mnc", "signal-dbm", "registered", "sim-present", "imsi", "iccid", "voicemail",
            "answer-delay-ms", "incoming-call", "remote-answer", "remote-hangup", "incoming-sms",
            "call-state", "last-dialed", "last-mo-sms", "mo-sms-count",
        };
        dev = qdev_new(TYPE_IOS_BASEBAND);
        s->modem = dev;
        object_property_add_child(OBJECT(machine), "baseband-modem", OBJECT(dev));
        if (s->imei && s->imei[0]) {
            object_property_set_str(OBJECT(dev), "imei", s->imei, &error_fatal);
        }
        /* 1.x takes its time zone only from the network: lockdownd has no TimeZone to set. */
        qdev_prop_set_bit(dev, "nitz", true);
        qdev_realize_and_unref(dev, NULL, &error_fatal);
        for (int i = 0; i < ARRAY_SIZE(controls); i++) {
            object_property_add_alias(OBJECT(machine), controls[i], OBJECT(dev), controls[i]);
        }
        modem = ios_baseband_chardev(dev);
    }

    /* UARTs (Samsung, S5L interrupt semantics) */
    for (int i = 0; i < 5; i++) {
        static const hwaddr bases[5] = { N45_UART0_BASE, N45_UART1_BASE, N45_UART2_BASE,
                                         N45_UART3_BASE, N45_UART4_BASE };
        dev = qdev_new("exynos4210.uart");
        qdev_prop_set_bit(dev, "s5l8720-irq", true);
        qdev_prop_set_chr(dev, "chardev", i == 1 && modem ? modem : serial_hd(i));
        qdev_prop_set_uint32(dev, "channel", i);
        /* 16 deep on the M68's modem port: the S5L8900's UFSTAT receive count is four
         * bits (0..15, a full flag above), so a deeper FIFO reads back as a wrapped,
         * small count and the H5 driver stops reading mid-packet. (The console UARTs
         * keep 256: only the host types into them.) */
        qdev_prop_set_uint32(dev, "rx-size", s->board == &m68_board && i == 1 ? 16 : 256);
        qdev_prop_set_uint32(dev, "tx-size", 256);
        /* The M68's baseband (UART1) and Bluetooth (UART3) ports flow-control on CTS. With
         * nothing attached the far end reads ready, so a write goes out and finds no answer. */
        qdev_prop_set_bit(dev, "cts", s->board == &m68_board && (i == 1 || i == 3));
        /* 115200 8N1 would be 86.8 us per character (tx-char-ns=86800); left
         * instant, since the guest's polled kprintf would otherwise take the
         * whole boot to ~10x real time on this tree. */
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, bases[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_UART0_IRQ + i));
    }

    /* SPI: nothing on SPI0, the LCD panel on SPI1, the Zephyr digitizer on SPI2 */
    ipod_touch_spi_create(N45_SPI0_BASE, n45_irq(s, N45_SPI0_IRQ), 0, "none", true);
    ipod_touch_spi_create(N45_SPI1_BASE, n45_irq(s, N45_SPI1_IRQ), 1, TYPE_S5L8900_LCD_PANEL, true);
    dev = ipod_touch_spi_create(N45_SPI2_BASE, n45_irq(s, N45_SPI2_IRQ), 2, s->board->touch, true);
    s->mt = IPOD_TOUCH_SPI(dev)->mt;
    s->mt->sysic = s->sysic;       /* ATN straight into its GPIO-IC line (the N45's group 4 bit 27, devos50) */
    s->mt->sysic_atn_group = s->board->touch_atn_irq / 32;
    s->mt->sysic_atn_bit = s->board->touch_atn_irq % 32;
    s->mt->gpio_state = s->gpio;
    if (s->board->touch_cs_gpio >= 0) {
        /* The Zephyr1 frames its bootloader stream by chip select (a GPIO pad, the DT's spi_cs0). */
        qdev_connect_gpio_out(DEVICE(s->gpio), GPIO2PAD(s->board->touch_cs_gpio) * 8 + GPIO2PIN(s->board->touch_cs_gpio),
                              qdev_get_gpio_in_named(DEVICE(s->mt), SSI_GPIO_CS, 0));
    }

    /* CLCD, S5L8900 register layout */
    dev = qdev_new("ipodtouch.lcd");
    qdev_prop_set_bit(dev, "s5l8900", true);
    if (s->panel_w) {
        qdev_prop_set_uint32(dev, "panel-width", s->panel_w);
        qdev_prop_set_uint32(dev, "panel-height", s->panel_h);
    }
    s->lcd = IPOD_TOUCH_LCD(dev);
    s->lcd->sysmem = sysmem;
    s->lcd->mt = s->mt;
    memory_region_add_subregion(sysmem, N45_DISPLAY_BASE, &s->lcd->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_LCD_IRQ));

    /* AES, SHA1 */
    dev = qdev_new("ipodtouch.aes");
    qdev_prop_set_uint32(dev, "addr-offset", 0x80000000);
    qdev_prop_set_bit(dev, "s5l8900-compat", true);
    memory_region_add_subregion(sysmem, N45_AES_BASE, &IPOD_TOUCH_AES(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    dev = qdev_new("ipodtouch.sha1");
    memory_region_add_subregion(sysmem, N45_SHA1_BASE, &IPOD_TOUCH_SHA1(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* 8900 engine hook */
    MemoryRegion *iomem = g_new(MemoryRegion, 1);
    memory_region_init_io(iomem, OBJECT(machine), &engine_8900_ops, s, "8900engine", 0x100);
    memory_region_add_subregion(sysmem, N45_ENGINE_8900_BASE, iomem);

    /* NAND: FMC + ECC, driven by the ADM */
    dev = qdev_new(TYPE_S5L8900_FMC);
    qdev_prop_set_string(dev, "nand", s->nand_path);
    qdev_prop_set_uint32(dev, "banks", s->board->nand_banks);
    if (s->nand_overlay && s->nand_overlay[0]) {
        qdev_prop_set_string(dev, "nand-overlay", s->nand_overlay);
    }
    s->fmc = S5L8900_FMC(dev);
    memory_region_add_subregion(sysmem, N45_NAND_BASE, &s->fmc->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    dev = qdev_new(TYPE_S5L8900_NAND_ECC);
    memory_region_add_subregion(sysmem, N45_NAND_ECC_BASE, &S5L8900_NAND_ECC(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_NAND_ECC_IRQ));

    dev = qdev_new(TYPE_S5L8900_ADM);
    s->adm = S5L8900_ADM(dev);
    s->adm->fmc = s->fmc;
    object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
    memory_region_add_subregion(sysmem, N45_ADM_BASE, &s->adm->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_ADM_IRQ));

    /* USB OTG + PHY */
    /* usb-tcp-addr: the host bridge (usbmuxd-qemu) the device-mode core talks to, as on the 2G */
    dev = ipod_touch_init_usb_otg(n45_irq(s, N45_USB_OTG_IRQ), (uint32_t *)s5l8900_usb_hwcfg);
    DeviceState *usb_otg_dev = dev;
    synopsys_usb_set_tcp_addr(S5L8900USBOTG(dev), s->usb_tcp_addr);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, N45_USBOTG_BASE, &S5L8900USBOTG(dev)->iomem);
    dev = qdev_new("ipodtouch.usbphys");
    qdev_connect_gpio_out_named(dev, "phy-reset", 0,
                               qdev_get_gpio_in_named(usb_otg_dev, "phy-reset", 0));
    memory_region_add_subregion(sysmem, N45_USBPHYS_BASE, &IPOD_TOUCH_USB_PHYS(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* two PL080 DMACs */
    static const hwaddr dmac_bases[2] = { N45_DMAC0_BASE, N45_DMAC1_BASE };
    static const int dmac_irqs[2] = { N45_DMAC0_IRQ, N45_DMAC1_IRQ };
    PL080State *dmac[2];
    for (int i = 0; i < 2; i++) {
        dev = qdev_new("pl080");
        dmac[i] = PL080(dev);
        object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
        memory_region_add_subregion(sysmem, dmac_bases[i], &PL080(dev)->iomem1);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, dmac_irqs[i]));
    }

    /* I2C0: accelerometer (and on the M68 the PMU and codec); I2C1: the N45's PMU and codec */
    I2CBus *i2c[2];
    for (int i = 0; i < 2; i++) {
        dev = qdev_new("ipodtouch.i2c");
        IPOD_TOUCH_I2C(dev)->base = i;
        memory_region_add_subregion(sysmem, i ? N45_I2C1_BASE : N45_I2C0_BASE, &IPOD_TOUCH_I2C(dev)->iomem);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, i ? N45_I2C1_IRQ : N45_I2C0_IRQ));
        i2c[i] = IPOD_TOUCH_I2C(dev)->bus;
    }
    dev = DEVICE(i2c_slave_new("lis302dl", 0x1D));
    if (s->board->accel_mount) {
        qdev_prop_set_string(dev, "mount", s->board->accel_mount);
    }
    i2c_slave_realize_and_unref(I2C_SLAVE(dev), i2c[0], &error_fatal);
    s->accel = LIS302DL(dev);
    lis302dl_apply_orientation(s->accel, 1);   /* init ran before the mount was set */
    /* The host's controls, the same names as the 2G and the iPad: UIDeviceOrientation 1-6
     * (lis302dl_apply_orientation also turns the LCD's presented picture, as the 2G's), raw
     * counts, a shake; accel-pitch/-roll/-pose are machine properties. Without them the app's
     * rotation never reached the 1G: the guest stayed portrait under a turned shell. */
    object_property_add_alias(OBJECT(machine), "accel-orientation", OBJECT(s->accel), "orientation");
    object_property_add_alias(OBJECT(machine), "accel-x", OBJECT(s->accel), "x");
    object_property_add_alias(OBJECT(machine), "accel-y", OBJECT(s->accel), "y");
    object_property_add_alias(OBJECT(machine), "accel-z", OBJECT(s->accel), "z");
    object_property_add_alias(OBJECT(machine), "accel-shake", OBJECT(s->accel), "shake");

    {
        /* PCF50635 at 0x73 (the N45's I2C1, the M68's I2C0). Its interrupt is GPIO-IC line 0x55 (the
         * DT's pmu node: interrupt-parent gpio, interrupts <0x55 1>). */
        I2CSlave *pmu = i2c_slave_new("pcf50633", 0x73);
        s->pmu = PCF50633(pmu);
        qdev_prop_set_uint8(DEVICE(pmu), "shutdown-reg", 0x0c);
        /* MBCS1 USBPRES|USBOK: a host on the cable. Without it the power source reads "ext 0",
         * the USB stack stops ("cable removed") and the device deep-sleeps after the boot. */
        qdev_prop_set_uint8(DEVICE(pmu), "usb-status-reg", 0x4b);
        qdev_prop_set_uint8(DEVICE(pmu), "usb-status-bits", 0x03);
        qdev_prop_set_bit(DEVICE(pmu), "rtc-bcd", true);
        /* LEDENA bit 0: 1.x clears it when the display sleeps (qemu_ios_ui_display_sleeping). */
        qdev_prop_set_uint8(DEVICE(pmu), "backlight-enable-reg", 0x29);
        qdev_prop_set_uint8(DEVICE(pmu), "backlight-enable-bit", 0x01);
        qdev_prop_set_uint8(DEVICE(pmu), "backlight-level-reg", 0);
        PCF50633(pmu)->usb_cable = (s->usb_tcp_addr && s->usb_tcp_addr[0]) || getenv("IT_USB_TCP");
        i2c_slave_realize_and_unref(pmu, i2c[s->board->pmu_i2c], &error_fatal);
        qdev_connect_gpio_out(DEVICE(pmu), 0, qdev_get_gpio_in(DEVICE(s->sysic), 0x55));
    }
    /* WM8758 codec at 0x1A beside it (the DT's audio0@1A). */
    i2c_slave_create_simple(i2c[s->board->pmu_i2c], "wm8758", 0x1A);

    /*
     * The WM8758's data port (the N45's I2S1, the M68's I2S0): the 2G's model (the same
     * AppleS5L8900XI2SController driver). dma-parent is dmac1 and the TX
     * channel's DT config word 0x884 is flow 1 (memory to peripheral) with
     * peripheral id 2; the FIFO is +0x10. interrupts <0xaa> over the GPIO IC is
     * group 5 bit 10: the source the driver's DMA start waits on. Without it
     * every system sound blocks mediaserverd for 10 s in StartIO
     * (kIOReturnNotReady) and no Buzz is played at all; without the codec
     * too, mediaserverd crash-loops on an empty device list and plays at most
     * one Buzz per 10 s respawn.
     */
    const S5L8900Board *bd = s->board;
    /*
     * N45: no host voice. The WM8758's analogue side (the headphone jack) is
     * not modelled, and the guest's Beep PCM here replays the ring's tail every
     * 0.37 s after a sound. The Mac hears the piezo (ipod_touch_piezo.c), what
     * an N45 with nothing in the jack plays. The M68 has no piezo: its clicks
     * and ringer are this PCM, so the host plays it.
     */
    n45_i2s_port(s, sysmem, dmac, &bd->codec_i2s);
    /*
     * The M68's baseband voice port, which carries what the speaker plays. With the window plain RAM,
     * AppleBasebandOutput's DMA start failed (device not ready), mediaserverd's StartIO with it, and the
     * device played nothing at all.
     */
    if (bd->bb_i2s.base) {
        n45_i2s_port(s, sysmem, dmac, &bd->bb_i2s);
    }

    /*
     * MBX: the 2G's model, the same PowerVR MBX Lite and the same AppleMBX driver. The id stub it
     * replaces read 0x12c (interrupt status) without the idle bit 0x40, so the first time anything
     * drove the engine (LayerKit's GL path, a display swap) the driver spun on it forever.
     * Interrupt 0xC is the mbx node's in the n45ap device tree.
     */
    dev = qdev_new("ipodtouch.mbx");
    IPOD_TOUCH_MBX(dev)->irq_enabled = true;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_MBX_IRQ));
    memory_region_add_subregion(sysmem, N45_MBX_BASE, &IPOD_TOUCH_MBX(dev)->iomem1);

    /* chip id: revision 2 */
    dev = qdev_new("ipodtouch.chipid");
    qdev_prop_set_uint32(dev, "word1", 0x2 << 24);
    qdev_prop_set_uint32(dev, "word2", 0);
    memory_region_add_subregion(sysmem, N45_CHIPID_BASE, &IPOD_TOUCH_CHIPID(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* TV-out: mixer/SDO blocks at the same three bases as the S5L8720 */
    dev = qdev_new("ipodtouch.tvout");
    memory_region_add_subregion(sysmem, N45_TVOUT1_BASE, &IPOD_TOUCH_TVOUT(dev)->mixer2_iomem);
    memory_region_add_subregion(sysmem, N45_TVOUT2_BASE, &IPOD_TOUCH_TVOUT(dev)->mixer1_iomem);
    memory_region_add_subregion(sysmem, N45_TVOUT3_BASE, &IPOD_TOUCH_TVOUT(dev)->sdo_iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_TVOUT_SDO_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1, n45_irq(s, N45_TVOUT_MIXER_IRQ));

    if (s->tvout_workaround) {
        iomem = g_new(MemoryRegion, 1);
        memory_region_init_io(iomem, OBJECT(machine), &zero_word_ops, NULL, "tvout-workaround", 4);
        memory_region_add_subregion_overlap(sysmem, s->tvout_workaround, iomem, 1);
    }

    qemu_register_reset(n45_cpu_reset, s);
    qemu_input_handler_register(DEVICE(s->cpu), &n45_kbd_handler);
    s->pwroff_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, n45_pwroff_tick, s);
    s->btn_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, n45_button_release_due, s);
    if (s->modem) {
        s->modem_battery_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, m68_modem_battery, s);
        m68_modem_battery(s);
    }
    qemu_register_powerdown_notifier(&n45_powerdown_notifier);
}

/* ---- properties -------------------------------------------------------- */

#define N45_STR_PROP(field)                                                     \
static char *n45_get_##field(Object *obj, Error **errp)                         \
{                                                                               \
    return g_strdup(IPOD_TOUCH_1G_MACHINE(obj)->field);                         \
}                                                                               \
static void n45_set_##field(Object *obj, const char *value, Error **errp)       \
{                                                                               \
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(obj);                    \
    g_free(s->field);                                                           \
    s->field = g_strdup(value);                                                 \
}

N45_STR_PROP(bootrom_path)
N45_STR_PROP(iboot_path)
N45_STR_PROP(nand_path)
N45_STR_PROP(nand_overlay)
N45_STR_PROP(usb_tcp_addr)
N45_STR_PROP(wifi_mac)

static void n45_get_usb_wrangler_quirk(Object *obj, Visitor *v, const char *name,
                                       void *opaque, Error **errp)
{
    bool value = IPOD_TOUCH_1G_MACHINE(obj)->usb_wrangler_quirk;
    visit_type_bool(v, name, &value, errp);
}

static void n45_set_usb_wrangler_quirk(Object *obj, Visitor *v, const char *name,
                                       void *opaque, Error **errp)
{
    visit_type_bool(v, name, &IPOD_TOUCH_1G_MACHINE(obj)->usb_wrangler_quirk, errp);
}

static void n45_get_tvout_workaround(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &IPOD_TOUCH_1G_MACHINE(obj)->tvout_workaround, errp);
}

static void n45_set_tvout_workaround(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &IPOD_TOUCH_1G_MACHINE(obj)->tvout_workaround, errp);
}

static bool n45_get_gles_debug(Object *obj, Error **errp)
{
    return IPOD_TOUCH_1G_MACHINE(obj)->gles_debug;
}

static void n45_set_gles_debug(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_1G_MACHINE(obj)->gles_debug = value;
    gles_host_set_debug(value);
}

static char *n45_get_gles_rejects(Object *obj, Error **errp)
{
    return gles_host_rejects();
}

static void n45_get_gles_contexts(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    int64_t count = gles_host_context_count();
    visit_type_int(v, name, &count, errp);
}

/* The app's attitude (hw/arm/ipod-attitude.h) in the device's frame. The sensor keeps it, so an
 * accel-orientation in between leaves the angles it set (as the 2G's machine reads them back). */
static void n45_get_accel_angle(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    LIS302DLState *a = IPOD_TOUCH_1G_MACHINE(obj)->accel;
    double value = a ? (!strcmp(name, "accel-pitch") ? a->pitch_mdeg : a->roll_mdeg) / 1000.0 : 0;

    visit_type_number(v, name, &value, errp);
}

static void n45_set_accel_angle(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    LIS302DLState *a = IPOD_TOUCH_1G_MACHINE(obj)->accel;
    double value, pitch, roll;

    if (!visit_type_number(v, name, &value, errp)) {
        return;
    }
    if (!a) {
        error_setg(errp, "%s is set once the machine has started", name);
        return;
    }
    pitch = a->pitch_mdeg / 1000.0, roll = a->roll_mdeg / 1000.0;
    if (!isfinite(value) || value < -180 || value > 180) {
        error_setg(errp, "%s must be finite and between -180 and 180 degrees", name);
        return;
    }
    *(!strcmp(name, "accel-pitch") ? &pitch : &roll) = value;
    lis302dl_apply_attitude(a, pitch, roll, a->flat_pose);
}

static char *n45_get_panel(Object *obj, Error **errp)
{
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(obj);
    return s->panel_w ? g_strdup_printf("%ux%u", s->panel_w, s->panel_h) : g_strdup("");
}

/* "panel=WxH": a panel of another size than the shipped 320x480 (issue #21). */
static void n45_set_panel(Object *obj, const char *value, Error **errp)
{
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(obj);
    unsigned w, h;
    char end;

    if (s->lcd) {
        error_setg(errp, "panel must be set before the machine starts");
        return;
    }
    /* The 2G's limits (its S5L8720 window keeps 9 bits of height); 3A101a ran at 384x504. */
    if (sscanf(value, "%ux%u%c", &w, &h, &end) != 2 || w < 64 || h < 64 || w > 1024 || h > 511 || (w & 1)) {
        error_setg(errp, "panel must be WxH (even width 64..1024, height 64..511)");
        return;
    }
    s->panel_w = w;
    s->panel_h = h;
}

static char *n45_get_accel_pose(Object *obj, Error **errp)
{
    LIS302DLState *a = IPOD_TOUCH_1G_MACHINE(obj)->accel;

    return g_strdup(a && a->flat_pose ? "flat" : "upright");
}

static void n45_set_accel_pose(Object *obj, const char *value, Error **errp)
{
    LIS302DLState *a = IPOD_TOUCH_1G_MACHINE(obj)->accel;

    if (strcmp(value, "flat") && strcmp(value, "upright")) {
        error_setg(errp, "accel-pose must be upright or flat");
        return;
    }
    if (!a) {
        error_setg(errp, "accel-pose is set once the machine has started");
        return;
    }
    lis302dl_apply_attitude(a, a->pitch_mdeg / 1000.0, a->roll_mdeg / 1000.0, !strcmp(value, "flat"));
}

static bool n45_get_wifi(Object *obj, Error **errp)
{
    return IPOD_TOUCH_1G_MACHINE(obj)->wifi;
}

static void n45_set_wifi(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_1G_MACHINE(obj)->wifi = value;
}

static void n45_machine_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    object_class_property_add_str(klass, "bootrom", n45_get_bootrom_path, n45_set_bootrom_path);
    object_class_property_set_description(klass, "bootrom", "S5L8900 bootrom (64 KiB, staged at 0x20000000)");
    object_class_property_add_str(klass, "iboot", n45_get_iboot_path, n45_set_iboot_path);
    object_class_property_set_description(klass, "iboot", "decrypted iBoot-204 (entered at 0x18000000)");
    object_class_property_add_str(klass, "nand", n45_get_nand_path, n45_set_nand_path);
    object_class_property_set_description(klass, "nand", "NAND directory: bank<N>/<page>.page");
    object_class_property_add_str(klass, "nand-overlay", n45_get_nand_overlay, n45_set_nand_overlay);
    object_class_property_set_description(klass, "nand-overlay", "writable directory guest NAND programs land in");
    object_class_property_add_str(klass, "usb-tcp-addr", n45_get_usb_tcp_addr, n45_set_usb_tcp_addr);
    object_class_property_set_description(klass, "usb-tcp-addr",
        "host:port of the USB host bridge (usbmuxd-qemu); empty = no cable (IT_USB_TCP)");
    object_class_property_add(klass, "usb-wrangler-quirk", "bool", n45_get_usb_wrangler_quirk,
                              n45_set_usb_wrangler_quirk, NULL, NULL);
    object_class_property_set_description(klass, "usb-wrangler-quirk",
        "let 1.x's USB wrangler take a PHY published before its notifier is stored (default on)");
    object_class_property_add(klass, "tvout-workaround", "uint32", n45_get_tvout_workaround,
                              n45_set_tvout_workaround, NULL, NULL);
    object_class_property_set_description(klass, "tvout-workaround",
        "physical address of a kernel word to pin at zero for the TV-out driver (0 = off)");

    object_class_property_add_bool(klass, "wifi", n45_get_wifi, n45_set_wifi);
    object_class_property_set_description(klass, "wifi",
        "on (default): the Marvell 88W8686 on the SDIO bus, its frames on -netdev id=wifi0, "
        "or on user networking when none is given; off = no card");
    object_class_property_add_str(klass, "wifi-mac", n45_get_wifi_mac, n45_set_wifi_mac);
    object_class_property_set_description(klass, "wifi-mac",
        "the 88W8686's EEPROM MAC, aa:bb:cc:dd:ee:ff (the unit's; iBoot's DT copy comes from nvram wifiaddr)");

    object_class_property_add(klass, "accel-pitch", "number", n45_get_accel_angle, n45_set_accel_angle, NULL, NULL);
    object_class_property_add(klass, "accel-roll", "number", n45_get_accel_angle, n45_set_accel_angle, NULL, NULL);
    object_class_property_add_str(klass, "accel-pose", n45_get_accel_pose, n45_set_accel_pose);
    object_class_property_add_str(klass, "panel", n45_get_panel, n45_set_panel);
    object_class_property_set_description(klass, "accel-pose", "upright (default) or flat");

    mc->desc = "iPod touch 1G (N45AP, S5L8900)";
    mc->init = n45_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
    /*
     * Windows the 1.x firmware touches that no model claims read as zero
     * rather than data-aborting the guest. Debt: log them (-d guest_errors)
     * and give each a model or an explicit RAM window.
     */
    mc->ignore_memory_transaction_failures = true;
}

/* Read-only physical display observation; dark pixels are not power-off. */
static bool n45_get_display_sleeping(Object *obj, Error **errp)
{
    return lcd_backlight_is_off();
}

static void n45_instance_init(Object *obj)
{
    object_property_add_bool(obj, "display-sleeping", n45_get_display_sleeping, NULL);
    object_property_set_description(obj, "display-sleeping",
        "Guest-controlled LCD backlight is off; not PMU standby or shutdown");
    IPOD_TOUCH_1G_MACHINE(obj)->usb_wrangler_quirk = true;
    IPOD_TOUCH_1G_MACHINE(obj)->wifi = true;
    guest_pkg_init(&IPOD_TOUCH_1G_MACHINE(obj)->pkg, obj);
    object_property_add_str(obj, "gles-rejects", n45_get_gles_rejects, NULL);
    object_property_set_description(obj, "gles-rejects",
        "Every refusal the GL bridge made so far, one NAME<tab>COUNT per line");
    object_property_add(obj, "gles-contexts", "int", n45_get_gles_contexts, NULL, NULL, NULL);
    object_property_add_bool(obj, "gles-debug", n45_get_gles_debug, n45_set_gles_debug);
}

static bool m68_get_ring_switch(Object *obj, Error **errp)
{
    return IPOD_TOUCH_1G_MACHINE(obj)->ring_silent;
}

/* The ring/silent switch: a level, its interrupt on both edges (buttons,m68 type 7). */
static void m68_set_ring_switch(Object *obj, bool value, Error **errp)
{
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(obj);

    s->ring_silent = value;
    if (s->gpio && s->board && s->board->ring_gpio) {
        uint32_t bit = 1u << GPIO2PIN(s->board->ring_gpio);
        s->gpio->rest_high_mask = (s->gpio->rest_high_mask & ~bit) | (value ? bit : 0);   /* kept across a reboot */
        n45_button(s, s->board->ring_gpio, s->board->ring_irq, value);
    }
}

N45_STR_PROP(imei)

static bool m68_get_baseband(Object *obj, Error **errp)
{
    return IPOD_TOUCH_1G_MACHINE(obj)->baseband;
}

static void m68_set_baseband(Object *obj, bool value, Error **errp)
{
    IPOD_TOUCH_1G_MACHINE(obj)->baseband = value;
}

static void m68_instance_init(Object *obj)
{
    IPOD_TOUCH_1G_MACHINE(obj)->baseband = true;
}

static void m68_machine_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "iPhone (M68AP, S5L8900)";
    object_class_property_add_bool(klass, "baseband", m68_get_baseband, m68_set_baseband);
    object_class_property_set_description(klass, "baseband",
        "on (default): the fake modem (ios-baseband) on UART1, its controls on the machine; "
        "off: UART1 is the second -serial");
    object_class_property_add_str(klass, "imei", n45_get_imei, n45_set_imei);
    object_class_property_set_description(klass, "imei",
        "the unit's IMEI (FirmwareKit's device.lock.json machine.imei), for the modem on UART1 to report");
    object_class_property_add_bool(klass, "ring-switch", m68_get_ring_switch, m68_set_ring_switch);
    object_class_property_set_description(klass, "ring-switch",
        "the ring/silent switch: on = silent (pad 0x1603 high), off = ring (default)");
}

static const TypeInfo m68_machine_info = {
    .name          = TYPE_IPHONE_2G_MACHINE,
    .parent        = TYPE_IPOD_TOUCH_1G_MACHINE,
    .instance_init = m68_instance_init,
    .class_init    = m68_machine_class_init,
};

static const TypeInfo n45_machine_info = {
    .name          = TYPE_IPOD_TOUCH_1G_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(IPodTouch1GMachineState),
    .instance_init = n45_instance_init,
    .class_init    = n45_machine_class_init,
};

static void n45_machine_types(void)
{
    type_register_static(&n45_machine_info);
    type_register_static(&m68_machine_info);
}

type_init(n45_machine_types)
