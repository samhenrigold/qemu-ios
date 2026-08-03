#include "hw/arm/ipod_touch_multitouch.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

static MTFrame *mt_build_frame(IPodTouchMultitouchState *s,
                               MTFingerState *fingers, uint32_t *out_len);

static bool mt_trace(void)
{
    static int on = -1;
    if (on < 0) { on = getenv("MT_TRACE") != NULL; }
    return on;
}
#define MTT(fmt, ...) do { if (mt_trace()) { \
    fprintf(stderr, "[MT] " fmt "\n", ##__VA_ARGS__); } } while (0)

/* Saturate a computed finger velocity into the int16_t the frame carries. */
static int16_t mt_clamp_vel(int64_t v)
{
    if (v > INT16_MAX) { return INT16_MAX; }
    if (v < INT16_MIN) { return INT16_MIN; }
    return (int16_t)v;
}

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
        /*
         * The report ID comes straight from the guest. Aborting QEMU here made
         * a driver probing for a report we do not implement kill the whole
         * machine -- the documented "Unknown report ID 0xbf" death. The
         * GET_REPORT_INFO path above was converted for exactly this reason and
         * this one was missed. Answer with a zeroed report: the checksum below
         * still runs, so the driver sees a well-formed reply it can reject on
         * its own terms.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "[MT] unimplemented short-control report ID 0x%02x; "
                      "returning an empty report\n", report_id);
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
            /*
             * out_buffer takes ownership of the pending frame, and next_frame
             * must be cleared or the next read frees the same allocation again
             * (the guest polls faster than touch_timer_tick produces frames, so
             * during a drag the same pointer was handed over repeatedly and
             * double-freed).
             *
             * When there is no pending frame we must STILL install a real frame
             * buffer: buf_size is the frame's length and the read loop below
             * walks that many bytes, so leaving the 0x100 scratch buffer in
             * place overruns the heap. That corruption showed up far away, as a
             * SIGSEGV inside QEMU's own TCG structures.
             */
            MTT("FRAME_READ pending=%s", s->next_frame ? "yes" : "NO");
            /*
             * IT_MT_EMPTY_TEST=N drops every Nth pending frame, forcing the
             * no-pending-frame path below.
             *
             * This exists because that path is unreachable on demand. The
             * driver only reads a frame after we raise the ATN interrupt, and
             * we only raise it when a frame is queued, so a healthy 3.1.3
             * session takes it approximately never: measured 0 out of 7505
             * frame reads across a ten-minute soak of taps, keyboard input and
             * button presses. A latent bug there is invisible to any amount of
             * driving the UI -- the corrupt-empty-frame bug below survived
             * exactly that way -- but shows up in under three minutes with
             * IT_MT_EMPTY_TEST=7.
             *
             * Regression check for it: run with IT_MT_EMPTY_TEST=41 and count
             * "Unknown command" lines in QEMU's output. Zero is the pass.
             * Measured on this tree, same binary but for get_empty_frame's
             * data_len: 8 forced empty frames produced 614 of them, and the
             * fix produced 0 from 15.
             *
             * Do NOT assert on the UI under this flag. A no-fingers report is a
             * finger LIFT, so dropping frames mid-gesture legitimately cancels
             * drags -- slide-to-unlock fails in both arms of that A/B and tells
             * you nothing. Assert on the byte stream, which is what broke.
             */
            {
                static int every = -1;
                if (every < 0) {
                    const char *e = getenv("IT_MT_EMPTY_TEST");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && s->next_frame &&
                    (s->frame_counter % every) == 0) {
                    MTT("EMPTY_TEST: dropping pending frame %d", s->frame_counter);
                    free(s->next_frame);
                    s->next_frame = NULL;
                }
            }
            free(s->out_buffer);
            if (s->next_frame) {
                s->out_buffer = (uint8_t *) s->next_frame;
                s->buf_size = s->next_frame_len;
                s->next_frame = NULL;
                s->next_frame_len = 0;
            } else {
                /*
                 * No new frame. Handing the guest a block of ZEROS here is not
                 * "no data" - it is a malformed frame: zero cmd byte, zero
                 * lengths, zero checksums. 3.1.3's driver polls about twice as
                 * fast as the touch timer produces frames, so a large fraction
                 * of its reads landed on one of these, and the finger's frame
                 * sequence never survived contact. Give it a well-formed report
                 * with no fingers instead, which is what the real controller
                 * returns when it is polled and has nothing new.
                 */
                uint32_t len;
                s->out_buffer = (uint8_t *) mt_build_frame(s, NULL, &len);
                s->buf_size = len;
            }
        }
        else if(value == 0x00) {
            /* SPI idle byte, not a command. */
            s->cur_cmd = 0;
            s->buf_size = 0;
        }
        else {
            /*
             * Treat this as the desync alarm, not as curiosity about an
             * unmodelled command.
             *
             * This device frames commands by counting bytes and nothing else --
             * there is no chip select to fall back on (see apple_spi_update_cs
             * in ipod_touch_spi.c). So if a response length ever disagrees with
             * what the guest clocks, the model starts reading payload bytes as
             * command bytes and never recovers. What that looks like from here
             * is a burst of "unknown commands" that the driver never actually
             * sent: 0xec, 0xb6, 0x01 and friends, seventeen of each, while the
             * guest logs "Could not detect HBPP" and kills the digitizer.
             *
             * A healthy session emits NONE of these -- measured 0 across 7505
             * frame reads. Any occurrence means touch is already dead or about
             * to be, so it is the assertion to check after touching this file.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "[MT] unknown command 0x%02x - the SPI byte stream has "
                          "most likely desynchronised; touch will not recover\n",
                          value);
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
            /* Guest-supplied bytes. A bad checksum is a NAK on real hardware,
             * not a dead machine. */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "[MT] HBPP data header checksum mismatch "
                          "(computed 0x%04x, header 0x%04x); ignoring\n",
                          checksum, s->in_buffer[8] << 8 | s->in_buffer[9]);
            return 0;
        }

        /* Parenthesised: '+' binds tighter than '|', so this used to compute
         * (b2<<10) | ((b3<<2)+5) -- with b3 == 255 the carry was OR-merged
         * instead of added and data_len disagreed with the byte count the
         * guest then clocked out, terminating the command early and
         * re-parsing the tail as fresh commands. */
        uint32_t data_len = ((s->in_buffer[2] << 10) | (s->in_buffer[3] << 2)) + 5;
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

