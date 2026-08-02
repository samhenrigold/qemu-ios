#include "hw/arm/ipod_touch_multitouch.h"
#include "qemu/log.h"

static MTFrame *get_empty_frame(IPodTouchMultitouchState *s);

static bool mt_trace(void)
{
    static int on = -1;
    if (on < 0) { on = getenv("MT_TRACE") != NULL; }
    return on;
}
#define MTT(fmt, ...) do { if (mt_trace()) { \
    fprintf(stderr, "[MT] " fmt "\n", ##__VA_ARGS__); } } while (0)

static void prepare_interface_version_response(IPodTouchMultitouchState *s) {
    memset(s->out_buffer + 1, 0, 15);

    // set the interface version
    s->out_buffer[2] = MT_INTERFACE_VERSION;

    // set the max packet size
    s->out_buffer[3] = (MT_MAX_PACKET_SIZE & 0xFF);
    s->out_buffer[4] = (MT_MAX_PACKET_SIZE >> 8) & 0xFF;

    // compute and set the checksum
    uint32_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += s->out_buffer[i];
    }

    s->out_buffer[14] = (checksum & 0xFF);
    s->out_buffer[15] = (checksum >> 8) & 0xFF;
}

static void prepare_cmd_status_response(IPodTouchMultitouchState *s) {
    memset(s->out_buffer + 1, 0, 15);

    // TODO we should probably set some CMD status here

    // compute and set the checksum
    uint32_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += s->out_buffer[i];
    }

    s->out_buffer[14] = (checksum & 0xFF);
    s->out_buffer[15] = (checksum >> 8) & 0xFF;
}

static void prepare_report_info_response(IPodTouchMultitouchState *s, uint8_t report_id) {
    memset(s->out_buffer + 1, 0, 15);

    // set the error
    s->out_buffer[2] = 0;

    // set the report length
    uint32_t report_length = 0;
    if(report_id == MT_REPORT_UNKNOWN1) {
        report_length = MT_REPORT_UNKNOWN1_SIZE;
    }
    else if(report_id == MT_REPORT_FAMILY_ID) {
        report_length = MT_REPORT_FAMILY_ID_SIZE;
    }
    else if(report_id == MT_REPORT_SENSOR_INFO) {
        report_length = MT_REPORT_SENSOR_INFO_SIZE;
    }
    else if(report_id == MT_REPORT_SENSOR_REGION_DESC) {
        report_length = MT_REPORT_SENSOR_REGION_DESC_SIZE;
    }
    else if(report_id == MT_REPORT_SENSOR_REGION_PARAM) {
        report_length = MT_REPORT_SENSOR_REGION_PARAM_SIZE;
    }
    else if(report_id == MT_REPORT_SENSOR_DIMENSIONS) {
        report_length = MT_REPORT_SENSOR_DIMENSIONS_SIZE;
    }
    else {
        /*
         * Anything outside the six modelled report IDs used to hw_error(),
         * which aborts the whole process -- a guest asking an innocuous
         * question killed the emulator. A third-party app (Wordsmith) does
         * exactly this with report ID 0xbf and took QEMU down with it.
         *
         * Report a zero-length report instead: the guest gets a well-formed,
         * empty answer for a feature we do not model, and carries on.
         */
        qemu_log_mask(LOG_UNIMP,
                      "ipod_touch_multitouch: unimplemented report ID 0x%02x\n",
                      report_id);
        report_length = 0;
    }

    s->out_buffer[3] = (report_length & 0xFF);
    s->out_buffer[4] = (report_length >> 8) & 0xFF;

    // compute and set the checksum
    uint32_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += s->out_buffer[i];
    }

    s->out_buffer[14] = (checksum & 0xFF);
    s->out_buffer[15] = (checksum >> 8) & 0xFF;
}

