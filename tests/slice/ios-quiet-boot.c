/* SLICE include/hw/arm/ios-boot-args.h file
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#define g_ascii_isspace(c) isspace((unsigned char)(c))
#include "slice.h"
int main(void)
{
    uint8_t image[512], original[512];
    memset(image, 0xa5, sizeof(image));
    memset(image + 0x18, 0, 4); /* text mode in a verbose KBoot */
    char *line = (char *)image + 0x38;
    strcpy(line, "-v rd=disk0s1\t-v debug=0x8 verbose=-v -verbose -v");
    memcpy(original, image, sizeof(image));
    assert(ios_boot_args_quiet(image, sizeof(image), 0x80000000, 0x80000000));
    assert(!strcmp(line, "   rd=disk0s1\t   debug=0x8 verbose=-v -verbose   "));
    assert(image[0x18] == 1 && image[0x19] == 0 && image[0x1a] == 0 &&
           image[0x1b] == 0);
    assert(!memcmp(image, original, 0x18));
    assert(!memcmp(image + 0x1c, original + 0x1c, 0x38 - 0x1c));
    assert(!memcmp(image + 0x138, original + 0x138, sizeof(image) - 0x138));
    assert(ios_boot_args_quiet(image, sizeof(image), 0x80000000, 0x80000000));
    strcpy(line, "rd=disk0s1");
    assert(ios_boot_args_quiet(image, 0x138, 0, 0));
    assert(!strcmp(line, "rd=disk0s1"));
    strcpy(line, "-s -v rd=md0");
    memset(image + 0x18, 0, 4);
    assert(ios_boot_args_quiet(image, sizeof(image), 0, 0));
    assert(!strcmp(line, "-s    rd=md0"));
    assert(!memcmp(image + 0x18, "\0\0\0\0",
                   4)); /* explicit single-user console */
    strcpy(line, "");
    assert(ios_boot_args_quiet(image, sizeof(image), 0, 0));
    memset(line, 'x', 256);
    memcpy(original, image, sizeof(image));
    assert(!ios_boot_args_quiet(image, sizeof(image), 0, 0));
    assert(!ios_boot_args_quiet(image, 0x137, 0, 0));
    assert(!ios_boot_args_quiet(image, sizeof(image), 1, 0));
    assert(!ios_boot_args_quiet(image, sizeof(image), 0, UINT32_MAX));
    assert(!memcmp(image, original, sizeof(image)));
    puts(
        "ARM32 quiet boot: graphics handoff, explicit single-user console, bounded flag filtering PASS");
}