/*
 * How many FingerData slots the frame carries on the wire, given how many
 * fingers it actually reports.
 *
 * The floor of 1 is not cosmetic. A no-fingers report built with data_len =
 * sizeof(MTFrameHeader) + 2 is a 47-byte frame, and the guest is only known to
 * be happy clocking the 75-byte shape a one-finger frame has. Keeping one
 * zeroed slot in the no-fingers report leaves that path byte-for-byte the
 * length it has always been, so the only frames whose length changes are the
 * genuinely multi-finger ones.
 *
 * IT_MT_PAD_FINGERS raises the floor. Its only purpose is the one-variable
 * experiment that established the guest sizes its read from dataLength rather
 * than from a constant: with it set to 2 every frame is 103 bytes and reports
 * numFingers=1, so touch behaviour is unchanged and the ONLY thing under test
 * is whether a longer frame desynchronises the SPI state machine. (It does not;
 * "Unknown command" stayed at 0.) Left in because that is the assumption the
 * whole multi-finger path rests on and it should stay cheap to re-check.
 */
static int mt_frame_slots(int n)
{
    static int pad = -1;

    if (pad < 0) {
        const char *e = getenv("IT_MT_PAD_FINGERS");
        pad = e ? atoi(e) : 1;
        if (pad < 1 || pad > MT_MAX_FINGERS) {
            pad = 1;
        }
    }
    return n > pad ? n : pad;
}

static bool mt_const_fingerid(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("IT_MT_FINGERID");
        on = (e && !strcmp(e, "const"));
    }
    return on;
}