static void prepare_short_control_response(IPodTouchMultitouchState *s, uint8_t report_id) {
    memset(s->out_buffer + 1, 0, 15);

    if(report_id == MT_REPORT_FAMILY_ID) {
        s->out_buffer[3] = MT_FAMILY_ID;
    }
    else if(report_id == MT_REPORT_SENSOR_INFO) {
        s->out_buffer[3] = MT_ENDIANNESS;
        s->out_buffer[4] = MT_SENSOR_ROWS;
        s->out_buffer[5] = MT_SENSOR_COLUMNS;
        s->out_buffer[6] = (MT_BCD_VERSION & 0xFF);
        s->out_buffer[7] = (MT_BCD_VERSION >> 8) & 0xFF;
    }
    else if(report_id == MT_REPORT_SENSOR_REGION_DESC) {
        s->out_buffer[3] = MT_SENSOR_REGION_DESC;
    }
    else if(report_id == MT_REPORT_SENSOR_REGION_PARAM) {
        s->out_buffer[3] = MT_SENSOR_REGION_PARAM;
    }
    else if(report_id == MT_REPORT_SENSOR_DIMENSIONS) {
        uint32_t *ob_int32 = (uint32_t *)&s->out_buffer[3];
        ob_int32[0] = MT_SENSOR_SURFACE_WIDTH;
        ob_int32[1] = MT_SENSOR_SURFACE_HEIGHT;
    }
    else {
        hw_error("Unknown report ID 0x%02x\n", report_id);
    }

    // compute and set the checksum
    uint32_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += s->out_buffer[i];
    }

    s->out_buffer[14] = (checksum & 0xFF);
    s->out_buffer[15] = (checksum >> 8) & 0xFF;
}

