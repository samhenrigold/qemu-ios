#include "hw/arm/ipod_touch_mbx.h"
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
#define MBX_SUBMIT_REG   0x130
#define MBX_STATUS_REG   0x12c
#define MBX_COMPLETE_NS  (1 * 1000 * 1000)  /* 1ms; the real thing is async */

static void mbx_complete(void *opaque)
{
    IPodTouchMBXState *s = (IPodTouchMBXState *)opaque;

    s->status |= 0x40;
    if (s->irq) {
        qemu_irq_raise(s->irq);
    }
    MBX_TRACE("completion fired, status=0x%08x", s->status);
}

static void mbx_submit(IPodTouchMBXState *s)
{
    if (!s->irq_enabled || !s->done_timer) {
        return;
    }
    timer_mod(s->done_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MBX_COMPLETE_NS);
}

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
        case 0x1020:
            val = s->addr != 0x0 ? s->addr : 0x10000;
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
	case 0x1020:
	    s->addr = val;
	    break;
	case MBX_SUBMIT_REG:
	    /* A non-zero write here is a submission; zero is the teardown that
	     * follows it. Only the submission arms the completion. */
	    if (val) {
	        mbx_submit(s);
	    }
	    break;
    }
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

static void patch_kernel(bool alreadypatched)
{
    if (alreadypatched) return;
    	alreadypatched = 1;

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
            patch_kernel(s->alreadypatched);
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

    memory_region_init_io(&s->iomem1, obj, &ipod_touch_mbx1_ops, s, TYPE_IPOD_TOUCH_MBX, 0x1000000);
    sysbus_init_mmio(sbd, &s->iomem1);
    memory_region_init_io(&s->iomem2, obj, &ipod_touch_mbx2_ops, s, TYPE_IPOD_TOUCH_MBX, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem2);

    sysbus_init_irq(sbd, &s->irq);
    s->done_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mbx_complete, s);

}

static void ipod_touch_mbx_class_init(ObjectClass *klass, void *data)
{
    
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
