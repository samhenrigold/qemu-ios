#ifndef HW_ARM_IPOD_VIDEO_H
#define HW_ARM_IPOD_VIDEO_H
#ifdef __APPLE__
#include <CoreMedia/CoreMedia.h>
typedef struct IPodVideoDecoder IPodVideoDecoder;
IPodVideoDecoder *ipod_video_create(CMVideoFormatDescriptionRef format, OSType pixel_format);
void ipod_video_close(IPodVideoDecoder *decoder);
bool ipod_video_frame(IPodVideoDecoder *decoder, uint8_t *data, size_t length,
                      uint32_t y, uint32_t uv);
#endif
#endif
