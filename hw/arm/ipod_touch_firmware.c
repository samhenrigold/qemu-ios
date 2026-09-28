#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "hw/arm/ipod_touch_firmware.h"

/* Exact banners read from the decrypted 5F138 kernel and native 7E18 RAM.
 * A Darwin major version alone does not identify addresses in a kernel. */
static const ITFirmwareDesc profiles[] = {
    {
        .build = "5F138",
        .kernel_banner = "Darwin Kernel Version 9.4.1: Sun Aug 10 21:25:25 PDT 2008; root:xnu-1228.7.27~12/RELEASE_ARM_S5L8720X",
        .legacy_kernel_patches = true,
    }, {
        .build = "7E18",
        .kernel_banner = "Darwin Kernel Version 10.0.0d3: Fri Dec 18 01:31:23 PST 2009; root:xnu-1357.5.30~6/RELEASE_ARM_S5L8720X",
    },
};

static const ITFirmwareDesc *loaded;

const ITFirmwareDesc *it_firmware_by_build(const char *build)
{
    if (build) {
        for (size_t i = 0; i < ARRAY_SIZE(profiles); i++) {
            if (!strcmp(build, profiles[i].build)) {
                return &profiles[i];
            }
        }
    }
    return NULL;
}

const ITFirmwareDesc *it_firmware_detect_kernel(const uint8_t *image, size_t size)
{
    const ITFirmwareDesc *found = NULL;
    if (!image) {
        return NULL;
    }
    for (size_t offset = 0; offset < size; offset++) {
        if (image[offset] != 'D') {
            continue;
        }
        for (size_t i = 0; i < ARRAY_SIZE(profiles); i++) {
            size_t len = strlen(profiles[i].kernel_banner) + 1;
            if (len <= size - offset &&
                !memcmp(image + offset, profiles[i].kernel_banner, len)) {
                if (found && found != &profiles[i]) {
                    return NULL; /* Ambiguous memory is not a patch target. */
                }
                found = &profiles[i];
            }
        }
    }
    return found;
}

const ITFirmwareDesc *it_firmware_loaded(void)
{
    if (!loaded) {
        uint8_t *image = g_try_malloc(IT_KERNEL_SCAN_LEN);
        if (!image) {
            return NULL;
        }
        cpu_physical_memory_read(IT_KERNEL_SCAN_PA_START, image, IT_KERNEL_SCAN_LEN);
        loaded = it_firmware_detect_kernel(image, IT_KERNEL_SCAN_LEN);
        g_free(image);
        if (loaded) {
            printf("[FIRMWARE] detected build %s\n", loaded->build);
        }
    }
    return loaded;
}

void it_firmware_reset(void)
{
    loaded = NULL;
}

/* Locate the command-line data passed to iBoot's diagnostic printf. The
 * Thumb literal loads identify both the format and the buffer; no guest code
 * is modified. Reject absent, ambiguous or out-of-window references. */
static uint16_t iboot_u16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t iboot_u32(const uint8_t *p)
{
    return iboot_u16(p) | (uint32_t)iboot_u16(p + 2) << 16;
}

uint32_t it_firmware_find_iboot_command_line(const uint8_t *image, size_t size,
                                           uint32_t base)
{
    static const char format[] = "gBootArgs.commandLine = [%s]\n";
    uint32_t found = 0;
    if (!image || size < sizeof(format) || size > UINT32_MAX - base || (base & 3)) {
        return 0;
    }
    for (size_t i = 0; i + 10 <= size; i += 2) {
        /* ldr r4, literal; ldr r0, literal; adds r1, r4, #0; bl printf */
        uint16_t buffer_load = iboot_u16(image + i);
        uint16_t format_load = iboot_u16(image + i + 2);
        if ((buffer_load & 0xff00) != 0x4c00 ||
            (format_load & 0xff00) != 0x4800 ||
            iboot_u16(image + i + 4) != 0x1c21 ||
            (iboot_u16(image + i + 6) & 0xf800) != 0xf000 ||
            (iboot_u16(image + i + 8) & 0xf800) != 0xf800) {
            continue;
        }
        size_t bp = ((i + 4) & ~(size_t)3) + (buffer_load & 255) * 4;
        size_t fp = ((i + 6) & ~(size_t)3) + (format_load & 255) * 4;
        if (bp > size - 4 || fp > size - 4) {
            continue;
        }
        uint32_t buffer = iboot_u32(image + bp);
        uint32_t text = iboot_u32(image + fp);
        if (text < base || text - base > size - sizeof(format) ||
            memcmp(image + (text - base), format, sizeof(format)) ||
            size < 256 || buffer < base || (buffer & 3) ||
            buffer - base > size - 256) {
            continue;
        }
        if (found) {
            return 0;
        }
        found = buffer;
    }
    return found;
}
