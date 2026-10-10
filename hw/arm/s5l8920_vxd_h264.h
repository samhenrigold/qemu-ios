/* VXD H.264 on the host (s5l8920_vxd_h264.c). */
#ifndef HW_ARM_S5L8920_VXD_H264_H
#define HW_ARM_S5L8920_VXD_H264_H

typedef struct VXDH264 VXDH264;

/* One slice as the guest gave it: the whole NAL (header byte first) and its registers. */
typedef struct VXDSlice {
    const uint8_t *nal;
    size_t len;
    unsigned sr_bit;        /* where the shift register starts, in bits from the NAL's first byte */
    uint32_t sps0, pps0, pic0, slice0, slice1;
    bool last;              /* the picture's last slice */
} VXDSlice;

typedef struct VXDPicture {
    const uint8_t *plane[3];  /* Y, Cb, Cr (4:2:0) */
    int stride[3];
    int width, height;
} VXDPicture;

VXDH264 *vxd_h264_new(void);
void vxd_h264_free(VXDH264 *h);
/* 1: a picture is complete in *pic (valid until the next call); 0: not yet; -1: the slice failed. */
int vxd_h264_decode(VXDH264 *h, const VXDSlice *sl, VXDPicture *pic);

#endif
