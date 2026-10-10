/* Where the VXD finds a slice's NAL in the guest's bitstream buffer (vxd_nal_start) and whether the next one goes on
 * with the same picture (vxd_more_slices): a sample as 4.x stores it (a 4-byte length before each NAL) and as 3.1.3's
 * AppleVXD375Framework hands it over (Annex B, a start code before each NAL), the DMA starting past the NAL's header.
 * The Annex B bytes are the 3GS's first two slices of N.O.V.A.'s logo movie on 3.1.3.
 *
 * SLICE hw/arm/s5l8920_vxd.c fn vxd_slice_nal vxd_nal_start vxd_more_slices
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int ldl_be_p(const void *p)
{
    const uint8_t *b = p;
    return (int)((uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
}
#include "slice.h"

int main(void)
{
    bool annexb;

    /* Annex B: the slice before ends (7b c7), a 4-byte start code, the IDR slice's header (25) and three more header
     * bytes, then the DMA's first bytes. 10 bytes before the DMA, 8 of it. */
    static const uint8_t annexb4[] = { 0x7b, 0xc7, 0, 0, 0, 1, 0x25, 0xb8, 0x20, 0x20,
                                       0xbf, 0xff, 0xfc, 0x3d, 0x14, 0x00, 0x04, 0x10 };
    assert(vxd_nal_start(annexb4, 10, 8, &annexb) == 6 && annexb);
    /* A 3-byte start code works the same. */
    static const uint8_t annexb3[] = { 0xc7, 0, 0, 1, 0x21, 0x9a, 0x11, 0x22, 0x33, 0x44 };
    assert(vxd_nal_start(annexb3, 6, 4, &annexb) == 4 && annexb);
    /* The nearest start code before the DMA is a parameter set's: no slice. */
    static const uint8_t sps[] = { 0, 0, 1, 0x67, 0x42, 0x00, 0x1e, 0x11 };
    assert(vxd_nal_start(sps, 6, 2, &annexb) == -1);

    /* A sample: 00 00 00 0a is the NAL's length (its header and 9 bytes); the DMA starts 2 bytes in. */
    static const uint8_t mp4[] = { 0x99, 0, 0, 0, 0x0a, 0x65, 0x88, 1, 2, 3, 4, 5, 6, 7, 8 };
    assert(vxd_nal_start(mp4, 7, 8, &annexb) == 5 && !annexb);

    /* What follows a slice: another slice of the picture (first_mb_in_slice > 0: first bit 0), a new picture's (first
     * bit 1), or something else. In a sample 00 00 01 20 is a length (288 bytes), not a start code. */
    static const uint8_t more_b4[6] = { 0, 0, 0, 1, 0x25, 0x00 }, more_b3[6] = { 0, 0, 1, 0x21, 0x40, 0 };
    static const uint8_t new_b3[6] = { 0, 0, 1, 0x21, 0x9a, 0 };
    static const uint8_t more_l[6] = { 0, 0, 0x01, 0x20, 0x25, 0x40 }, sei_l[6] = { 0, 0, 0, 9, 0x06, 0x05 };
    assert(vxd_more_slices(more_b4, true));
    assert(vxd_more_slices(more_b3, true));
    assert(!vxd_more_slices(new_b3, true));
    assert(vxd_more_slices(more_l, false));
    assert(!vxd_more_slices(sei_l, false));
    printf("ok\n");
    return 0;
}
