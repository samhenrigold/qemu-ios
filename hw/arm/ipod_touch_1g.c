/*
 * iPod touch 1G (N45AP, S5L8900) -- `-M iPod-Touch-1G`.
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
 * s5l8900_* models (FMC, NAND ECC, ADM, LCD panel) and the three small stubs
 * kept here (8900 engine hook, MBX ids, TV-out workaround).
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
#include "hw/arm/ipod_touch_aes.h"
#include "hw/arm/ipod_touch_sha1.h"
#include "hw/arm/s5l8900_nand_ecc.h"
#include "hw/arm/s5l8900_lcd_panel.h"
#include "hw/i2c/ipod_touch_i2c.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "crypto/cipher.h"
#include "ui/input.h"

static const int n45_gpio_irqs[GPIO_NUMINTGROUPS] = {
    N45_GPIO_G0_IRQ, N45_GPIO_G1_IRQ, N45_GPIO_G2_IRQ, N45_GPIO_G3_IRQ,
    N45_GPIO_G4_IRQ, N45_GPIO_G5_IRQ, N45_GPIO_G6_IRQ,
};

static const uint32_t s5l8900_usb_hwcfg[] = { 0, 0x7a8f60d0, 0x082000e8, 0x01f08024 };

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

/* ---- MBX id stub ------------------------------------------------------- */

/*
 * The PowerVR MBX Lite. iPhone OS 1.x's kernel only probes it for its ids
 * (SpringBoard runs with LK_ENABLE_MBX2D=0 in devos50's image); the full 2.x/
 * 3.x model (ipodtouch.mbx) is for later builds. Stub (fidelity class S).
 */
static uint64_t mbx_stub_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case 0x12c:  return 0x100;
    case 0xf00:  return (1 << 0x18) | 0x10000;
    case 0x1020: return 0x10000;
    default:     return 0;
    }
}

