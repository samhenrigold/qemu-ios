#ifndef HW_ARM_IPOD_TOUCH_FIRMWARE_H
#define HW_ARM_IPOD_TOUCH_FIRMWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IT_KERNEL_SCAN_PA_START 0x08000000u
#define IT_KERNEL_SCAN_LEN 0x01000000u

typedef struct ITFirmwareDesc {
    const char *build;
    const char *kernel_banner;
    uint32_t pegetgmttimeofday_pa;
    uint32_t iboot_boot_args_pa;
    uint32_t amfi_slide;
    uint32_t amfi_get_task_va;
    uint32_t amfi_get_task_name_va;
    bool legacy_kernel_patches;
} ITFirmwareDesc;

const ITFirmwareDesc *it_firmware_by_build(const char *build);
const ITFirmwareDesc *it_firmware_detect_kernel(const uint8_t *image, size_t size);
/* Positive results are cached until CPU reset; an early empty RAM scan retries. */
const ITFirmwareDesc *it_firmware_loaded(void);
void it_firmware_reset(void);

#endif
