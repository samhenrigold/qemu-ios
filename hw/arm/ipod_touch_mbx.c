#include "hw/arm/ipod_touch_mbx.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "cpu.h"

/*
 * MMIO trace, off unless MBX_TRACE=1 is in the emulator's environment.
 *
 * The guest PC is logged with every access: AppleMBX.kext is carved out at
 * /Users/shg/Developer/ipod2g-re/kexts/com.apple.driver.AppleMBX.macho with
 * __text at 0xc04e4000, so a PC in that range maps straight onto a disassembly
 * line and tells you which driver routine touched the register.
 */
static int mbx_trace = -1;

static bool mbx_tracing(void)
{
    if (mbx_trace < 0) {
        const char *v = getenv("MBX_TRACE");
        mbx_trace = (v && *v && *v != '0') ? 1 : 0;
    }
    return mbx_trace == 1;
}

static uint32_t mbx_guest_pc(void)
{
    if (!current_cpu) {
        return 0;
    }
    return (uint32_t)ARM_CPU(current_cpu)->env.regs[15];
}

#define MBX_TRACE(fmt, ...)                                                    \
    do {                                                                       \
        if (mbx_tracing()) {                                                    \
            fprintf(stderr, "[MBX] pc=0x%08x " fmt "\n", mbx_guest_pc(),        \
                    ##__VA_ARGS__);                                             \
        }                                                                      \
    } while (0)

/*
 * Completion shim for the unemulated MBX.
 *
 * The GPU itself is not emulated and is out of scope -- the MBX Lite command
 * format is proprietary and essentially un-reverse-engineered. But the hang it
 * causes is not a rendering problem: completion is interrupt-driven, and the
 * machine wired no interrupt to the MBX at all. An app that submits work then
 * sleeps waiting for the completion interrupt never wakes.
 *
 * Evidence: tracing every MBX access while launching a real OpenGL ES app
 * (AwesomeBall) shows 34 accesses ending in a submission write to 0x130, and
 * then silence -- the guest is blocked, not polling.
 *
 * So raise the interrupt shortly after a submission and let the driver's
 * handler run. The interrupt number is 0x35, taken from the mbx node's
 * "interrupts" property in a real device's ioreg dump.
 *
 * This cannot make anything render. The intent is only that the guest stops
 * waiting forever, so the UI survives an app that touches the GPU.
 */
/*
 * What the driver actually does with these registers, read out of
 * com.apple.driver.AppleMBX (__text at 0xc04e4000).
 *
 *   0x130  interrupt MASK/enable. The driver writes 0x867c and caches the same
 *          value at this->0x140 (c04e8988-c04e89a4). It is not a doorbell, so
 *          arming a completion off a write here fires off the driver's power-on
 *          sequence, not off a frame.
 *   0x12c  interrupt STATUS. Bit 0x40 also doubles as "idle": c04e8624 spins on
 *          it before programming the engine.
 *   0x134  write-1-to-clear for 0x12c. c04e8998 writes 0x7ff to clear all.
 *
 * The ISR is at c04ea478:
 *
 *   if (this->0x214 != 2) return;                  <-- bails with no MMIO at all
 *   pending = [0x12c] & this->0x140;
 *   [0x134] = pending;
 *   ... 0x400 -> TA overflow, 0x10/0x20/0x200 -> state machine ...
 *   0x4 -> this->0x13c,  0x8 -> this->0x13d,  0x40 -> this->0x13e
 *   if (0x13c && 0x13d && 0x13e) -> frame done
 *   if (anything handled) bl c04e9b00               <-- queue processor
 *
 * and c04e9b00 eventually reaches c04e9898, which does
 *
 *   this->0x208 = completedCommand->0x48;
 *   commandGate->commandWakeup(&this->0x208);
 *
 * waking waitForTimeStamp (c04e8058), which commandSleeps on &this->0x208 until
 * it reaches the requested timestamp. So a faithful completion has to raise the
 * interrupt with 0x4|0x8|0x40 set in 0x12c *while* 0x130 is armed and the device
 * state this->0x214 is 2. The current shim raises it 1ms later, by which time
 * the driver's idle path (c04e5cc0) has written 0x130 <- 0 and this->0x214 <- 0,
 * so the ISR returns immediately -- which is exactly the observed "interrupt
 * fires, guest ignores it, zero MBX accesses".
 */
#define MBX_MMU_CTRL_REG 0x1020
#define MBX_MMU_ENABLE   0x00000001   /* driver's request */
#define MBX_MMU_ACK      0x00010000   /* hardware's acknowledgement */
#define MBX_SUBMIT_REG   0x130        /* interrupt mask, per the notes above */
#define MBX_STATUS_REG   0x12c
#define MBX_INTCLR_REG   0x134        /* write-1-to-clear for 0x12c */

/* 0x4 | 0x8 | 0x40: the three the ISR folds into "frame done". */
#define MBX_INT_FRAME_DONE 0x4c

/*
 * How often to post a completion while the driver's mask is armed. The ISR
 * bails out unless the device object is in its running state, so a completion
 * posted at the wrong moment is simply dropped; repeating means the driver
 * eventually sees one once it is ready. 16 ms is roughly a frame and keeps the
 * interrupt rate far too low to storm.
 */
#define MBX_COMPLETE_PERIOD_NS (16 * 1000 * 1000)

static uint32_t reverse_byte_order(uint32_t value) {
    return ((value & 0x000000FF) << 24) |
           ((value & 0x0000FF00) << 8) |
           ((value & 0x00FF0000) >> 8) |
           ((value & 0xFF000000) >> 24);
}

static uint64_t ipod_touch_mbx1_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;
    uint32_t val;

    switch(addr)
    {
        case MBX_STATUS_REG:
            /* 0x40 was pinned here unconditionally; keep that as the base so
             * behaviour is unchanged when the shim is off. */
            val = 0x40 | s->status;
            if (s->irq_enabled && s->irq) {
                /* Reading the status acknowledges the completion. */
                s->status = 0;
                qemu_irq_lower(s->irq);
            }
            break;
        case 0xf00:
            val = (2 << 0x10) | (1 << 0x18); // seems to be some kind of identifier
            break;
        case MBX_MMU_CTRL_REG:
            /*
             * Bit 0 is the driver's enable *request*; bit 16 is the hardware's
             * *acknowledgement*. AppleMBXMMU drives them as a handshake:
             *
             *   enable  (0xc04ee224): set bit 0,   spin until bit 16 sets
             *   disable (0xc04ee770): clear bit 0, spin until bit 16 clears
             *
             * Echoing writes back unchanged satisfies the enable loop by
             * accident -- writing 1 to bit 0 leaves the stale bit 16 set -- but
             * makes the disable loop spin forever, because nothing ever clears
             * bit 16. That is the real hang behind "OpenGL ES apps freeze":
             * AwesomeBall tears the MMU down on startup and never comes back,
             * burning ~21M reads of this one register. It is not the completion
             * interrupt at all.
             *
             * So acknowledge the request: mirror bit 0 into bit 16.
             */
            val = s->addr;
            if (s->irq_enabled) {
                val = (val & ~MBX_MMU_ACK) | ((val & MBX_MMU_ENABLE) ? MBX_MMU_ACK : 0);
            }
            if (val == 0) {
                val = MBX_MMU_ACK; /* never written: as before, report ready */
            }
            break;
        default:
            val = 0;
            break;
    }
    MBX_TRACE("mbx1 rd  [0x%06x] -> 0x%08x", (uint32_t)addr, val);
    return val;
}