/*
 * Build a wire frame reporting every non-FREE finger in `fingers`, or a frame
 * reporting no fingers at all when `fingers` is NULL. Returns a heap buffer the
 * caller owns and its length on the wire in *out_len.
 *
 * WHY THE CHECKSUM OFFSET IS COMPUTED AND NOT A STRUCT FIELD
 *
 * AppleMultitouchZ2SPI reads the 16-byte length packet, takes dataLength from
 * it, and then verifies a checksum stored at header + dataLength - 2. The
 * header starts at offset 21, so a one-finger dataLength of 54 puts that
 * checksum at offset 73 -- exactly where the old MTFrame::checksum1 field sat,
 * which is why a fixed offset worked for as long as one finger was all we
 * could report.
 *
 * The no-fingers report used to declare dataLength 26, omitting the finger
 * slot, while still writing its checksum at offset 73. The driver looked at
 * offset 45 instead, read the two zero bytes at the head of the absent finger
 * data, and compared them against a non-zero computed value -- so EVERY
 * no-fingers report was a guaranteed checksum failure:
 * "ERROR: corrupt frame packet checksum == 0x0000. calculated == 0x....".
 * The driver tolerates a few, counts them, then gives up with "Too many errors
 * reading a frame of data" and "*** Killing device (#N) ***". The re-probe that
 * follows leaves the SPI byte stream misaligned -- the model starts parsing
 * payload bytes as commands (a burst of "Unknown command 0xec/0xb6/0x01") --
 * and touch is dead for the rest of the session. Forcing that path deliberately
 * (IT_MT_EMPTY_TEST, every 7th read) reproduced it in under three minutes,
 * including the guest's "Could not detect HBPP" line. This device cannot
 * resynchronise once the two sides disagree: there is no usable chip-select
 * signal (see the note on apple_spi_update_cs in ipod_touch_spi.c).
 *
 * The fix at the time was to give the no-fingers report the same dataLength as
 * a one-finger one. That is now the n == 0 case of the general rule: the
 * checksum offset is derived from dataLength here exactly as the driver derives
 * it, and buf_size is set from the frame's real length. The guest does size its
 * read from dataLength -- measured, see mt_frame_slots() -- so a longer frame is
 * safe; the no-fingers report nonetheless keeps its one zeroed slot so the most
 * common frame on the wire stays the length it has always been.
 */
