#ifndef IPOD_TOUCH_MULTITOUCH_H
#define IPOD_TOUCH_MULTITOUCH_H

/* scratch response buffer for short commands */
#define MT_BUFFER_SIZE 0x100

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/ssi/ssi.h"
#include "hw/arm/ipod_touch_sysic.h"
#include "hw/arm/ipod_touch_gpio.h"

#define TYPE_IPOD_TOUCH_MULTITOUCH                "ipodtouch.multitouch"
OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchMultitouchState, IPOD_TOUCH_MULTITOUCH)

#define MT_INTERFACE_VERSION     0x1
#define MT_FAMILY_ID             81
#define MT_ENDIANNESS            0x1
#define MT_SENSOR_ROWS           15
#define MT_SENSOR_COLUMNS        10
#define MT_BCD_VERSION           51
#define MT_SENSOR_REGION_DESC    0x0
#define MT_SENSOR_REGION_PARAM   0x0
#define MT_MAX_PACKET_SIZE       0x294 // 660
#define MT_SENSOR_SURFACE_WIDTH  5000
#define MT_SENSOR_SURFACE_HEIGHT 7500

// internal surface width/height
#define MT_INTERNAL_SENSOR_SURFACE_WIDTH  (9000 - MT_SENSOR_SURFACE_WIDTH) * 84 / 73
#define MT_INTERNAL_SENSOR_SURFACE_HEIGHT (13850 - MT_SENSOR_SURFACE_HEIGHT) * 84 / 73

// report IDs
#define MT_REPORT_UNKNOWN1            0x70
#define MT_REPORT_FAMILY_ID           0xD1
#define MT_REPORT_SENSOR_INFO         0xD3
#define MT_REPORT_SENSOR_REGION_DESC  0xD0
#define MT_REPORT_SENSOR_REGION_PARAM 0xA1
#define MT_REPORT_SENSOR_DIMENSIONS   0xD9

// report sizes
#define MT_REPORT_UNKNOWN1_SIZE            0x1
#define MT_REPORT_FAMILY_ID_SIZE           0x1
#define MT_REPORT_SENSOR_INFO_SIZE         0x5
#define MT_REPORT_SENSOR_REGION_DESC_SIZE  0x1
#define MT_REPORT_SENSOR_REGION_PARAM_SIZE 0x1
#define MT_REPORT_SENSOR_DIMENSIONS_SIZE   0x8

#define MT_CMD_HBPP_DATA_PACKET      0x30
#define MT_CMD_GET_CMD_STATUS        0xE1
#define MT_CMD_GET_INTERFACE_VERSION 0xE2
#define MT_CMD_GET_REPORT_INFO       0xE3
#define MT_CMD_SHORT_CONTROL_WRITE   0xE4
#define MT_CMD_SHORT_CONTROL_READ    0xE6
#define MT_CMD_FRAME_READ            0xEA
/*
 * 3.1.3's AppleMultitouchZ2SPI never issues 0xEA. It polls the panel with 0xEB
 * instead (observed: 0xEB, then 0xEC and 0x01, repeating). Treat 0xEB as a
 * frame read; without it buf_size stays 0, the transaction is shredded into
 * one-byte "unknown command" fragments, and no touch frame ever reaches iOS.
 */
#define MT_CMD_FRAME_READ_V2         0xEB

// frame types
#define MT_FRAME_TYPE_PATH 0x44

// frame event types
#define MT_EVENT_TOUCH_FULL_END 0x0
#define MT_EVENT_TOUCH_START 0x3
#define MT_EVENT_TOUCH_MOVED 0x4
#define MT_EVENT_TOUCH_ENDED 0x7

typedef struct MTFrameLengthPacket
{
    uint8_t cmd;
    uint8_t length1;
    uint8_t length2;
    uint8_t unused[11];
    uint8_t checksum1;
    uint8_t checksum2;
} __attribute__((__packed__)) MTFrameLengthPacket;