static void ipod_touch_mbx1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;
    MBX_TRACE("mbx1 wr  [0x%06x] <- 0x%08x", (uint32_t)addr, (uint32_t)val);

    switch(addr)
    {
	case MBX_MMU_CTRL_REG:
	    s->addr = val;
	    break;
	case MBX_SUBMIT_REG:
	    if (s->complete_shim) {
	        s->int_mask = val;
	        if (val) {
	            timer_mod(s->complete_timer,
	                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MBX_COMPLETE_PERIOD_NS);
	        } else {
	            timer_del(s->complete_timer);
	            s->status = 0;
	            if (s->irq) {
	                qemu_irq_lower(s->irq);
	            }
	        }
	    }
	    break;
	case MBX_INTCLR_REG:
	    if (s->complete_shim) {
	        s->status &= ~(uint32_t)val;
	        if (!s->status && s->irq) {
	            qemu_irq_lower(s->irq);
	        }
	    }
	    break;
    }
}

/* Post a completion while the driver's interrupt mask is armed. */
static void ipod_touch_mbx_complete(void *opaque)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;

    if (!s->int_mask) {
        return;
    }
    s->status |= MBX_INT_FRAME_DONE & s->int_mask;
    if (s->status && s->irq) {
        qemu_irq_raise(s->irq);
    }
    timer_mod(s->complete_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MBX_COMPLETE_PERIOD_NS);
}