static uint32_t ipod_touch_multitouch_transfer(SSIPeripheral *dev, uint32_t value)
{
    IPodTouchMultitouchState *s = IPOD_TOUCH_MULTITOUCH(dev);

    //printf("<MULTITOUCH> Got value: 0x%02x\n", value);

    if(s->cur_cmd == 0) {
        //printf("Starting command 0x%02x\n", value);
        // we're currently not in a command - start a new command
        if (s->prev_cmd_log) {
            MTT("cmd 0x%02x clocked %d bytes (buf_size %d)",
                s->prev_cmd_log, s->buf_ind, s->buf_size);
        }
        s->prev_cmd_log = value;
        s->cur_cmd = value;
        /*
         * These were leaked once per SPI command (0x200 bytes a time, and the
         * guest polls the panel continuously). next_frame is never aliased to
         * out_buffer - MT_CMD_FRAME_READ clears it on handover - so freeing
         * here is safe.
         */
        free(s->out_buffer);
        free(s->in_buffer);
        s->out_buffer = calloc(1, MT_BUFFER_SIZE);
        s->out_buffer[0] = value; // the response header
        s->buf_ind = 0;
        s->in_buffer = calloc(1, MT_BUFFER_SIZE);
        s->in_buffer_ind = 0;
        
        if(value == 0x18) { // filler packet??
            s->buf_size = 2;
            s->out_buffer[1] = 0xE1;
        }
        else if(value == 0x1A) { // HBPP ACK
            s->buf_size = 2;
            if(s->hbpp_atn_ack_response[0] == 0 && s->hbpp_atn_ack_response[1] == 0) {
                // return the default ACK response
                s->out_buffer[0] = 0x4B;
                s->out_buffer[1] = 0xC1;
            }
            else {
                s->out_buffer[0] = s->hbpp_atn_ack_response[0];
                s->out_buffer[1] = s->hbpp_atn_ack_response[1];
            }
             
        }
        else if(value == 0x1C) { // read register
            s->buf_size = 8;
            memset(s->out_buffer, 0, 8); // just return zeros
        }
        else if(value == 0x1D) { // execute
            s->buf_size = 12;
            memset(s->out_buffer, 0, 12); // just return zeros
        }
        else if(value == 0x1F) { // calibration
            s->buf_size = 2;
            s->out_buffer[1] = 0x0;
        }
        else if(value == 0x1E) { // write register
            s->buf_size = 16;
            memset(s->out_buffer, 0, 16); // just return zeros
        }
        else if(value == 0x1F) { // calibration
            s->buf_size = 2;
            s->out_buffer[1] = 0x0;
        }
        else if(value == MT_CMD_HBPP_DATA_PACKET) {
            s->buf_size = 20; // should be enough initially, until we get the packet length
            memset(s->out_buffer + 1, 0, 20 - 1); // just return zeros
            /*
             * The ATN ACK (0x1A) reports the status of the LAST operation, and
             * the two statuses are not interchangeable: the firmware-download
             * loop in AppleMultitouch*SPI compares the ack against 0x4BC1 and
             * retries the same chunk five times before giving up, while the
             * register-write path compares against 0x4AD1.
             *
             * hbpp_atn_ack_response used to be a one-way latch: the first 0x1E
             * set it to 0x4AD1 and nothing ever put it back. That is invisible
             * on the first boot (the whole download happens before any 0x1E),
             * but every LATER download -- and the driver re-downloads the ~48 KB
             * of panel firmware every time it powers the digitizer back up after
             * sleep -- got 0x4AD1 for every chunk, burned its five retries and
             * bailed, leaving touch dead until reboot.
             *
             * Starting a HBPP data packet means we are in the bootloader again,
             * so put the ack back to the download status.
             */
            s->hbpp_atn_ack_response[0] = 0x4B;
            s->hbpp_atn_ack_response[1] = 0xC1;
        }
        else if(value == 0x47) { // unknown command, probably used to clear the interrupt
            s->buf_size = 2;
        }
        else if(value == MT_CMD_GET_CMD_STATUS) {
            s->buf_size = 16;
            prepare_cmd_status_response(s);
        }
        else if(value == MT_CMD_GET_INTERFACE_VERSION) {
            s->buf_size = 16;
            prepare_interface_version_response(s);
        }
        else if(value == MT_CMD_GET_REPORT_INFO) {
            s->buf_size = 16;
        }
        else if(value == MT_CMD_SHORT_CONTROL_WRITE) {
            s->buf_size = 16;
        }
        else if(value == MT_CMD_SHORT_CONTROL_READ) {
            s->buf_size = 16;
        }
        else if(value == MT_CMD_FRAME_READ || value == MT_CMD_FRAME_READ_V2) {
            s->buf_size = sizeof(MTFrame);
            /*
             * out_buffer takes ownership of the pending frame, and next_frame
             * must be cleared or the next read frees the same allocation again
             * (the guest polls faster than touch_timer_tick produces frames, so
             * during a drag the same pointer was handed over repeatedly and
             * double-freed).
             *
             * When there is no pending frame we must STILL install a
             * frame-sized buffer: buf_size is sizeof(MTFrame) and the read loop
             * below walks that many bytes, so leaving the 0x100 scratch buffer
             * in place overruns the heap. That corruption showed up far away,
             * as a SIGSEGV inside QEMU's own TCG structures.
             */
            MTT("FRAME_READ pending=%s", s->next_frame ? "yes" : "NO");
            free(s->out_buffer);
            if (s->next_frame) {
                s->out_buffer = (uint8_t *) s->next_frame;
                s->next_frame = NULL;
            } else {
                /*
                 * No new frame. Handing the guest a block of ZEROS here is not
                 * "no data" - it is a malformed frame: zero cmd byte, zero
                 * lengths, zero checksums. 3.1.3's driver polls about twice as
                 * fast as the 10Hz touch timer produces frames, so ~40% of its
                 * reads landed on one of these, and the finger's frame sequence
                 * never survived contact. Give it a well-formed report with no
                 * fingers instead, which is what the real controller returns
                 * when it is polled and has nothing new.
                 */
                s->out_buffer = (uint8_t *) get_empty_frame(s);
            }
        }
        else if(value == 0x00) {
            /* SPI idle byte, not a command. */
            s->cur_cmd = 0;
            s->buf_size = 0;
        }
        else {
            printf("%s Unknown command 0x%02x!\n", __func__, value);
        }
    }

    s->in_buffer[s->in_buffer_ind] = value;
    s->in_buffer_ind++;

    if(s->cur_cmd == MT_CMD_HBPP_DATA_PACKET && s->in_buffer_ind == 10) {
        // verify the header checksum
        uint32_t checksum = 0;
        for(int i = 2; i < 8; i++) {
            checksum += s->in_buffer[i];
        }

        if(checksum != (s->in_buffer[8] << 8 | s->in_buffer[9])) {
            hw_error("HBPP data header checksum doesn't match!");
        }

        uint32_t data_len = (s->in_buffer[2] << 10) | (s->in_buffer[3] << 2) + 5;
        // extend the lengths of the in/out buffers
        free(s->in_buffer);
        s->in_buffer = malloc(data_len + 0x10);

        free(s->out_buffer);
        s->out_buffer = malloc(data_len);
        memset(s->out_buffer, 0, data_len);
        s->buf_size = data_len;
        s->buf_ind = 0;
    }
    else if(s->cur_cmd == MT_CMD_GET_REPORT_INFO && s->in_buffer_ind == 2) {
        prepare_report_info_response(s, s->in_buffer[1]);
    }
    else if(s->cur_cmd == MT_CMD_SHORT_CONTROL_WRITE && s->in_buffer_ind == 16) {
        // TODO we should persist the report here!
    }
    else if(s->cur_cmd == MT_CMD_SHORT_CONTROL_READ && s->in_buffer_ind == 2) {
        prepare_short_control_response(s, s->in_buffer[1]);
    }

    // TODO process register writes!

    /* The guest can clock out more bytes than the response holds; never read
     * past the buffer. */
    uint8_t ret_val = 0;
    if (s->out_buffer && s->buf_ind < s->buf_size) {
        ret_val = s->out_buffer[s->buf_ind];
    }
    s->buf_ind++;

    //printf("<MULTITOUCH> Got value: 0x%02x, returning 0x%02x (index: %d, buffer length: %d)\n", value, ret_val, s->buf_ind, s->buf_size);

    if(s->buf_ind >= s->buf_size) {
        //printf("Finished command 0x%02x\n", s->cur_cmd);

        if(s->cur_cmd == 0x1E) {
            // make sure we return a success status on the next HBPP ACK
            s->hbpp_atn_ack_response[0] = 0x4A;
            s->hbpp_atn_ack_response[1] = 0xD1;
        }

        // we're done with the command
        s->cur_cmd = 0;
        s->buf_size = 0;
        //free(s->out_buffer);
        //free(s->in_buffer);
    }

    return ret_val;
}