static MTFrame *mt_build_frame(IPodTouchMultitouchState *s,
                               MTFingerState *fingers, uint32_t *out_len)
{
    uint8_t *buf = calloc(1, MT_FRAME_ALLOC);
    MTFrame *frame = (MTFrame *) buf;
    uint8_t *hdr = (uint8_t *) &frame->frame_packet.header;
    FingerData *fd = (FingerData *) (hdr + sizeof(MTFrameHeader));
    int report[MT_MAX_FINGERS];
    int n = 0, slots, i;
    uint16_t data_len, checksum;
    uint64_t elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
    int64_t dt = elapsed_ns + 1 - s->last_frame_timestamp;

    if (fingers) {
        for (i = 0; i < MT_MAX_FINGERS; i++) {
            if (fingers[i].phase != MT_FINGER_FREE) {
                report[n++] = i;
            }
        }
    }
    slots = mt_frame_slots(n);
    data_len = sizeof(MTFrameHeader) + slots * sizeof(FingerData) + 2;

    /* the frame length packet */
    frame->frame_length.cmd = MT_CMD_FRAME_READ;
    frame->frame_length.length1 = (data_len & 0xFF);
    frame->frame_length.length2 = (data_len >> 8) & 0xFF;

    checksum = 0;
    for (i = 0; i < 14; i++) {
        checksum += ((uint8_t *) &frame->frame_length)[i];
    }
    frame->frame_length.checksum1 = (checksum & 0xFF);
    frame->frame_length.checksum2 = (checksum >> 8) & 0xFF;

    /* the frame packet */
    frame->frame_packet.cmd = MT_CMD_FRAME_READ;
    frame->frame_packet.length1 = (data_len & 0xFF);
    frame->frame_packet.length2 = (data_len >> 8) & 0xFF;

    checksum = 0;
    for (i = 0; i < 4; i++) {
        checksum += ((uint8_t *) &frame->frame_length)[i];
    }
    /* the first five bytes have to sum up to 0 */
    frame->frame_packet.checksum_pad = 0xFF - (checksum & 0xFF) + 1;

    frame->frame_packet.header.type = MT_FRAME_TYPE_PATH;
    frame->frame_packet.header.frameNum = s->frame_counter;
    frame->frame_packet.header.headerLen = sizeof(MTFrameHeader);
    frame->frame_packet.header.timestamp = elapsed_ns;
    frame->frame_packet.header.numFingers = n;
    frame->frame_packet.header.fingerDataLen = sizeof(FingerData);

    for (i = 0; i < n; i++) {
        MTFingerState *f = &fingers[report[i]];
        bool contact = (f->phase == MT_FINGER_DOWN);
        int diff_x, diff_y;

        /*
         * The finger ID is what ties a contact across frames, and it is what
         * the gesture recognisers use to tell "two fingers moving apart" from
         * "one finger that jumped". Slot index + 1, stable for the life of the
         * contact; the single-finger path always reported 1, which is what slot
         * 0 still gets.
         */
        fd[i].id = report[i] + 1;
        switch (f->phase) {
        case MT_FINGER_DOWN:
            fd[i].event = f->started ? MT_EVENT_TOUCH_MOVED
                                     : MT_EVENT_TOUCH_START;
            break;
        case MT_FINGER_LIFTED:
            fd[i].event = MT_EVENT_TOUCH_ENDED;
            break;
        default:
            fd[i].event = MT_EVENT_TOUCH_FULL_END;
            break;
        }
        /*
         * The two bytes after id and event. Apple's multitouch path records
         * carry a finger index and a hand index alongside the path identity,
         * and the single-finger code sent the constants 2 and 1 for the one
         * contact it could describe. Slot 0 still gets exactly those, so
         * one-finger behaviour is unchanged; further contacts get a distinct
         * finger index, on the assumption that two contacts claiming to be the
         * same finger of the same hand is not something real hardware reports.
         *
         * IT_MT_FINGERID=const sends the old constant for every contact, to
         * bisect this if it turns out not to matter.
         */
        fd[i].unk_2 = mt_const_fingerid() ? 2 : (2 + report[i]);
        fd[i].unk_3 = 1;

        /*
         * Velocity.
         *
         * velY used to be derived from x, so vertical flicks reported no
         * vertical speed at all, and the scaling was integer-divided before the
         * *1000, so any delta smaller than the elapsed time truncated to zero -
         * which is every ordinary drag. Multiply first, then divide.
         *
         * The reference point is per finger and is where that finger was in the
         * last frame we SENT, because dt is measured from the last frame's
         * timestamp. Sharing one prev_x/prev_y across fingers would report each
         * finger's speed as the distance to wherever a different finger was.
         */
        diff_x = (int)((f->x - f->prev_x) * MT_INTERNAL_SENSOR_SURFACE_WIDTH);
        diff_y = (int)((f->y - f->prev_y) * MT_INTERNAL_SENSOR_SURFACE_HEIGHT);
        /*
         * velX/velY are int16_t and the quotient is unbounded: a full-width
         * flick covered in a single frame computes into the hundreds of
         * thousands, which truncates on assignment and wraps to a small value
         * of arbitrary SIGN - a fast leftward drag reported as a slow rightward
         * one. Clamp rather than truncate.
         */
        fd[i].velX = mt_clamp_vel(diff_x * 1000 / dt);
        fd[i].velY = mt_clamp_vel(diff_y * 1000 / dt);

        fd[i].x = (int)(f->x * MT_INTERNAL_SENSOR_SURFACE_WIDTH);
        fd[i].y = (int)(f->y * MT_INTERNAL_SENSOR_SURFACE_HEIGHT);
        /* A contact reports a real ellipse; a lifted one reports nothing. */
        fd[i].radius1 = contact ? 100 : 0;
        fd[i].radius2 = contact ? 660 : 0;
        fd[i].radius3 = contact ? 580 : 0;
        fd[i].angle = 19317;
        fd[i].contactDensity = contact ? 150 : 0;

        f->prev_x = f->x;
        f->prev_y = f->y;
    }

    /*
     * The trailing checksum. Its POSITION is derived from data_len, exactly as
     * the driver derives where to look for it, so it follows the finger data
     * wherever the finger count puts it. Writing it to a fixed struct offset is
     * the bug this whole layout exists to prevent.
     */
    checksum = 0;
    for (i = 0; i < data_len - 2; i++) {
        checksum += hdr[i];
    }
    hdr[data_len - 2] = (checksum & 0xFF);
    hdr[data_len - 1] = (checksum >> 8) & 0xFF;

    /*
     * Only a frame that actually reported fingers moves the velocity time base.
     * The no-fingers report is emitted on every poll the guest makes -- far more
     * often than fingers move -- so advancing the timestamp here would leave
     * every real frame computing its velocity over the microseconds since the
     * last poll instead of the time since the last real frame, inflating
     * reported finger speed by more than an order of magnitude and pinning
     * velX/velY at the clamp.
     */
    if (n > 0) {
        s->last_frame_timestamp = elapsed_ns;
    }
    if (n > 0 && mt_trace()) {
        for (i = 0; i < n; i++) {
            MTT("frame %u len %u nf %d slot%d id %d ev %d at (%d,%d)",
                s->frame_counter, (unsigned)(MT_FRAME_PREFIX_LEN + data_len),
                n, report[i], fd[i].id, fd[i].event, fd[i].x, fd[i].y);
        }
    }
    s->frame_counter += 1;

    *out_len = MT_FRAME_PREFIX_LEN + data_len;
    assert(*out_len <= MT_FRAME_ALLOC);
    return frame;
}

