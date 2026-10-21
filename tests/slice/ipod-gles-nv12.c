/* gles_surface_nv12 against VideoToolbox's VTPixelTransferSession (601 matrix, the conversion it replaced) on every
 * Y, Cb, Cr combination, video and full range: within 1 per channel, and at most 0.6% of the channel values off by 1.
 *
 * SLICE hw/arm/gles-host.c fn gles_surface_nv12
 * CFLAGS -framework VideoToolbox -framework CoreVideo -framework CoreFoundation
 */
#include <VideoToolbox/VideoToolbox.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "slice.h"

static bool vt(unsigned w, unsigned h, OSType format, uint8_t *input, uint8_t *bgra)
{
    CVPixelBufferRef source = NULL, dest = NULL;
    VTPixelTransferSessionRef transfer = NULL;
    void *planes[] = { input, input + (size_t)w * h };
    size_t widths[] = { w, w / 2 }, heights[] = { h, h / 2 }, strides[] = { w, w };
    bool ok = !CVPixelBufferCreateWithPlanarBytes(NULL, w, h, format, NULL, 0, 2, planes, widths, heights,
                                                  strides, NULL, NULL, NULL, &source) &&
        !CVPixelBufferCreateWithBytes(NULL, w, h, kCVPixelFormatType_32BGRA, bgra, (size_t)w * 4,
                                      NULL, NULL, NULL, &dest) &&
        !VTPixelTransferSessionCreate(NULL, &transfer);
    if (ok) {
        CVBufferSetAttachment(source, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4,
                              kCVAttachmentMode_ShouldPropagate);
        ok = VTPixelTransferSessionTransferImage(transfer, source, dest) == noErr;
        VTPixelTransferSessionInvalidate(transfer);
        CFRelease(transfer);
    }
    if (dest) CVPixelBufferRelease(dest);
    if (source) CVPixelBufferRelease(source);
    return ok;
}

int main(void)
{
    /* 2048x2048 chroma samples: each (Cb, Cr) pair 64 times, its 2x2 blocks holding Y 0-255 between them. */
    enum { W = 4096, H = 4096 };
    uint8_t *in = malloc(W * H * 3 / 2), *want = malloc((size_t)W * H * 4), *got = malloc((size_t)W * H * 4);
    for (unsigned k = 0; k < W / 2 * H / 2; k++) {
        unsigned j = k & 63, pair = k >> 6, cy = k / (W / 2), cx = k % (W / 2);
        in[W * H + cy * W + cx * 2] = pair >> 8;
        in[W * H + cy * W + cx * 2 + 1] = pair & 255;
        for (unsigned q = 0; q < 4; q++) in[(cy * 2 + q / 2) * W + cx * 2 + q % 2] = j * 4 + q;
    }
    for (int full = 0; full < 2; full++) {
        uint32_t format = full ? '420f' : '420v';
        assert(vt(W, H, format, in, want) && gles_surface_nv12(W, H, format, in, got));
        size_t off = 0;
        for (size_t i = 0; i < (size_t)W * H * 4; i++) {
            int d = abs(want[i] - got[i]);
            assert(d <= 1);
            off += d;
        }
        printf("%s: max 1, %.3f%% of channel values off by 1\n", full ? "420f" : "420v", off * 100.0 / (W * H * 3.0));
        assert(off * 1000 <= (size_t)W * H * 3 * 6);
    }
    puts("PASS: NV12 to BGRA within 1 of VideoToolbox, video and full range");
    return 0;
}