static MTFrame *get_frame(IPodTouchMultitouchState *s, uint8_t event, float x, float y, uint16_t radius1, uint16_t radius2, uint16_t radius3, uint16_t contactDensity) {
    /*
     * Deliberately generous: the SPI read path walks out_buffer by buf_size and
     * the guest can clock out more than sizeof(MTFrame). Allocating exactly the
     * struct size corrupts the heap (observed as a SIGSEGV inside TCG's
     * translation-block tree). Keep the padding until the real transfer length
     * is pinned down.
     */
    MTFrame *frame = calloc(sizeof(MTFrame), sizeof(uint8_t *));

    uint16_t data_len = sizeof(MTFrameHeader) + sizeof(FingerData) + 2;

    /// create the frame length packet
    frame->frame_length.cmd = MT_CMD_FRAME_READ;
    frame->frame_length.length1 = (data_len & 0xFF);
    frame->frame_length.length2 = (data_len >> 8) & 0xFF;

    uint16_t checksum = 0;
    for(int i = 0; i < 14; i++) {
        checksum += ((uint8_t *) &frame->frame_length)[i];
    }
    frame->frame_length.checksum1 = (checksum & 0xFF);
    frame->frame_length.checksum2 = (checksum >> 8) & 0xFF;

    // create the frame packet
    frame->frame_packet.cmd = MT_CMD_FRAME_READ;
    frame->frame_packet.length1 = (data_len & 0xFF);
    frame->frame_packet.length2 = (data_len >> 8) & 0xFF;

    checksum = 0;
    for(int i = 0; i < 4; i++) {
        checksum += ((uint8_t *) &frame->frame_length)[i];
    }

    // the first five bytes have to sum up to 0.
    frame->frame_packet.checksum_pad = 0xFF - (checksum & 0xFF) + 1;

    frame->frame_packet.header.type = MT_FRAME_TYPE_PATH;
    frame->frame_packet.header.frameNum = s->frame_counter;
    frame->frame_packet.header.headerLen = sizeof(MTFrameHeader);
    uint64_t elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
    frame->frame_packet.header.timestamp = elapsed_ns;
    frame->frame_packet.header.numFingers = 1;
    frame->frame_packet.header.fingerDataLen = sizeof(FingerData);

    // create the finger data
    frame->finger_data.id = 1;
    frame->finger_data.event = event;
    frame->finger_data.unk_2 = 2;
    frame->finger_data.unk_3 = 1;

    // compute the velocity
    /*
     * velY used to be derived from x, so vertical flicks reported no vertical
     * speed at all. The scaling was also integer-divided before the *1000, so
     * any delta smaller than the elapsed time truncated to zero - which is
     * every ordinary drag. Both together left velX/velY pinned at 0, and the
     * gesture recognisers that need speed (slide-to-unlock especially) never
     * fired. Multiply first, then divide.
     */
    int diff_x = (int)((x - s->prev_touch_x) * MT_INTERNAL_SENSOR_SURFACE_WIDTH);
    int diff_y = (int)((y - s->prev_touch_y) * MT_INTERNAL_SENSOR_SURFACE_HEIGHT);
    int64_t dt = elapsed_ns + 1 - s->last_frame_timestamp;
    frame->finger_data.velX = (int)(diff_x * 1000 / dt);
    frame->finger_data.velY = (int)(diff_y * 1000 / dt);

    frame->finger_data.x = (int)(x * MT_INTERNAL_SENSOR_SURFACE_WIDTH);
    frame->finger_data.y = (int)(y * MT_INTERNAL_SENSOR_SURFACE_HEIGHT);
    frame->finger_data.radius1 = radius1;
    frame->finger_data.radius2 = radius2;
    frame->finger_data.radius3 = radius3;
    frame->finger_data.angle = 19317;
    frame->finger_data.contactDensity = contactDensity; // seems to be a medium press

    // compute the checksum over the frame data.
    checksum = 0;
    for(int i = 0; i < data_len - 2; i++) {
        checksum += ((uint8_t *) &frame->frame_packet.header)[i];
    }
    frame->checksum1 = (checksum & 0xFF);
    frame->checksum2 = (checksum >> 8) & 0xFF;

    s->last_frame_timestamp = elapsed_ns;
    s->frame_counter += 1;

    return frame;
}