typedef struct MTFrameHeader
{
    uint8_t type;
    uint8_t frameNum;
    uint8_t headerLen;
    uint8_t unk_3;
    uint32_t timestamp;
    uint8_t unk_8;
    uint8_t unk_9;
    uint8_t unk_A;
    uint8_t unk_B;
    uint16_t unk_C;
    uint16_t isImage;

    uint8_t numFingers;
    uint8_t fingerDataLen;
    uint16_t unk_12;
    uint16_t unk_14;
    uint16_t unk_16;
} __attribute__((__packed__)) MTFrameHeader;

typedef struct MTFramePacket
{
    uint8_t cmd;
    uint8_t unused1;
    uint8_t length1;
    uint8_t length2;
    uint8_t checksum_pad;
    MTFrameHeader header;
} __attribute__((__packed__)) MTFramePacket;

typedef struct FingerData
{
    uint8_t id;
    uint8_t event;
    uint8_t unk_2;
    uint8_t unk_3;
    int16_t x;
    int16_t y;
    int16_t velX;
    int16_t velY;
    uint16_t radius2;
    uint16_t radius3;
    uint16_t angle;
    uint16_t radius1;
    uint16_t contactDensity;
    uint16_t unk_16;
    uint16_t unk_18;
    uint16_t unk_1A;
} __attribute__((__packed__)) FingerData;

/*
 * How many simultaneous contacts the model reports. The panel itself handles
 * more, but every gesture iPhone OS 3.1.3 recognises (pinch, rotate, two-finger
 * scroll, three- and four-finger swipes in later builds) fits inside five, and
 * the frame has to stay under the negotiated MT_MAX_PACKET_SIZE.
 */
#define MT_MAX_FINGERS 5

/*
 * A touch frame is VARIABLE LENGTH and the layout below is only the fixed
 * prefix. The full wire frame is
 *
 *   [0 .. 15]                       length packet   (16 bytes)
 *   [16 .. 20]                      frame packet prologue (5 bytes)
 *   [21 .. 21+24)                   frame header    (sizeof(MTFrameHeader))
 *   [45 .. 45+n*28)                 n x FingerData
 *   [45+n*28 .. 45+n*28+2)          16-bit checksum
 *
 * with dataLength = sizeof(MTFrameHeader) + n * sizeof(FingerData) + 2 stored
 * in the length packet, and the total frame length 21 + dataLength.
 *
 * The trailing checksum's POSITION therefore moves with the finger count: the
 * driver reads dataLength out of the length packet and sums dataLength - 2
 * bytes starting at the header, then expects the checksum immediately after.
 * The struct used to end in `FingerData finger_data; uint8_t checksum1,
 * checksum2;` with the comment "TODO we assume one finger for now", which
 * nailed the checksum to offset 73 -- correct for exactly one finger and wrong
 * for every other count. Getting it wrong is not benign: the driver counts
 * checksum failures, and past a threshold it kills the panel and re-probes,
 * which desynchronises the SPI state machine permanently.
 *
 * So the frame is built into a plain byte buffer by mt_build_frame(), which
 * returns the length alongside it. Nothing may reach for a fixed offset.
 */
typedef struct MTFrame {
    MTFrameLengthPacket frame_length;
    MTFramePacket frame_packet;
    uint8_t finger_data[];
} __attribute__((__packed__)) MTFrame;

/* Byte offset of the frame header within the frame; the checksum is measured
 * from here, and so is the total length. */
#define MT_FRAME_PREFIX_LEN (sizeof(MTFrameLengthPacket) + \
                             sizeof(MTFramePacket) - sizeof(MTFrameHeader))

/*
 * Deliberately generous. The SPI read path walks out_buffer by buf_size and the
 * guest can clock out more than the frame holds; allocating exactly the frame
 * size corrupted the heap, which showed up far away as a SIGSEGV inside TCG's
 * translation-block tree.
 */
#define MT_FRAME_ALLOC 0x400