/*
 * Let the USB device stack go on bus even though the PTP interface function
 * never gets a driver.
 *
 * IOUSBDeviceController::handleUSBCableConnect refuses to bring the controller
 * up until every interface function declared by SetDeviceDescription has
 * registered. The descriptors come from
 * /System/Library/AppleUSBDevice/USBDeviceConfiguration.plist, pushed by the
 * configd plug-in com.apple.configd.usbdeviceconfig, and for iPod2,1 they name
 * five functions. Four are served by in-kernel drivers (USBAudioControl,
 * USBAudioStreaming, IapOverUsbHid, AppleUSBMux). PTP is served by the userland
 * daemon /usr/libexec/ptpd, which does not come up here -- so PTP acquires no
 * alternate setting, emits no interface descriptor despite being counted in
 * bNumInterfaces, and its name never leaves the pending set. The count floors at
 * exactly one and handleUSBCableConnect is never re-driven, so the controller is
 * never touched at all.
 *
 * gated_registerFunction removes the caller from the set, then:
 *     subs sl, r0, #0     ; r0 = set->getCount()
 *     bne  <return>       ; still waiting
 * Rewriting the compare as "- #1" makes it proceed when exactly one function
 * (PTP) is left, on the last real registration. sl stays 0 on that path, which
 * matters: it is stored back as the "set is empty" marker and reused as the
 * configuration loop index.
 *
 * The site is located at run time rather than hardcoded, anchored on a log
 * string, so this does not depend on one firmware build's addresses:
 *
 *     cstring "all functions registered"
 *       -> the literal-pool word holding its address
 *       -> the ldr rX, [pc, #imm] that loads that word
 *       -> backwards to the subs rN, r0, #0 / bne pair guarding it
 *
 * Verified to derive 0xc05d45cc on 2.1.1 / build 5F138. Only that one build was
 * available to check, so the approach is portable in principle but unproven on a
 * second image.
 */
#define KERNEL_VA_TO_PA(va)   ((va) - 0xb8000000u)
#define KERNEL_SCAN_PA_START  0x08000000u
#define KERNEL_SCAN_LEN       0x01000000u   /* 16 MiB covers the whole kernelcache */

static bool patch_usb_gate_enabled;

void ipod_touch_mbx_set_patch_usb_gate(bool enabled)
{
    patch_usb_gate_enabled = enabled;
}

static uint32_t kernel_read_word(uint32_t va)
{
    uint32_t w = 0;
    cpu_physical_memory_read(KERNEL_VA_TO_PA(va), (uint8_t *)&w, sizeof(w));
    return w;
}

/* VA of the NUL-terminated string containing needle, or 0. */
static uint32_t kernel_find_cstring(const uint8_t *image, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);

    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(image + i, needle, nlen) != 0) {
            continue;
        }
        /* Back up to just past the preceding NUL - the literal pool points at
         * the start of the string, not at our substring. */
        size_t start = i;
        while (start > 0 && image[start - 1] != 0) {
            start--;
        }
        return KERNEL_SCAN_PA_START + start + 0xb8000000u;
    }
    return 0;
}

/*
 * Per-build constants.
 *
 * Almost nothing in this machine is firmware-specific: the AES engine keys its
 * fake GID blobs on the img3 KBAG ciphertext, the USB mux gate locates itself
 * from an anchor string, and the iBoot bluetooth-node poke searches for its
 * string. What is left is a single kernel address that cannot be derived
 * without a symbol lookup, so it is tabulated per build and selected by the
 * kernel's own version banner.
 */
typedef struct {
    const char *build;
    const char *version_needle;   /* substring of the Darwin version banner */
    uint32_t pegetgmttimeofday_pa;
} ITFirmwareDesc;

