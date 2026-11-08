/* Rendered camera Y/UV planes must retain their shader channels and row
 * density; copying a BGRA prefix into L8/LA88 shrinks and corrupts the frame.
 * SLICE include/hw/arm/guest-services/gles.h define GLES_SURFACE_
 * SLICE hw/arm/gles-host.c fn gles_surface_pack_mono
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "slice.h"
int main(void)
{
    const uint8_t input[] = {11, 22, 33, 44, 55, 66, 77, 88};
    uint8_t pixels[sizeof(input)];
    memcpy(pixels, input, sizeof(input));
    assert(gles_surface_pack_mono(GLES_SURFACE_L8, pixels, 2));
    assert(pixels[0] == 33 && pixels[1] == 77);
    assert(!memcmp(pixels + 2, input + 2, 6));
    memcpy(pixels, input, sizeof(input));
    assert(gles_surface_pack_mono(GLES_SURFACE_A8, pixels, 2));
    assert(pixels[0] == 44 && pixels[1] == 88);
    const uint32_t formats[] = {GLES_SURFACE_LA88, GLES_SURFACE_LA88_IOS7,
                                GLES_SURFACE_LA8_IOS10};
    const uint8_t expected[] = {33, 44, 77, 88};
    for (unsigned i = 0; i < 3; i++) {
        memcpy(pixels, input, sizeof(input));
        assert(gles_surface_pack_mono(formats[i], pixels, 2));
        assert(!memcmp(pixels, expected, sizeof(expected)));
        assert(!memcmp(pixels + 4, input + 4, 4));
    }
    memcpy(pixels, input, sizeof(input));
    assert(!gles_surface_pack_mono(0, pixels, 2));
    assert(!memcmp(pixels, input, sizeof(input)));
    puts("ok");
}
