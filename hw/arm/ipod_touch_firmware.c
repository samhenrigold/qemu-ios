#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "hw/arm/ipod_touch_firmware.h"

/* Exact banners read from the decrypted 5F138 kernel and native 7E18 RAM.
 * A Darwin major version alone does not identify addresses in a kernel. */
static const ITFirmwareDesc profiles[] = {
    {
        .build = "5F138",
        .kernel_banner = "Darwin Kernel Version 9.4.1: Sun Aug 10 21:25:25 PDT 2008; root:xnu-1228.7.27~12/RELEASE_ARM_S5L8720X",
        .pegetgmttimeofday_pa = 0x0816b460u,
        .iboot_boot_args_pa = 0x0ff2a584u,
        .legacy_kernel_patches = true,
    }, {
        .build = "7E18",
        .kernel_banner = "Darwin Kernel Version 10.0.0d3: Fri Dec 18 01:31:23 PST 2009; root:xnu-1357.5.30~6/RELEASE_ARM_S5L8720X",
        .pegetgmttimeofday_pa = 0x081953e0u,
        .amfi_slide = 0xb8000000u,
        .amfi_get_task_va = 0xc01ab200u,
        .amfi_get_task_name_va = 0xc01ab2a0u,
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