/*
 * A valid frame that reports no fingers, for polls that arrive between real
 * frames. Mirrors get_frame's framing exactly; only the finger data is absent.
 */
static MTFrame *get_empty_frame(IPodTouchMultitouchState *s)
{
    MTFrame *frame = calloc(sizeof(MTFrame), sizeof(uint8_t *));
    uint16_t data_len = sizeof(MTFrameHeader) + 2;
    uint16_t checksum = 0;

    frame->frame_length.cmd = MT_CMD_FRAME_READ;
    frame->frame_length.length1 = (data_len & 0xFF);
    frame->frame_length.length2 = (data_len >> 8) & 0xFF;
    for (int i = 0; i < 14; i++) {
        checksum += ((uint8_t *) &frame->frame_length)[i];
    }
    frame->frame_length.checksum1 = (checksum & 0xFF);
    frame->frame_length.checksum2 = (checksum >> 8) & 0xFF;

    frame->frame_packet.cmd = MT_CMD_FRAME_READ;
    frame->frame_packet.length1 = (data_len & 0xFF);
    frame->frame_packet.length2 = (data_len >> 8) & 0xFF;
    checksum = 0;
    for (int i = 0; i < 4; i++) {
        checksum += ((uint8_t *) &frame->frame_length)[i];
    }
    frame->frame_packet.checksum_pad = 0xFF - (checksum & 0xFF) + 1;

    frame->frame_packet.header.type = MT_FRAME_TYPE_PATH;
    frame->frame_packet.header.frameNum = s->frame_counter;
    frame->frame_packet.header.headerLen = sizeof(MTFrameHeader);
    frame->frame_packet.header.timestamp =
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
    frame->frame_packet.header.numFingers = 0;
    frame->frame_packet.header.fingerDataLen = sizeof(FingerData);

    checksum = 0;
    for (int i = 0; i < data_len - 2; i++) {
        checksum += ((uint8_t *) &frame->frame_packet.header)[i];
    }
    frame->checksum1 = (checksum & 0xFF);
    frame->checksum2 = (checksum >> 8) & 0xFF;

    s->frame_counter += 1;
    return frame;
}

static void ipod_touch_multitouch_inform_frame_ready(IPodTouchMultitouchState *s) {
    MTT("frame ready -> raise gpio3 bit13");
    s->sysic->gpio_int_status[3] |= (1 << 13); // the multitouch interrupt bit is in group 3 (32 interrupts per group), and the 13th of the 3th group
    qemu_irq_raise(s->sysic->gpio_irqs[3]);
}

void ipod_touch_multitouch_on_touch(IPodTouchMultitouchState *s) {
    MTT("TOUCH START at (%.3f, %.3f)", s->touch_x, s->touch_y);
    s->touch_down = true;

    s->next_frame = get_frame(s, MT_EVENT_TOUCH_START, s->touch_x, s->touch_y, 100, 660, 580, 150);
    ipod_touch_multitouch_inform_frame_ready(s);

    timer_mod(s->touch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 10);
}

