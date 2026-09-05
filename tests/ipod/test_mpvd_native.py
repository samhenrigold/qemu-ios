#!/usr/bin/env python3
"""Compare MPVD's generated MPEG-4 configuration against a movie's real VOL.

macOS: python3 tests/ipod/test_mpvd_native.py movie.mov WIDTH HEIGHT TIME_BITS
Requires access to the native media service. No guest assets are bundled.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "hw/arm/ipod_touch_mpvd.c").read_text()
helpers = source[source.index("static void mpvd_bits("):source.index("static void mpvd_decoder_close(")]
probe = r'''#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
HELPERS
struct Result { CVPixelBufferRef image; OSStatus status; };
static void decoded(void *ctx, void *src, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime pts, CMTime duration) {
    struct Result *r = ctx;
    r->status = status;
    if (image) r->image = CVPixelBufferRetain(image);
}
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 5);
    unsigned width = strtoul(argv[2], NULL, 10);
    unsigned height = strtoul(argv[3], NULL, 10);
    unsigned bits = strtoul(argv[4], NULL, 10);
    assert(!mpvd_make_format(0, height, bits));
    assert(!mpvd_make_format(width, 2049, bits));
    assert(!mpvd_make_format(width, height, 17));
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:
        [NSURL fileURLWithPath:@(argv[1])] options:nil];
    AVAssetTrack *track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
    assert(track);
    NSError *error = nil;
    AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    AVAssetReaderTrackOutput *output = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:track outputSettings:nil];
    [reader addOutput:output];
    assert([reader startReading]);
    CMVideoFormatDescriptionRef generated = mpvd_make_format(width, height, bits);
    assert(generated);
    CMVideoFormatDescriptionRef formats[] = {
        (__bridge CMVideoFormatDescriptionRef)track.formatDescriptions.firstObject,
        generated };
    VTDecompressionSessionRef sessions[2] = {0};
    struct Result results[2] = {0};
    NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey:
        @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)};
    for (unsigned i = 0; i < 2; i++) {
        VTDecompressionOutputCallbackRecord callback = {decoded, &results[i]};
        OSStatus status = VTDecompressionSessionCreate(NULL, formats[i], NULL,
            (__bridge CFDictionaryRef)attrs, &callback, &sessions[i]);
        if (status) fprintf(stderr, "native decoder creation: %d\n", (int)status);
        assert(!status);
    }
    unsigned frames = 0;
    CMSampleBufferRef sample;
    while ((sample = [output copyNextSampleBuffer])) {
        CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample);
        if (!data) { CFRelease(sample); continue; }
        CMSampleTimingInfo timing;
        assert(!CMSampleBufferGetSampleTimingInfo(sample, 0, &timing));
        size_t size = CMSampleBufferGetTotalSampleSize(sample);
        for (unsigned i = 0; i < 2; i++) {
            CMSampleBufferRef packet = NULL;
            assert(!CMSampleBufferCreateReady(NULL, data, formats[i], 1, 1,
                &timing, 1, &size, &packet));
            assert(!VTDecompressionSessionDecodeFrame(sessions[i], packet, 0, NULL, NULL));
            assert(!VTDecompressionSessionWaitForAsynchronousFrames(sessions[i]));
            CFRelease(packet);
            assert(!results[i].status && results[i].image);
            assert(CVPixelBufferGetWidth(results[i].image) == width);
            assert(CVPixelBufferGetHeight(results[i].image) == height);
            assert(CVPixelBufferGetPlaneCount(results[i].image) == 2);
            assert(!CVPixelBufferLockBaseAddress(results[i].image, kCVPixelBufferLock_ReadOnly));
        }
        for (unsigned plane = 0; plane < 2; plane++) {
            const uint8_t *a = CVPixelBufferGetBaseAddressOfPlane(results[0].image, plane);
            const uint8_t *b = CVPixelBufferGetBaseAddressOfPlane(results[1].image, plane);
            size_t as = CVPixelBufferGetBytesPerRowOfPlane(results[0].image, plane);
            size_t bs = CVPixelBufferGetBytesPerRowOfPlane(results[1].image, plane);
            for (unsigned row = 0; row < (plane ? height / 2 : height); row++) {
                if (memcmp(a + row * as, b + row * bs, width)) {
                    fprintf(stderr, "pixel mismatch frame %u plane %u row %u\n", frames, plane, row);
                    return 1;
                }
            }
        }
        for (unsigned i = 0; i < 2; i++) {
            CVPixelBufferUnlockBaseAddress(results[i].image, kCVPixelBufferLock_ReadOnly);
            CVPixelBufferRelease(results[i].image);
            results[i].image = NULL;
        }
        CFRelease(sample);
        frames++;
    }
    assert(reader.status == AVAssetReaderStatusCompleted && frames);
    for (unsigned i = 0; i < 2; i++) {
        VTDecompressionSessionInvalidate(sessions[i]);
        CFRelease(sessions[i]);
    }
    CFRelease(generated);
    printf("PASS: %u frames, all decoded Y/UV pixels match the original format\n", frames);
    return 0;
}}
'''
if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    with tempfile.TemporaryDirectory(prefix="mpvd-native-") as directory:
        main = pathlib.Path(directory) / "check.m"
        exe = pathlib.Path(directory) / "check"
        main.write_text(probe.replace("HELPERS", helpers))
        subprocess.run(["clang", "-fobjc-arc", "-Wno-deprecated-declarations",
                        "-framework", "AVFoundation", "-framework", "VideoToolbox",
                        "-framework", "Foundation", "-framework", "CoreMedia",
                        "-framework", "CoreVideo", str(main), "-o", str(exe)], check=True)
        subprocess.run([str(exe), *sys.argv[1:]], check=True)
