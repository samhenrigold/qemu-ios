/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_IOS_BOOT_ARGS_H
#define HW_ARM_IOS_BOOT_ARGS_H

/* ARM32 iBoot boot_args: video.v_display at +0x18; command line at +0x38,
 * 256 bytes. Quiet the staged RAM handoff, never the source image. */
static inline bool ios_boot_args_quiet(uint8_t *image, size_t length,
                                       uint32_t load, uint32_t arguments)
{
    uint64_t offset;
    char *line, *end;
    bool single_user = false;

    if (arguments < load) {
        return false;
    }
    offset = (uint64_t)arguments - load;
    if (offset + 0x38 + 256 > length) {
        return false;
    }
    line = (char *)image + offset + 0x38;
    end = memchr(line, 0, 256);
    if (!end) {
        return false;
    }
    for (char *p = line; p < end;) {
        char *start;
        while (p < end && g_ascii_isspace(*p)) {
            p++;
        }
        start = p;
        while (p < end && !g_ascii_isspace(*p)) {
            p++;
        }
        if (p - start == 2 && !memcmp(start, "-v", 2)) {
            memset(start, ' ', 2);
        } else if (p - start == 2 && !memcmp(start, "-s", 2)) {
            single_user = true;
        }
    }
    if (!single_user) {
        /* A bundle built with -v also carries TEXT_MODE here. Removing only
         * the token leaves XNU's framebuffer console/caret active. Supply
         * GRAPHICS_MODE as iBoot does for a normal graphical boot. */
        image[offset + 0x18] = 1;
        memset(image + offset + 0x19, 0, 3);
    }
    return true;
}
#endif
