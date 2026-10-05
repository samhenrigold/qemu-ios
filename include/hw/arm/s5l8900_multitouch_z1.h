#ifndef HW_ARM_S5L8900_MULTITOUCH_Z1_H
#define HW_ARM_S5L8900_MULTITOUCH_Z1_H

#include "hw/arm/ipod_touch_multitouch.h"

/*
 * The original iPhone's digitizer (multi-touch,z1, AppleMultitouchSPI): a
 * Zephyr1 behind SPI2. A subtype of the shared ipodtouch.multitouch, so the
 * host input (set_finger, the frame timers, ATN into the GPIO IC) and the
 * frame contents are the Zephyr2's; only the wire protocol is its own.
 */
#define TYPE_S5L8900_MULTITOUCH_Z1 "s5l8900.multitouch-z1"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8900MultitouchZ1State, S5L8900_MULTITOUCH_Z1)

#define Z1_PACKET_MAX 0x400

typedef struct S5L8900MultitouchZ1State {
    IPodTouchMultitouchState parent_obj;

    uint8_t cmd;                  /* first byte of the transaction in progress, 0 = idle */
    uint32_t index;               /* bytes clocked in this transaction */
    uint32_t length;              /* its length when known without chip select, else 0 */
    uint8_t in[16];               /* the first bytes the host sent */
    uint8_t out[Z1_PACKET_MAX];   /* what we clock back, by index */
    uint32_t out_len;
    bool selected;                /* chip select seen asserted at least once */

    /* bootloader */
    uint32_t sum;                 /* running sum of the packet or stream being received */
    uint32_t upload_sum;          /* what the next verify (05 00 00 06) reports */
    bool stream_next;             /* a blank data packet arms the main firmware stream */
    bool streaming;
    uint32_t stream_bytes;
    unsigned stage;               /* 0 bootloader, 1 A-Speed running, 2 main firmware running */

    /* frames: the one offered by the last length read, until it is read */
    uint8_t *frame;
    uint32_t frame_len;           /* payload (header + fingers) */
    bool offered;                 /* the last transaction was a length read that offered it */
} S5L8900MultitouchZ1State;

#endif