static void mbx_stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps mbx_stub_ops = {
    .read = mbx_stub_read,
    .write = mbx_stub_write,
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

/* ---- boot images ------------------------------------------------------- */

static void n45_stage(IPodTouch1GMachineState *s, const char *what, const char *path,
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
}

static void n45_write32(IPodTouch1GMachineState *s, hwaddr addr, uint32_t v)
{
    uint32_t le = cpu_to_le32(v);
    address_space_write(s->nsas, addr, MEMTXATTRS_UNSPECIFIED, &le, 4);
}

/* Re-staged on every reset, so a warm reboot enters the same iBoot. */
static void n45_stage_boot_chain(IPodTouch1GMachineState *s)
{
    n45_stage(s, "bootrom", s->bootrom_path, N45_VROM_BASE, N45_VROM_SIZE);
    n45_stage(s, "iboot", s->iboot_path, N45_IBOOT_BASE, N45_IBOOT_SIZE);

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

static void n45_cpu_reset(void *opaque)
{
    IPodTouch1GMachineState *s = opaque;

    cpu_reset(CPU(s->cpu));
    n45_stage_boot_chain(s);
    cpu_set_pc(CPU(s->cpu), N45_IBOOT_BASE);
}

/* ---- buttons ----------------------------------------------------------- */

/* Same chords as the 2G (imgtools/itqmp.py BUTTONS): Cmd+Shift+H home, Cmd+L power. */
static void n45_button(IPodTouch1GMachineState *s, uint32_t gpio, uint32_t gpio_irq, bool down)
{
    uint32_t *pads = s->gpio->gpio_state;
    bool was = gpio_is_on(pads, gpio);

    if (down == was) {
        return;
    }
    if (down) {
        gpio_set_on(pads, gpio);
    } else {
        gpio_set_off(pads, gpio);
    }
    unsigned group = gpio_irq / 32, bit = gpio_irq % 32;
    s->sysic->gpio_int_status[group] |= 1u << bit;
    qemu_irq_raise(s->sysic->gpio_irqs[group]);
}

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
            n45_button(s, N45_GPIO_BUTTON_HOME, N45_GPIO_BUTTON_HOME_IRQ, k->down);
        }
        return;
    case Q_KEY_CODE_L:
        if (!k->down || s->kbd_cmd) {
            n45_button(s, N45_GPIO_BUTTON_POWER, N45_GPIO_BUTTON_POWER_IRQ, k->down);
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

/* ---- machine ----------------------------------------------------------- */

static void n45_machine_init(MachineState *machine)
{
    IPodTouch1GMachineState *s = IPOD_TOUCH_1G_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *dev;
    SysBusDevice *busdev;

    Object *cpuobj = object_new(machine->cpu_type);
    s->cpu = ARM_CPU(cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(sysmem), &error_abort);
    object_property_set_bool(cpuobj, "has_el3", false, NULL);
    object_property_set_bool(cpuobj, "has_el2", false, NULL);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    s->nsas = cpu_get_address_space(CPU(s->cpu), ARMASIdx_NS);
    object_unref(cpuobj);

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
    allocate_ram(sysmem, "ram", N45_RAM_BASE, N45_RAM_SIZE);
    allocate_ram(sysmem, "sram1", N45_SRAM1_BASE, 0x10000);
    allocate_ram(sysmem, "vrom", N45_VROM_BASE, N45_VROM_SIZE);
    allocate_ram(sysmem, "iboot", N45_IBOOT_BASE, N45_IBOOT_SIZE);
    allocate_ram(sysmem, "llb-stubs", N45_LLB_BASE, 0x1000);
    allocate_ram(sysmem, "edgeic", N45_EDGEIC_BASE, 0x1000);
    /* Register windows the 1.x kernel touches but nothing models yet (debt). */
    allocate_ram(sysmem, "watchdog", N45_WATCHDOG_BASE, 0x10000);
    allocate_ram(sysmem, "iis0", N45_IIS0_BASE, 0x10000);
    allocate_ram(sysmem, "iis1", N45_IIS1_BASE, 0x10000);
    allocate_ram(sysmem, "iis2", N45_IIS2_BASE, 0x10000);
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
    IPOD_TOUCH_TIMER(dev)->sysclk = s->sysclk;
    memory_region_add_subregion(sysmem, N45_TIMER1_BASE, &IPOD_TOUCH_TIMER(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_TIMER1_IRQ));

    /* system controller: 7 GPIO interrupt groups */
    dev = qdev_new("ipodtouch.sysic");
    qdev_prop_set_bit(dev, "direct-boot", true);   /* no SecureROM latched the epoch */
    qdev_prop_set_uint32(dev, "epoch", 2);
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
    s->gpio = IPOD_TOUCH_GPIO(dev);
    memory_region_add_subregion(sysmem, N45_GPIO_BASE, &s->gpio->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* SDIO host, no card (the 1G's Marvell 88W8686 is not modelled) */
    dev = qdev_new("ipodtouch.sdio");
    memory_region_add_subregion(sysmem, N45_SDIO_BASE, &IPOD_TOUCH_SDIO(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* UARTs (Samsung, S5L interrupt semantics) */
    for (int i = 0; i < 5; i++) {
        static const hwaddr bases[5] = { N45_UART0_BASE, N45_UART1_BASE, N45_UART2_BASE,
                                         N45_UART3_BASE, N45_UART4_BASE };
        if (!exynos4210_uart_create(bases[i], 256, i, serial_hd(i),
                                    n45_irq(s, N45_UART0_IRQ + i), true)) {
            error_report("iPod-Touch-1G: cannot create UART%d", i);
            exit(1);
        }
    }

    /* SPI: nothing on SPI0, the LCD panel on SPI1, the Zephyr digitizer on SPI2 */
    ipod_touch_spi_create(N45_SPI0_BASE, n45_irq(s, N45_SPI0_IRQ), 0, "none", true);
    ipod_touch_spi_create(N45_SPI1_BASE, n45_irq(s, N45_SPI1_IRQ), 1, TYPE_S5L8900_LCD_PANEL, true);
    dev = ipod_touch_spi_create(N45_SPI2_BASE, n45_irq(s, N45_SPI2_IRQ), 2, "multitouch", true);
    s->mt = IPOD_TOUCH_SPI(dev)->mt;
    s->mt->sysic = s->sysic;       /* ATN straight into GPIO group 3 bit 13 */
    s->mt->gpio_state = s->gpio;

    /* CLCD, S5L8900 register layout */
    dev = qdev_new("ipodtouch.lcd");
    qdev_prop_set_bit(dev, "s5l8900", true);
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
    dev = ipod_touch_init_usb_otg(n45_irq(s, N45_USB_OTG_IRQ), (uint32_t *)s5l8900_usb_hwcfg);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, N45_USBOTG_BASE, &S5L8900USBOTG(dev)->iomem);
    dev = qdev_new("ipodtouch.usbphys");
    memory_region_add_subregion(sysmem, N45_USBPHYS_BASE, &IPOD_TOUCH_USB_PHYS(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* two PL080 DMACs */
    static const hwaddr dmac_bases[2] = { N45_DMAC0_BASE, N45_DMAC1_BASE };
    static const int dmac_irqs[2] = { N45_DMAC0_IRQ, N45_DMAC1_IRQ };
    for (int i = 0; i < 2; i++) {
        dev = qdev_new("pl080");
        object_property_set_link(OBJECT(dev), "downstream", OBJECT(sysmem), &error_fatal);
        memory_region_add_subregion(sysmem, dmac_bases[i], &PL080(dev)->iomem1);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, dmac_irqs[i]));
    }

    /* I2C0: accelerometer; I2C1: PMU */
    dev = qdev_new("ipodtouch.i2c");
    IPOD_TOUCH_I2C(dev)->base = 0;
    memory_region_add_subregion(sysmem, N45_I2C0_BASE, &IPOD_TOUCH_I2C(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_I2C0_IRQ));
    i2c_slave_create_simple(IPOD_TOUCH_I2C(dev)->bus, "lis302dl", 0x1D);

    dev = qdev_new("ipodtouch.i2c");
    IPOD_TOUCH_I2C(dev)->base = 1;
    memory_region_add_subregion(sysmem, N45_I2C1_BASE, &IPOD_TOUCH_I2C(dev)->iomem);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, n45_irq(s, N45_I2C1_IRQ));
    i2c_slave_create_simple(IPOD_TOUCH_I2C(dev)->bus, "pcf50633", 0x73);

    /* MBX ids */
    iomem = g_new(MemoryRegion, 1);
    memory_region_init_io(iomem, OBJECT(machine), &mbx_stub_ops, NULL, "mbx", 0x1000000);
    memory_region_add_subregion(sysmem, N45_MBX_BASE, iomem);

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
    object_class_property_add(klass, "tvout-workaround", "uint32", n45_get_tvout_workaround,
                              n45_set_tvout_workaround, NULL, NULL);
    object_class_property_set_description(klass, "tvout-workaround",
        "physical address of a kernel word to pin at zero for the TV-out driver (0 = off)");

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

static const TypeInfo n45_machine_info = {
    .name          = TYPE_IPOD_TOUCH_1G_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(IPodTouch1GMachineState),
    .class_init    = n45_machine_class_init,
};

static void n45_machine_types(void)
{
    type_register_static(&n45_machine_info);
}

type_init(n45_machine_types)
