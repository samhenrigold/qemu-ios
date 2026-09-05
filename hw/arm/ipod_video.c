#include "qemu/osdep.h"
#include "hw/arm/ipod_video.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "qemu/error-report.h"
#ifdef __APPLE__
#include <VideoToolbox/VideoToolbox.h>

struct IPodVideoDecoder {
    VTDecompressionSessionRef session;
    CMVideoFormatDescriptionRef format;
    CVPixelBufferRef image;
    OSStatus status;
};

static void video_decoded(void *opaque, void *source, OSStatus status,
                         VTDecodeInfoFlags flags, CVImageBufferRef image,
                         CMTime pts, CMTime duration)
{
    IPodVideoDecoder *d = opaque;
    d->status = status;
    if (image) {
        if (d->image) {
            CVPixelBufferRelease(d->image);
        }
        d->image = CVPixelBufferRetain(image);
    }
}

void ipod_video_close(IPodVideoDecoder *d)
{
    if (!d) {
        return;
    }
    if (d->session) {
        VTDecompressionSessionWaitForAsynchronousFrames(d->session);
        VTDecompressionSessionInvalidate(d->session);
        CFRelease(d->session);
    }
    if (d->image) {
        CVPixelBufferRelease(d->image);
    }
    CFRelease(d->format);
    g_free(d);
}

IPodVideoDecoder *ipod_video_create(CMVideoFormatDescriptionRef format, OSType pixel_format)
{
    if (!format) {
        return NULL;
    }
    IPodVideoDecoder *d = g_new0(IPodVideoDecoder, 1);
    d->format = (CMVideoFormatDescriptionRef)CFRetain(format);
    OSStatus err;
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixel_format);
    if (!number) {
        ipod_video_close(d);
        return NULL;
    }
    const void *keys[] = { kCVPixelBufferPixelFormatTypeKey };
    const void *values[] = { number };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(number);
    if (!attrs) {
        ipod_video_close(d);
        return NULL;
    }
    VTDecompressionOutputCallbackRecord callback = { video_decoded, d };
    err = VTDecompressionSessionCreate(NULL, d->format, NULL,
                                       attrs, &callback, &d->session);
    CFRelease(attrs);
    if (err) {
        warn_report("ipod video: native decoder creation failed: %d", (int)err);
        ipod_video_close(d);
        return NULL;
    }
    return d;
}

bool ipod_video_frame(IPodVideoDecoder *d, uint8_t *data, size_t length,
                      uint32_t y, uint32_t uv)
{
    CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(d->format);
    unsigned width = dimensions.width, height = dimensions.height;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    OSStatus err;
    bool ok = false;
    if (d->image) {
        CVPixelBufferRelease(d->image);
        d->image = NULL;
    }
    d->status = 0;
    err = CMBlockBufferCreateWithMemoryBlock(NULL, data, length, kCFAllocatorNull,
                                            NULL, 0, length, 0, &block);
    if (err) {
        goto done;
    }
    CMSampleTimingInfo timing = { CMTimeMake(1, 30), kCMTimeZero, kCMTimeInvalid };
    err = CMSampleBufferCreateReady(NULL, block, d->format, 1, 1, &timing,
                                   1, &length, &sample);
    if (err) {
        goto done;
    }
    err = VTDecompressionSessionDecodeFrame(d->session, sample, 0, NULL, NULL);
    VTDecompressionSessionWaitForAsynchronousFrames(d->session);
    if (err || d->status || !d->image ||
        CVPixelBufferGetPlaneCount(d->image) != 2 ||
        CVPixelBufferGetWidth(d->image) != width ||
        CVPixelBufferGetHeight(d->image) != height) {
        warn_report("ipod video: native frame failed: %d/%d", (int)err, (int)d->status);
        goto done;
    }
    if (!y && !uv) {
        ok = true; /* reference picture: retain in native DPB, no guest write */
        goto done;
    }
    if (CVPixelBufferLockBaseAddress(d->image, kCVPixelBufferLock_ReadOnly)) {
        goto done;
    }
    const char *dump_path = getenv("IT_VIDEO_DUMP");
    FILE *dump = dump_path ? fopen(dump_path, "ab") : NULL;
    ok = true;
    for (unsigned plane = 0; plane < 2 && ok; plane++) {
        const uint8_t *pixels = CVPixelBufferGetBaseAddressOfPlane(d->image, plane);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(d->image, plane);
        uint32_t dst = plane ? uv : y;
        for (unsigned row = 0; row < (plane ? height / 2 : height); row++) {
            if (dump && fwrite(pixels + row * stride, 1, width, dump) != width) {
                warn_report("ipod video: diagnostic frame dump failed");
                fclose(dump);
                dump = NULL;
            }
            if (address_space_write(&address_space_memory, dst + row * width,
                                    MEMTXATTRS_UNSPECIFIED,
                                    pixels + row * stride, width)) {
                ok = false;
                break;
            }
        }
    }
    if (dump) {
        fclose(dump);
    }
    CVPixelBufferUnlockBaseAddress(d->image, kCVPixelBufferLock_ReadOnly);
done:
    if (sample) {
        CFRelease(sample);
    }
    if (block) {
        CFRelease(block);
    }
    return ok;
}
#endif