static void ipod_touch_multitouch_inform_frame_ready(IPodTouchMultitouchState *s) {
    MTT("frame ready -> raise gpio3 bit13");
    s->sysic->gpio_int_status[3] |= (1 << 13); // the multitouch interrupt bit is in group 3 (32 interrupts per group), and the 13th of the 3th group
    qemu_irq_raise(s->sysic->gpio_irqs[3]);
}

/*
 * Touch report rate.
 *
 * A real Zephyr2 digitizer reports at roughly 60 Hz. This model reported at 10,
 * so during a drag the guest learned the finger position ten times a second
 * while the panel presents at 53-57 fps. A finger-tracked animation cannot be
 * smoother than its input, which is what made slide-to-unlock feel sluggish and
 * drop frames even though the compositor was measurably fine.
 *
 * The rate also sets the reported speed, because get_frame() derives velocity
 * as (distance since the last host sample) / (time since the last frame). At
 * 10 Hz that divided a single mouse step by 100 ms and under-reported finger
 * speed by about 6x, which is its own reason the slide-to-unlock recogniser
 * struggled. Raising the rate fixes the magnitude as well as the smoothness.
 *
 * IT_MT_HZ overrides it, for bisecting against the old behaviour (IT_MT_HZ=10).
 */
static int64_t mt_frame_period_ns(void)
{
    static int64_t ns = -1;

    if (ns < 0) {
        const char *e = getenv("IT_MT_HZ");
        int hz = e ? atoi(e) : 0;

        if (hz <= 0 || hz > 250) {
            hz = 60;
        }
        ns = NANOSECONDS_PER_SECOND / hz;
    }
    return ns;
}

/* Is any finger in contact right now? */
static bool mt_any_down(IPodTouchMultitouchState *s)
{
    for (int i = 0; i < MT_MAX_FINGERS; i++) {
        if (s->fingers[i].phase == MT_FINGER_DOWN) {
            return true;
        }
    }
    return false;
}

/* Is there anything at all to report? */
static bool mt_any_active(IPodTouchMultitouchState *s)
{
    for (int i = 0; i < MT_MAX_FINGERS; i++) {
        if (s->fingers[i].phase != MT_FINGER_FREE) {
            return true;
        }
    }
    return false;
}

/*
 * Emit one frame carrying every finger currently being reported, and advance
 * each finger's phase past what that frame just said.
 *
 * All reporting goes through here. The single-finger code emitted a frame per
 * event with the event baked into the call; with several fingers the events
 * differ per finger within the same frame, so the phase has to live on the
 * finger and the frame is a snapshot of all of them.
 */