/*
 * Per-finger tracking.
 *
 * A finger is not simply down or up: the driver wants to see TOUCH_START once,
 * then TOUCH_MOVED for as long as the contact lasts, then TOUCH_ENDED, and --
 * when the last contact goes -- a TOUCH_FULL_END frame a moment later. The
 * phase is what the NEXT emitted frame will report for this slot, and
 * mt_emit_frame() advances it.
 */
typedef enum MTFingerPhase {
    MT_FINGER_FREE = 0,   /* not reported at all                              */
    MT_FINGER_DOWN,       /* reports TOUCH_START once, then TOUCH_MOVED       */
    MT_FINGER_LIFTED,     /* reports TOUCH_ENDED on the next frame            */
    MT_FINGER_ENDING,     /* reports TOUCH_FULL_END on the end-timer frame    */
} MTFingerPhase;

typedef struct MTFingerState {
    MTFingerPhase phase;
    bool started;        /* TOUCH_START already reported                      */
    float x, y;          /* position, 0..1 in the guest's portrait FB space   */
    float prev_x, prev_y;/* where this finger was in the last frame WE SENT   */
} MTFingerState;

typedef struct IPodTouchMultitouchState {
    SSIPeripheral ssidev;
    uint8_t cur_cmd;
    uint8_t *out_buffer;
    uint8_t *in_buffer;
    uint32_t buf_size;
    uint8_t prev_cmd_log;
    uint32_t buf_ind;
    uint32_t in_buffer_ind;
    uint8_t hbpp_atn_ack_response[2];
    MTFrame *next_frame;
    /*
     * Length of next_frame on the wire. buf_size is set from this when the
     * frame is handed to the guest, because a frame is no longer a fixed
     * sizeof(MTFrame) once it can carry more than one finger.
     */
    uint32_t next_frame_len;
    uint32_t frame_counter;
    bool touch_down;
    QEMUTimer *touch_timer;
    QEMUTimer *touch_end_timer;
    IPodTouchSYSICState *sysic;
    IPodTouchGPIOState *gpio_state;
    void *pmu;   // Pcf50633State* — D1759 PMU, raises the wake-button interrupt

    MTFingerState fingers[MT_MAX_FINGERS];

    /*
     * Slot 0's position, kept as a separate field because the legacy pointer
     * path writes it directly before calling on_touch/on_motion/on_release:
     * ipod_touch_lcd_mouse_event() and the scripted slide-to-power-off in
     * ipod_touch_2g.c both do this. Mirrored into fingers[0] on every emit.
     * touch_down means "slot 0 is down", which is what those two edge-detect on.
     */
    float touch_x;
    float touch_y;
    float prev_touch_x;
    float prev_touch_y;
    uint64_t last_frame_timestamp;

    /*
     * Virtual-clock time of the last touch frame emitted while the finger is
     * down, used to coalesce host motion to the report rate. See
     * mt_frame_period_ns() in ipod_touch_multitouch.c.
     */
    int64_t last_motion_ns;
} IPodTouchMultitouchState;

/*
 * Single-finger entry points, kept as-is for the pointer paths that set
 * touch_x/touch_y and then call one of these. They all operate on slot 0.
 */
void ipod_touch_multitouch_on_touch(IPodTouchMultitouchState *s);
void ipod_touch_multitouch_on_release(IPodTouchMultitouchState *s);
void ipod_touch_multitouch_on_motion(IPodTouchMultitouchState *s);

/*
 * Multi-finger entry point. slot is 0..MT_MAX_FINGERS-1, x/y are 0..1 in the
 * guest's portrait framebuffer space (y measured from the bottom, matching
 * touch_x/touch_y). Raising and lowering a finger is a state change, so it is
 * reported immediately; a position change on a finger already down is coalesced
 * to the report rate.
 */
void ipod_touch_multitouch_set_finger(IPodTouchMultitouchState *s, int slot,
                                      float x, float y, bool down);

#endif