static const ITFirmwareDesc it_firmware_descs[] = {
    /* iPhone OS 2.1.1, xnu-1228.  VA 0xc016b460 */
    { "5F138", "Darwin Kernel Version 9.",  0x0816b460u },
    /* iOS 3.1.3, xnu-1357.  VA 0xc01953e0 */
    { "7E18",  "Darwin Kernel Version 10.", 0x081953e0u },
};

/* Identify the loaded kernelcache. Returns NULL if it is not one we know. */
static const ITFirmwareDesc *it_firmware_desc(void)
{
    static const ITFirmwareDesc *cached;
    static bool searched;

    if (searched) {
        return cached;
    }
    searched = true;

    g_autofree uint8_t *image = g_try_malloc(KERNEL_SCAN_LEN);
    if (!image) {
        return NULL;
    }
    cpu_physical_memory_read(KERNEL_SCAN_PA_START, image, KERNEL_SCAN_LEN);

    for (size_t d = 0; d < ARRAY_SIZE(it_firmware_descs); d++) {
        const ITFirmwareDesc *desc = &it_firmware_descs[d];
        if (kernel_find_cstring(image, KERNEL_SCAN_LEN, desc->version_needle)) {
            printf("[FIRMWARE] detected build %s\n", desc->build);
            cached = desc;
            return cached;
        }
    }

    printf("[FIRMWARE] unrecognised kernelcache; build-specific patches skipped\n");
    return NULL;
}

static void patch_usb_function_gate(void)
{
    if (!patch_usb_gate_enabled) {
        return;
    }

    uint8_t *image = g_try_malloc(KERNEL_SCAN_LEN);
    if (!image) {
        printf("[USBGATE] could not allocate scan buffer\n");
        return;
    }
    cpu_physical_memory_read(KERNEL_SCAN_PA_START, image, KERNEL_SCAN_LEN);

    uint32_t str_va = kernel_find_cstring(image, KERNEL_SCAN_LEN,
                                          "all functions registered");
    if (!str_va) {
        printf("[USBGATE] anchor string not found; not patching\n");
        g_free(image);
        return;
    }

    /* Literal-pool slots holding that address. */
    uint32_t patched_at = 0;
    for (size_t i = 0; i + 4 <= KERNEL_SCAN_LEN && !patched_at; i += 4) {
        uint32_t word = ldl_le_p(image + i);
        if (word != str_va) {
            continue;
        }
        uint32_t pool_va = KERNEL_SCAN_PA_START + i + 0xb8000000u;

        /* The ldr rX, [pc, #imm] that loads it. ARM literal loads resolve
         * against pc+8, and bit 23 is the add/subtract flag so it stays in the
         * mask. */
        uint32_t ldr_va = 0;
        for (uint32_t back = 8; back < 4096 && !ldr_va; back += 4) {
            uint32_t va = pool_va - 8 - back;
            uint32_t w = kernel_read_word(va);
            if ((w & 0x0fff0000u) == 0x059f0000u && va + 8 + (w & 0xfff) == pool_va) {
                ldr_va = va;
            }
        }
        if (!ldr_va) {
            continue;
        }

        /* Backwards to the guarding "subs rN, r0, #0" followed by a bne. */
        for (uint32_t back = 4; back < 80; back += 4) {
            uint32_t va = ldr_va - back;
            uint32_t w = kernel_read_word(va);
            if ((w & 0xfff00fffu) != 0xe2500000u) {
                continue;
            }
            uint32_t next = kernel_read_word(va + 4);
            bool is_bne = (next & 0x0f000000u) == 0x0a000000u && (next >> 28) == 0x1;
            if (!is_bne) {
                continue;
            }
            uint32_t patched = w | 1;
            cpu_physical_memory_write(KERNEL_VA_TO_PA(va), (uint8_t *)&patched,
                                      sizeof(patched));
            printf("[USBGATE] patched gated_registerFunction count check at "
                   "0x%08x (0x%08x -> 0x%08x)\n", va, w, patched);
            patched_at = va;
            break;
        }
    }

    if (!patched_at) {
        printf("[USBGATE] could not locate the count check; not patching\n");
    }
    g_free(image);
}