static void mt_emit_frame(IPodTouchMultitouchState *s)
{
    uint32_t len;
    bool down_after;
    int i;

    /* Slot 0's position is written straight into touch_x/touch_y by the
     * pointer paths, so pick it up here rather than duplicating it. */
    s->fingers[0].x = s->touch_x;
    s->fingers[0].y = s->touch_y;

    /*
     * A frame the guest never collected. next_frame is a single slot and the
     * guest polls slower than we can produce, so this used to leak one frame
     * buffer per skipped report. It is never aliased into out_buffer without
     * being cleared, so freeing here is safe.
     */
    free(s->next_frame);
    s->next_frame = mt_build_frame(s, s->fingers, &len);
    s->next_frame_len = len;

    /* mt_build_frame advanced prev_x/prev_y for the fingers it reported. */
    s->prev_touch_x = s->fingers[0].prev_x;
    s->prev_touch_y = s->fingers[0].prev_y;

    for (i = 0; i < MT_MAX_FINGERS; i++) {
        if (s->fingers[i].phase == MT_FINGER_DOWN) {
            s->fingers[i].started = true;
        }
    }
    down_after = mt_any_down(s);
    for (i = 0; i < MT_MAX_FINGERS; i++) {
        MTFingerState *f = &s->fingers[i];

        switch (f->phase) {
        case MT_FINGER_LIFTED:
            /*
             * TOUCH_ENDED has now been reported. The extra TOUCH_FULL_END frame
             * is the end of the whole gesture, so it is only owed when nothing
             * is left in contact; a finger lifting out of a pinch just goes.
             */
            f->phase = down_after ? MT_FINGER_FREE : MT_FINGER_ENDING;
            break;
        case MT_FINGER_ENDING:
            f->phase = MT_FINGER_FREE;
            break;
        default:
            break;
        }
        if (f->phase == MT_FINGER_FREE) {
            f->started = false;
        }
    }

    s->touch_down = (s->fingers[0].phase == MT_FINGER_DOWN);
    ipod_touch_multitouch_inform_frame_ready(s);
}

/*
 * A finger arriving cancels a pending end-of-gesture: the FULL_END frame would
 * otherwise land in the middle of the new contact.
 */
static void mt_cancel_pending_end(IPodTouchMultitouchState *s)
{
    for (int i = 0; i < MT_MAX_FINGERS; i++) {
        if (s->fingers[i].phase == MT_FINGER_ENDING) {
            s->fingers[i].phase = MT_FINGER_FREE;
            s->fingers[i].started = false;
        }
    }
    if (s->touch_end_timer) {
        timer_del(s->touch_end_timer);
    }
}

void ipod_touch_multitouch_set_finger(IPodTouchMultitouchState *s, int slot,
                                      float x, float y, bool down)
{
    MTFingerState *f;
    bool state_change = false;
    int64_t now;

    if (slot < 0 || slot >= MT_MAX_FINGERS) {
        return;
    }
    f = &s->fingers[slot];

    /*
     * A contact that leaves the panel ends, it does not slide along the bezel.
     * The pointer paths cannot produce this -- QEMU clamps absolute axes to the
     * window -- but synthesised multi-touch can: a pinch driven from a single
     * host pointer pushes its mirrored second finger off the edge long before
     * the real one gets there. The original iPhone Simulator made this an
     * explicit transition (_dragWentOffScreen) rather than leaving a stuck
     * contact, and a stuck contact here is worse than a lost one: the guest
     * would keep a finger down that nothing can ever lift.
     */
    if (down && (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f)) {
        if (f->phase != MT_FINGER_DOWN) {
            return;
        }
        down = false;
        x = f->x;
        y = f->y;
    }

    if (slot == 0) {
        s->touch_x = x;
        s->touch_y = y;
    }
    f->x = x;
    f->y = y;

    if (down && f->phase != MT_FINGER_DOWN) {
        MTT("FINGER %d DOWN at (%.3f, %.3f)", slot, x, y);
        mt_cancel_pending_end(s);
        f->phase = MT_FINGER_DOWN;
        f->started = false;
        /*
         * A new touch has no speed. Without this the first frame measures the
         * distance from wherever this slot's previous contact ended, so two
         * quick taps in different places report a large spurious velocity.
         */
        f->prev_x = x;
        f->prev_y = y;
        state_change = true;
    } else if (!down && f->phase == MT_FINGER_DOWN) {
        MTT("FINGER %d UP at (%.3f, %.3f)", slot, x, y);
        f->phase = MT_FINGER_LIFTED;
        state_change = true;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!state_change) {
        /* Pure motion. Nothing to say about a finger that is not in contact. */
        if (f->phase != MT_FINGER_DOWN) {
            return;
        }
        /*
         * Coalesce to the report rate. A host that delivers motion faster than
         * the panel reports would otherwise storm the guest with GPIO
         * interrupts, and next_frame is a single slot so the extra frames would
         * be discarded anyway.
         */
        if (now - s->last_motion_ns < mt_frame_period_ns()) {
            return;
        }
    }
    s->last_motion_ns = now;
    mt_emit_frame(s);

    if (mt_any_down(s)) {
        timer_mod(s->touch_timer, now + mt_frame_period_ns());
    } else {
        timer_del(s->touch_timer);
        if (mt_any_active(s)) {
            timer_mod(s->touch_end_timer, now + NANOSECONDS_PER_SECOND / 10);
        }
    }
}