void ipod_touch_multitouch_on_release(IPodTouchMultitouchState *s) {
    MTT("TOUCH END at (%.3f, %.3f)", s->touch_x, s->touch_y);
    s->next_frame = get_frame(s, MT_EVENT_TOUCH_ENDED, s->touch_x, s->touch_y, 0, 0, 0, 0);
    s->touch_down = false;
    ipod_touch_multitouch_inform_frame_ready(s);

    timer_del(s->touch_timer);
    timer_mod(s->touch_end_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 10);
}

static void touch_timer_tick(void *opaque)
{
    IPodTouchMultitouchState *s = (IPodTouchMultitouchState *)opaque;

    s->next_frame = get_frame(s, MT_EVENT_TOUCH_MOVED, s->touch_x, s->touch_y, 100, 660, 580, 150);
    ipod_touch_multitouch_inform_frame_ready(s);

    if(s->touch_down) {
        // reschedule the timer
        timer_mod(s->touch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 10);
    }
}

static void touch_end_timer_tick(void *opaque)
{
    IPodTouchMultitouchState *s = (IPodTouchMultitouchState *)opaque;
    s->next_frame = get_frame(s, MT_EVENT_TOUCH_FULL_END, s->touch_x, s->touch_y, 0, 0, 0, 0);
    s->touch_down = false;
    ipod_touch_multitouch_inform_frame_ready(s);
}

static void ipod_touch_multitouch_realize(SSIPeripheral *d, Error **errp)
{
    IPodTouchMultitouchState *s = IPOD_TOUCH_MULTITOUCH(d);
    memset(s->hbpp_atn_ack_response, 0, 2);
    s->touch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, touch_timer_tick, s);
    s->touch_end_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, touch_end_timer_tick, s);

    s->prev_touch_x = 0;
    s->prev_touch_y = 0;
    s->last_frame_timestamp = 0;
}

/*
 * Put the digitizer back to power-on state on a warm reset.
 *
 * This device is a command/buffer state machine: cur_cmd plus the in/out buffer
 * cursors track where it is in an SPI exchange, and hbpp_atn_ack_response is a
 * two-byte latch handed back on the next transfer. A reset landing anywhere
 * mid-sequence leaves the next boot's very first exchange answering with the
 * previous boot's leftovers, and AppleMultitouchZ2SPI's bootloader never gets
 * off the ground -- on the second boot there is no "enabled power, scheduled
 * bootloading" and no firmware or calibration download at all.
 *
 * That is enough to stop SpringBoard: no digitizer means no HID event service,
 * and the graphics handoff never completes even though the display stack itself
 * came up.
 *
 * The frame state goes too, so the next boot does not inherit a half-reported
 * touch or a stale frame counter.
 */
static void ipod_touch_multitouch_reset(DeviceState *dev)
{
    IPodTouchMultitouchState *s = IPOD_TOUCH_MULTITOUCH(dev);

    s->cur_cmd = 0;
    s->buf_size = 0;
    s->buf_ind = 0;
    s->in_buffer_ind = 0;
    memset(s->hbpp_atn_ack_response, 0, sizeof(s->hbpp_atn_ack_response));

    s->next_frame = NULL;
    s->frame_counter = 0;
    s->touch_down = false;
    s->touch_x = 0;
    s->touch_y = 0;
    s->prev_touch_x = 0;
    s->prev_touch_y = 0;
    s->last_frame_timestamp = 0;

    if (s->touch_timer) {
        timer_del(s->touch_timer);
    }
    if (s->touch_end_timer) {
        timer_del(s->touch_end_timer);
    }
}

static void ipod_touch_multitouch_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = ipod_touch_multitouch_realize;
    k->transfer = ipod_touch_multitouch_transfer;
    dc->reset = ipod_touch_multitouch_reset;
}

static const TypeInfo ipod_touch_multitouch_type_info = {
    .name = TYPE_IPOD_TOUCH_MULTITOUCH,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(IPodTouchMultitouchState),
    .class_init = ipod_touch_multitouch_class_init,
};

static void ipod_touch_multitouch_register_types(void)
{
    type_register_static(&ipod_touch_multitouch_type_info);
}

type_init(ipod_touch_multitouch_register_types)