static void patch_kernel(bool *alreadypatched)
{
    if (*alreadypatched) return;
    *alreadypatched = true;

    /*
     * Give the guest a real clock. The 2G has no RTC iOS reads at boot, so the
     * calendar starts at 1900. Rewrite _PEGetGMTTimeOfDay's head to read host
     * UTC seconds from the HOST_GMT_SECONDS cp15 reg and return:
     *     mrc p15, 3, r0, c15, c15, #1   ; 7f ee 3f 0f
     *     bx  lr                         ; 70 47
     * XNU seeds clock_initialize_calendar from r0 once; its tick advances after.
     * (If the date is still wrong, this MBX-triggered patch point ran after the
     * calendar was already seeded and the patch needs an earlier trigger.)
     *
     * The function's address is the one genuinely build-specific constant left
     * in this file, so it comes from the firmware descriptor picked below.
     */
    const ITFirmwareDesc *fw = it_firmware_desc();
    if (fw) {
        static const uint8_t gmt_patch[6] = {0x7f, 0xee, 0x3f, 0x0f, 0x70, 0x47};
        cpu_physical_memory_write(fw->pegetgmttimeofday_pa, gmt_patch,
                                  sizeof(gmt_patch));
    }

    patch_usb_function_gate();

    // patch the loading of the AppleBCM4325 driver
    char *bcm4325_vars = "test";

    // write the pointer to our custom subroutine
    uint32_t *data = malloc(sizeof(uint32_t) * 200);
    data[0] = 0xC0460000;
    cpu_physical_memory_write(0x8324aa8, (uint8_t *)data, sizeof(uint32_t) * 1);

    // create the call to the subroutine
    data = malloc(sizeof(uint32_t) * 200);
    data[0] = reverse_byte_order(0x0640A0E1); // mov r4, r6
    data[1] = reverse_byte_order(0x9C309FE5); // ldr r3, [pc, #0x9c]
    data[2] = reverse_byte_order(0x33FF2FE1); // blx r3
    data[3] = reverse_byte_order(0x00F020E3); // NOP
    data[4] = reverse_byte_order(0x00F020E3); // NOP
    data[5] = reverse_byte_order(0x00F020E3); // NOP
    cpu_physical_memory_write(0x8324a00, (uint8_t *)data, sizeof(uint32_t) * 6);

    // fill in the driver load subroutine
    data = malloc(sizeof(uint32_t) * 200);
    data[0] = reverse_byte_order(0xFE402DE9); // push on stack

    // TODO I should clean this up
    for(int i = 1; i < 21; i++) { data[i] = reverse_byte_order(0x00F020E3); } // NOP

    // call the IONetworkController metaclass initialization
    data[21] = reverse_byte_order(0x0100B0E3); // movs r0, #0x1
    data[22] = reverse_byte_order(0xB8109FE5); // ldr r1, [pc, #0xb8]
    data[23] = reverse_byte_order(0xB8209FE5); // ldr r2, [pc, #0xb8]
    data[24] = reverse_byte_order(0x32FF2FE1); // blx r2

    // load the "com.apple.driver.AppleBCM4325" kext
    data[25] = reverse_byte_order(0xB4009FE5); // ldr r0, [pc, #0xb4]
    data[26] = reverse_byte_order(0x0110B0E3); // movs r1, #0x1
    data[27] = reverse_byte_order(0xB0209FE5); // ldr r2, [pc, #0xd8]
    data[28] = reverse_byte_order(0x32FF2FE1); // blx r2

    data[29] = reverse_byte_order(0xFE80BDE8); // pop from stack

    cpu_physical_memory_write(0x8460000, (uint8_t *)data, sizeof(uint32_t) * 50);

    // write the data section of the driver load subroutine (0x100 items from the start of the subroutine)
    data = malloc(sizeof(uint32_t) * 200);
    data[0] = 0xc0460200; // the address of the BCM4325Vars string
    data[1] = 0xc013c373; // the address of OSData::withBytes
    data[2] = 0xc013cc3d; // the address of OSDictionary::withCapacity
    data[3] = 0xc03467bc; // the "BCM4325Vars" string
    data[4] = 0xc013ad8d; // the address of OSObject::operator.new
    data[5] = 0xc032c294; // the object initialization method of AppleBCM4325
    data[6] = 0xffff; // the 2nd parameter for the call to the IONetworkController metaclass initialization
    data[7] = 0xc02f94f9; // the initialization method of the IONetworkController metaclass
    data[8] = 0xc038a320; // the "com.apple.driver.AppleBCM4325" string
    data[9] = 0xc015de01; // the kmod_load_request method

    cpu_physical_memory_write(0x8460100, (uint8_t *)data, sizeof(uint32_t) * 10);
}