void ipod_touch_multitouch_on_touch(IPodTouchMultitouchState *s) {
    ipod_touch_multitouch_set_finger(s, 0, s->touch_x, s->touch_y, true);
}

/*
 * Host pointer motion while the finger is down.
 *
 * The timer alone only resamples the position on its own cadence, so it turns
 * continuous host motion into a staircase. Reporting here as well makes the
 * guest see motion when it actually happens, with the timer left as a
 * keep-alive floor for a finger that is held still.
 */
void ipod_touch_multitouch_on_motion(IPodTouchMultitouchState *s) {
    /* Motion never starts a contact -- callers use on_touch for that. */
    if (s->fingers[0].phase != MT_FINGER_DOWN) {
        return;
    }
    ipod_touch_multitouch_set_finger(s, 0, s->touch_x, s->touch_y, true);
}

void ipod_touch_multitouch_on_release(IPodTouchMultitouchState *s) {
    ipod_touch_multitouch_set_finger(s, 0, s->touch_x, s->touch_y, false);
}

static void touch_timer_tick(void *opaque)
{
    IPodTouchMultitouchState *s = (IPodTouchMultitouchState *)opaque;

    if (!mt_any_active(s)) {
        return;
    }
    mt_emit_frame(s);

    if (mt_any_down(s)) {
        s->last_motion_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_mod(s->touch_timer, s->last_motion_ns + mt_frame_period_ns());
    }
}

static void touch_end_timer_tick(void *opaque)
{
    IPodTouchMultitouchState *s = (IPodTouchMultitouchState *)opaque;

    if (!mt_any_active(s)) {
        return;
    }
    mt_emit_frame(s);
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

    free(s->next_frame);
    s->next_frame = NULL;
    s->next_frame_len = 0;
    s->frame_counter = 0;
    memset(s->fingers, 0, sizeof(s->fingers));
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

/*
 * KNOWN GAP, deliberate: out_buffer/in_buffer and next_frame are heap pointers
 * holding an SPI transaction and a touch report in flight. They are not
 * migrated, so a snapshot taken with a finger down restores as a finger that
 * was never pressed. post_load therefore forces the released state rather than
 * leaving touch_down set with no frame to deliver -- otherwise the guest would
 * wait for a TOUCH_ENDED that can no longer arrive. Snapshots are taken at
 * rest in practice; this only bounds the damage if one is not.
 */
static int ipod_touch_multitouch_post_load(void *opaque, int version_id)
{
    IPodTouchMultitouchState *s = opaque;

    s->next_frame = NULL;
    s->next_frame_len = 0;
    s->buf_ind = 0;
    s->in_buffer_ind = 0;
    memset(s->fingers, 0, sizeof(s->fingers));
    s->touch_down = false;
    if (s->touch_timer) {
        timer_del(s->touch_timer);
    }
    if (s->touch_end_timer) {
        timer_del(s->touch_end_timer);
    }
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_multitouch = {
    .name = "ipod_touch_multitouch",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ipod_touch_multitouch_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(ssidev, IPodTouchMultitouchState),
        VMSTATE_UINT8(cur_cmd, IPodTouchMultitouchState),
        VMSTATE_UINT8(prev_cmd_log, IPodTouchMultitouchState),
        VMSTATE_UINT32(buf_size, IPodTouchMultitouchState),
        VMSTATE_UINT8_ARRAY(hbpp_atn_ack_response, IPodTouchMultitouchState, 2),
        VMSTATE_UINT32(frame_counter, IPodTouchMultitouchState),
        VMSTATE_UINT64(last_frame_timestamp, IPodTouchMultitouchState),
        VMSTATE_INT64(last_motion_ns, IPodTouchMultitouchState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_multitouch_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = ipod_touch_multitouch_realize;
    k->transfer = ipod_touch_multitouch_transfer;
    dc->reset = ipod_touch_multitouch_reset;
    dc->vmsd = &vmstate_ipod_touch_multitouch;
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