static uint64_t ipod_touch_mbx2_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;
    uint32_t val = 0;

    switch(addr)
    {
        case 0xC:
            patch_kernel(&s->alreadypatched);
	    break;
	case 0x4:
	    val = 0xFF;
	    break;
        default:
            break;
    }
    MBX_TRACE("mbx2 rd  [0x%03x] -> 0x%08x", (uint32_t)addr, val);
    return val;
}

static void ipod_touch_mbx2_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MBX_TRACE("mbx2 wr  [0x%03x] <- 0x%08x", (uint32_t)addr, (uint32_t)val);
}

static const MemoryRegionOps ipod_touch_mbx1_ops = {
    .read = ipod_touch_mbx1_read,
    .write = ipod_touch_mbx1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps ipod_touch_mbx2_ops = {
    .read = ipod_touch_mbx2_read,
    .write = ipod_touch_mbx2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_mbx_init(Object *obj)
{
    IPodTouchMBXState *s = IPOD_TOUCH_MBX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /*
     * IT_MBX_RAM: back the 16 MB MBX aperture with real RAM instead of pure
     * MMIO. 3.1.3's AppleMBX builds an IOMemoryDescriptor for its surfaces and
     * calls prepare() (which wires physical page frames); over an init_io
     * region there are no page frames, and the driver logs
     *   "MBX: Failed to add a surface mapping because prepare() failed ..."
     * so no surface is ever mapped and nothing composites to the framebuffer.
     * Bring-up probe: RAM makes the aperture wireable, at the cost of the
     * register request/ack mirror on this bank.
     */
    if (getenv("IT_MBX_RAM")) {
        memory_region_init_ram(&s->iomem1, obj, "mbx1_ram", 0x1000000, &error_fatal);
    } else {
        memory_region_init_io(&s->iomem1, obj, &ipod_touch_mbx1_ops, s, TYPE_IPOD_TOUCH_MBX, 0x1000000);
    }
    sysbus_init_mmio(sbd, &s->iomem1);
    memory_region_init_io(&s->iomem2, obj, &ipod_touch_mbx2_ops, s, TYPE_IPOD_TOUCH_MBX, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem2);

    sysbus_init_irq(sbd, &s->irq);

    s->complete_shim = getenv("IT_MBX_COMPLETE") != NULL;
    if (s->complete_shim) {
        s->irq_enabled = true;
        s->complete_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         ipod_touch_mbx_complete, s);
    }
}

/*
 * Warm resets have to clear the MMU handshake.
 *
 * addr backs register 0x1020, whose bit 0 is the driver's MMU enable request.
 * The guest clears it on the way down, so without a reset the next boot starts
 * with the MMU marked disabled and AppleMBXDevice initialises against the
 * previous boot's state. That matters beyond the GPU: CoreSurface uses the MBX
 * as its swap device ("AppleMBX: Using AppleM2CLCD as legacy swap device"), so
 * a half-initialised MBX stops SpringBoard ever programming its framebuffer
 * into the display controller -- the panel stays on the boot logo even though
 * SpringBoard is running and has attached to IOMobileFramebuffer.
 *
 * alreadypatched must be cleared too: the USB gate patch is applied to kernel
 * memory that a reset reloads, so it has to be re-applied on the next boot.
 */
static void ipod_touch_mbx_reset(DeviceState *dev)
{
    IPodTouchMBXState *s = IPOD_TOUCH_MBX(dev);

    s->addr = 0;
    s->status = 0;
    s->alreadypatched = false;
    if (s->irq) {
        qemu_irq_lower(s->irq);
    }
}

static void ipod_touch_mbx_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = ipod_touch_mbx_reset;
}

static const TypeInfo ipod_touch_mbx_type_info = {
    .name = TYPE_IPOD_TOUCH_MBX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMBXState),
    .instance_init = ipod_touch_mbx_init,
    .class_init = ipod_touch_mbx_class_init,
};

static void ipod_touch_mbx_register_types(void)
{
    type_register_static(&ipod_touch_mbx_type_info);
}

type_init(ipod_touch_mbx_register_types)
