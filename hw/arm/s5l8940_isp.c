/*
 * H4 ISP control transport and virtual-sensor bring-up. This model implements
 * the device protocol; it does not inject guest code or replace guest drivers.
 * No Apple firmware, camera calibration or image assets are distributed.
 *
 * Bootstrap and channel ABI cross-checked against patjak/facetimehd's GPL-2.0
 * driver (fthd_isp.c, fthd_ringbuf.c) and the native H4 driver.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

#define TYPE_H4_ISP "apple-h4-isp"
OBJECT_DECLARE_SIMPLE_TYPE(H4ISPState, H4_ISP)
#define H4_CHANNELS 7
#define H4_SLOTS 8
#define H4_QUEUE_SIZE 0x6000
#define H4_BUFFERS 64

typedef struct H4Buffer {
    uint8_t descriptor[40];
    unsigned channel;
} H4Buffer;

typedef struct H4SensorState {
    uint32_t mode, mipi_mode, width, height, format, stride_y, stride_uv;
    uint32_t crop_x, crop_y, crop_w, crop_h, recycle_mode;
    uint16_t fps, max_fps, min_fps;
    uint16_t still_pending;
    uint8_t focus_frames;
    bool started, recycling, returning, video_range;
    bool temporal_filter, motion_history, filter_enabled;
} H4SensorState;

struct H4ISPState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    DeviceState *dart;
    uint32_t regs[0x100000 / 4];
    uint32_t firmware_base, queue_base;
    unsigned stage;
    H4SensorState sensor[2];
    uint32_t exposure_tag[2];
    uint16_t exposure_bias[2];
    bool trace, trace_mmio;
    unsigned output_trace_frames;
    unsigned buffer_trace_count;
    QEMUTimer *frame_timer;
    H4Buffer buffers[H4_BUFFERS];
    unsigned buffer_count, active_channel, completion_slot, frames;
    uint32_t descriptor_size;
    uint32_t completion_base;
    bool memory_requested;
};

static const struct {
    const char *name;
    unsigned type;
    unsigned source;
} h4_channels[] = {
    {"SHAREDMALLOC", 1, 0}, {"TERMINAL", 1, 1}, {"BUF_H2T", 0, 2},
    {"BUF_T2H", 1, 2},      {"IO", 0, 3},       {"IO_T2H", 1, 3},
    {"DEBUG", 0, 3},
};

static bool h4_dma(H4ISPState *s, uint32_t iova, void *buf, size_t len,
                   bool write)
{
    while (len) {
        size_t n = MIN(len, 0x1000 - (iova & 0xfff));
        hwaddr pa = s5l8930_dart_dev_xlate(s->dart, 0, iova);
        if (pa == (hwaddr)-1) {
            if (s->trace)
                fprintf(stderr, "H4 DMA unmapped %08x len=%zu write=%u\n", iova,
                        len, write);
            return false;
        }
        MemTxResult result =
            write ? address_space_write(&address_space_memory, pa,
                                        MEMTXATTRS_UNSPECIFIED, buf, n)
                  : address_space_read(&address_space_memory, pa,
                                       MEMTXATTRS_UNSPECIFIED, buf, n);
        if (result != MEMTX_OK) {
            return false;
        }
        iova += n;
        buf = (uint8_t *)buf + n;
        len -= n;
    }
    return true;
}

static void h4_irq(H4ISPState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[0x41000 / 4] & s->regs[0x41004 / 4]));
}

static void h4_schedule(H4ISPState *s)
{
    if (s->frame_timer && s->buffer_count) {
        timer_mod(s->frame_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000 / 15);
    }
}

/* BUF_H2T uses flags in word 2, rather than the IO response byte count.
 * Copy descriptors before acknowledging: the driver immediately reuses the
 * command allocation after seeing ownership return to the host. */
static bool h4_buffer_layout(const uint8_t *packet, unsigned count,
                             unsigned stride)
{
    if (!count || count > (0x180 - 8) / stride)
        return false;
    for (unsigned n = 0; n < count; n++) {
        const uint8_t *d = packet + 8 + n * stride;
        unsigned planes = ldl_le_p(d + 24);
        if (!ldl_le_p(d) || !ldl_le_p(d + 32) || planes < 1 || planes > 2)
            return false;
    }
    return true;
}

static bool h4_submit_buffers(H4ISPState *s, uint32_t addr, unsigned size)
{
    uint8_t packet[0x180];
    if (size != sizeof(packet) ||
        !h4_dma(s, s->firmware_base + (addr & ~3u), packet, size, false)) {
        return false;
    }
    unsigned count = ldl_le_p(packet + 4);
    if (ldl_le_p(packet) != 1 || !count ||
        count > H4_BUFFERS - s->buffer_count) {
        return false;
    }
    /* Later H4 drivers append a word to each descriptor. Infer the wire
     * layout from a complete, validated list rather than an OS version.
     * Single-descriptor lists have the same meaningful first 36 bytes. */
    unsigned stride = s->descriptor_size ? s->descriptor_size : 36;
    if (!h4_buffer_layout(packet, count, stride)) {
        unsigned alternate = stride == 36 ? 40 : 36;
        if (!h4_buffer_layout(packet, count, alternate))
            return false;
        stride = alternate;
    }
    s->descriptor_size = stride;
    for (unsigned n = 0; n < count; n++) {
        H4Buffer *buffer = &s->buffers[s->buffer_count++];
        memset(buffer->descriptor, 0, sizeof(buffer->descriptor));
        memcpy(buffer->descriptor, packet + 8 + n * stride, stride);
        buffer->channel = s->active_channel;
        if (s->trace && s->output_trace_frames < 20) {
            fprintf(stderr, "H4 SUBMIT ch=%u", buffer->channel);
            for (unsigned k = 0; k < 36; k += 4)
                fprintf(stderr, " %08x", ldl_le_p(buffer->descriptor + k));
            fprintf(stderr, "\n");
        }
    }
    h4_schedule(s);
    return true;
}

/* Original 5x7 glyphs for the two virtual-sensor labels. */
static int h4_label(unsigned channel, unsigned x, unsigned y, unsigned width,
                    unsigned height)
{
    static const struct {
        char letter;
        uint8_t rows[7];
    } glyphs[] = {
        {'B', {30, 17, 17, 30, 17, 17, 30}},
        {'A', {14, 17, 17, 31, 17, 17, 17}},
        {'C', {14, 17, 16, 16, 16, 17, 14}},
        {'K', {17, 18, 20, 24, 20, 18, 17}},
        {'F', {31, 16, 16, 30, 16, 16, 16}},
        {'R', {30, 17, 17, 30, 20, 18, 17}},
        {'O', {14, 17, 17, 17, 17, 17, 14}},
        {'N', {17, 25, 25, 21, 19, 19, 17}},
        {'T', {31, 4, 4, 4, 4, 4, 4}},
    };
    const char *label = channel ? "FRONT" : "BACK";
    unsigned scale = MAX(width / 160, 1), length = strlen(label);
    unsigned box_width = (length * 6 + 4) * scale;
    unsigned box_height = 11 * scale;
    if (box_width > width || box_height > height)
        return -1;
    unsigned left = (width - box_width) / 2, top = (height - box_height) / 2;
    if (x < left || x >= left + box_width || y < top || y >= top + box_height)
        return -1;
    x = (x - left) / scale;
    y = (y - top) / scale;
    if (x < 2 || y < 2 || y >= 9)
        return 16;
    unsigned letter = (x - 2) / 6, column = (x - 2) % 6;
    if (letter >= length || column >= 5)
        return 16;
    for (unsigned i = 0; i < G_N_ELEMENTS(glyphs); i++) {
        if (glyphs[i].letter == label[letter]) {
            return glyphs[i].rows[y - 2] & (1 << (4 - column)) ? 235 : 16;
        }
    }
    return 16;
}

static bool h4_pattern(H4ISPState *s, H4Buffer *buffer)
{
    static const uint8_t bars[8][3] = {
        {235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
        {106, 202, 222}, {81, 90, 240},  {41, 240, 110}, {16, 128, 128},
    };
    if (buffer->channel >= 2) {
        return false;
    }
    H4SensorState *sensor = &s->sensor[buffer->channel];
    unsigned w = sensor->width, h = sensor->height;
    /* Surface plane addresses, like IPC addresses, are relative to the
     * firmware DART allocation's origin (native driver's ISP buffer ABI). */
    uint32_t yva = s->firmware_base + ldl_le_p(buffer->descriptor);
    uint32_t uvva = s->firmware_base + ldl_le_p(buffer->descriptor + 4);
    uint8_t row[16384];
    if (w > 4096 || h > 4096 || sensor->format > 2 ||
        sensor->stride_y > sizeof(row) || sensor->stride_uv > sizeof(row) ||
        sensor->stride_y < w * (sensor->format ? 2 : 1)) {
        return false;
    }
    if (ldl_le_p(buffer->descriptor + 24) == 1) {
        uint8_t metadata[0x1000] = {0};
        stl_le_p(metadata + 4,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 24 / 1000);
        stl_le_p(metadata + 8, s->frames);
        stl_le_p(metadata + 12, 10);
        stl_le_p(metadata + 16, 64);
        stl_le_p(metadata + 16 + 2 * 4, 0x140); /* Exposure */
        stl_le_p(metadata + 16 + 3 * 4, 0x700); /* Lens actuator */
        stl_le_p(metadata + 16 + 7 * 4, 0x180); /* AE */
        stl_le_p(metadata + 16 + 8 * 4, 0x400); /* AF */
        stl_le_p(metadata + 16 + 9 * 4, 0x600); /* AWB */
        stl_le_p(metadata + 64, buffer->channel ? 0x9760 : 0x145);
        stl_le_p(metadata + 64 + 8, buffer->channel ? 0 : 1);
        stw_le_p(metadata + 64 + 0x18, w);
        stw_le_p(metadata + 64 + 0x1a, h);
        stw_le_p(metadata + 64 + 0x24, w); /* Sensor crop width / height. */
        stw_le_p(metadata + 64 + 0x26, h);
        stl_le_p(metadata + 64 + 0x30, 30 << 8);
        stw_le_p(metadata + 64 + 0x40, 256); /* Unity binning gain. */
        stw_le_p(metadata + 64 + 0x38, w);   /* Scaler crop width / height. */
        stw_le_p(metadata + 64 + 0x3a, h);
        metadata[64 + 0x2a] = sensor->still_pending != 0;
        /* Native H4 posts PreFlashComplete for count 1 and FlashFired for
         * count 2. Both records travel with an image, as on the older ISP. */
        if (sensor->still_pending && sensor->still_pending <= 2) {
            stw_le_p(metadata + 64 + 0x28, 3 - sensor->still_pending);
        }
        stl_le_p(metadata + 0x140, 800000); /* 1/30 s at 24 MHz. */
        stw_le_p(metadata + 0x144, 256);
        stw_le_p(metadata + 0x146, 256);
        stw_le_p(metadata + 0x148, 256);
        stl_le_p(metadata + 0x150, s->exposure_tag[buffer->channel]);
        stl_le_p(metadata + 0x180, 1); /* AE stable */
        stl_le_p(metadata + 0x180 + 0xc, 128);
        stl_le_p(metadata + 0x180 + 0x10, 128);
        stl_le_p(metadata + 0x180 + 0x234, 1); /* Fully exposed */
        stw_le_p(metadata + 0x400 + 0x16, sensor->focus_frames > 1);
        stw_le_p(metadata + 0x400 + 0x18, sensor->focus_frames <= 1);
        stw_le_p(metadata + 0x600 + 4, 1); /* AWB stable */
        stl_le_p(metadata + 0x600 + 8, 6500);
        stw_le_p(metadata + 0x600 + 0xc, 256);
        stw_le_p(metadata + 0x600 + 0xe, 256);
        stw_le_p(metadata + 0x600 + 0x10, 256);
        /* A stationary virtual lens still has a current actuator position.
         * Video stabilization uses this to select its lens calibration. */
        stw_le_p(metadata + 0x700, 512);
        return h4_dma(s, yva, metadata, sizeof(metadata), true);
    }
    uint8_t header[64] = {0};
    stl_le_p(header + 4, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 24 / 1000);
    stl_le_p(header + 8, s->frames);
    if (yva < 64 || !h4_dma(s, yva - 64, header, sizeof(header), true))
        return false;
    if (!w || !h || !yva || (!sensor->format && !uvva) ||
        (uint64_t)yva + (uint64_t)sensor->stride_y * h > UINT32_MAX ||
        (uint64_t)uvva + (uint64_t)sensor->stride_uv * (h / 2) > UINT32_MAX) {
        return false;
    }
    uint8_t baseline[16384];
    memset(baseline, 16, sensor->stride_y);
    for (unsigned x = 0; x < w; x++) {
        const uint8_t *c = bars[buffer->channel ? 7 - x * 8 / w : x * 8 / w];
        /* bounded: x < w <= 4096, at most two bytes per 16384-byte row. */
        baseline[x * (sensor->format ? 2 : 1)] =
            sensor->video_range ? c[0] : (c[0] - 16) * 255 / 219;
        if (sensor->format) {
            int chroma = c[((x & 1) ^ (sensor->format == 2)) ? 2 : 1];
            baseline[x * 2 + 1] =
                sensor->video_range
                    ? chroma
                    : CLAMP(128 + (chroma - 128) * 255 / 224, 0, 255);
        }
    }
    unsigned scale = MAX(w / 160, 1),
             box_w = (buffer->channel ? 34 : 28) * scale;
    unsigned box_h = 11 * scale, sweep = (s->frames * 8) % h;
    bool label_fits = box_w <= w && box_h <= h;
    for (unsigned y = 0; y < h; y++) {
        uint8_t *line = baseline;
        bool lit = y >= sweep && y < sweep + MAX(h / 32, 2);
        bool label_row =
            label_fits && y >= (h - box_h) / 2 && y < (h + box_h) / 2;
        if (lit || label_row) {
            memcpy(row, baseline, sensor->stride_y);
            line = row;
            if (lit)
                for (unsigned x = 0; x < w; x++)
                    row[x * (sensor->format ? 2 : 1)] =
                        sensor->video_range ? 235 : 255;
            if (label_row)
                for (unsigned x = (w - box_w) / 2; x < (w + box_w) / 2; x++) {
                    int luma = h4_label(buffer->channel, x, y, w, h);
                    if (luma >= 0)
                        row[x * (sensor->format ? 2 : 1)] =
                            sensor->video_range ? luma
                                                : (luma - 16) * 255 / 219;
                }
        }
        if (!h4_dma(s, yva + y * sensor->stride_y, line, sensor->stride_y,
                    true))
            return false;
    }
    if (!sensor->format) {
        memset(row, 128, sensor->stride_uv);
        for (unsigned x = 0; x < w; x += 2) {
            const uint8_t *c =
                bars[buffer->channel ? 7 - x * 8 / w : x * 8 / w];
            row[x] = sensor->video_range
                         ? c[1]
                         : CLAMP(128 + ((int)c[1] - 128) * 255 / 224, 0, 255);
            row[x + 1] =
                sensor->video_range
                    ? c[2]
                    : CLAMP(128 + ((int)c[2] - 128) * 255 / 224, 0, 255);
        }
        for (unsigned y = 0; y < h / 2; y++) {
            if (!h4_dma(s, uvva + y * sensor->stride_uv, row, sensor->stride_uv,
                        true))
                return false;
        }
    }
    return true;
}

static void h4_frame(void *opaque)
{
    H4ISPState *s = opaque;
    uint8_t entry[64], packet[0x180] = {0};
    uint32_t ring = s->queue_base + 0x1c00 + s->completion_slot * 64;
    if (!s->buffer_count || s->stage < 2)
        return;
    if (!s->completion_base) {
        if (!s->memory_requested) {
            memset(entry, 0, sizeof(entry));
            stl_le_p(entry + 4, 0x1000);
            h4_dma(s, s->queue_base + 0x1000, entry, sizeof(entry), true);
            s->memory_requested = true;
            s->regs[0x41000 / 4] |= 0x10;
            h4_irq(s);
        }
        h4_schedule(s);
        return;
    }
    H4Buffer *buffer = &s->buffers[0];
    if (buffer->channel >= 2) {
        return;
    }
    bool returning = s->sensor[buffer->channel].returning;
    if ((!s->sensor[buffer->channel].started && !returning) ||
        !h4_dma(s, ring, entry, sizeof(entry), false) ||
        !(ldl_le_p(entry) & 1)) {
        h4_schedule(s);
        return;
    }
    unsigned selected[10], count = 0;
    unsigned stride = s->descriptor_size ? s->descriptor_size : 36;
    bool image = false;
    for (unsigned n = 0;
         n < s->buffer_count && count < (sizeof(packet) - 8) / stride; n++) {
        if (s->buffers[n].channel != buffer->channel)
            continue;
        bool single_plane = ldl_le_p(s->buffers[n].descriptor + 24) == 1;
        if (!returning && !single_plane && image)
            continue;
        image |= !single_plane;
        selected[count++] = n;
    }
    bool success = !returning;
    for (unsigned n = 0; n < count; n++) {
        if (!returning)
            success &= h4_pattern(s, &s->buffers[selected[n]]);
        memcpy(packet + 8 + n * stride, s->buffers[selected[n]].descriptor,
               stride);
    }
    stl_le_p(packet, success ? 2 : 1);
    stl_le_p(packet + 4, count);
    uint32_t output = s->completion_base + s->completion_slot * sizeof(packet);
    if (!h4_dma(s, output, packet, sizeof(packet), true)) {
        h4_schedule(s);
        return;
    }
    memset(entry, 0, sizeof(entry));
    stl_le_p(entry, output - s->firmware_base);
    stl_le_p(entry + 4, sizeof(packet));
    stl_le_p(entry + 8, 0x30000000);
    h4_dma(s, ring, entry, sizeof(entry), true);
    if (s->trace && s->output_trace_frames < 20) {
        fprintf(stderr, "H4 FRAME %u ch=%u tag=%08x success=%u pending=%u\n",
                s->frames, buffer->channel, ldl_le_p(buffer->descriptor + 32),
                success, s->buffer_count);
    }
    s->frames++;
    s->output_trace_frames++;
    if (success && image && s->sensor[buffer->channel].still_pending) {
        s->sensor[buffer->channel].still_pending--;
    }
    if (success && image && s->sensor[buffer->channel].focus_frames) {
        s->sensor[buffer->channel].focus_frames--;
    }
    s->completion_slot = (s->completion_slot + 1) % H4_SLOTS;
    unsigned retained = 0, completed = 0;
    for (unsigned n = 0; n < s->buffer_count; n++) {
        if (completed < count && selected[completed] == n)
            completed++;
        else
            s->buffers[retained++] = s->buffers[n];
    }
    s->buffer_count = retained;
    s->regs[0x41000 / 4] |= 0x40;
    h4_irq(s);
    h4_schedule(s);
}

static void h4_publish_channels(H4ISPState *s)
{
    uint8_t table[H4_CHANNELS * 256] = {0};
    uint8_t ring[H4_SLOTS * 64] = {0};
    uint32_t relative = s->queue_base - s->firmware_base;
    for (unsigned i = 0; i < H4_CHANNELS; i++) {
        uint8_t *d = table + i * 256;
        pstrcpy((char *)d, 64, h4_channels[i].name);
        stl_le_p(d + 0x40, h4_channels[i].type);
        /* Source is a doorbell selector, not the channel table index.
         * H4 exposes four selectors at interrupt bits 4..7. */
        stl_le_p(d + 0x44, h4_channels[i].source);
        stl_le_p(d + 0x48, H4_SLOTS);
        stl_le_p(d + 0x4c, relative + 0x1000 + i * 0x400);
        memset(ring, 0, sizeof(ring));
        /* Empty H2T slots are available to the host (bit 0 set).
         * Empty T2H slots must also start with bit 0 set: the host consumes
         * those only after the device publishes a message with bit 0 clear. */
        for (unsigned j = 0; j < H4_SLOTS; j++) {
            stl_le_p(ring + j * 64, 1);
        }
        h4_dma(s, s->queue_base + 0x1000 + i * 0x400, ring, sizeof(ring), true);
    }
    if (!h4_dma(s, s->queue_base, table, sizeof(table), true)) {
        if (s->trace)
            fprintf(stderr, "H4 IPC descriptor DMA failed base=%08x\n",
                    s->queue_base);
        return;
    }
    s->regs[0xc3000 / 4] = relative;
    s->regs[0x41000 / 4] |= 0x10;
    s->stage = 2;
    h4_irq(s);
}

/* Channel controls for a generated sensor. Physical lens/AE processing does
 * not alter the deterministic test image, but transport, geometry, selection,
 * and streaming state follow the native driver's command ABI. */
static uint16_t h4_io_command(H4ISPState *s, uint16_t op, uint8_t *p,
                              unsigned request, unsigned response)
{
    uint32_t ch = request >= 4 ? ldl_le_p(p) : UINT32_MAX;
    uint32_t value = request >= 8 ? ldl_le_p(p + 4) : 0;
    H4SensorState *sensor = ch < 2 ? &s->sensor[ch] : NULL;

    switch (op) {
    case 0: /* ISP start */
    case 2: /* ISP reset */
        return 0;
    case 1:   /* ISP stop */
    case 0xa: /* ISP power down */
        s->sensor[0].started = s->sensor[1].started = false;
        return 0;
    case 4: /* Print enable: no embedded firmware console in this model. */
        return request >= 4 ? 0 : 1;
    case 3: /* ISP configuration */
        if (response < 20)
            return 1;
        memset(p, 0, response);
        stl_le_p(p, 24000000); /* Timestamp frequency Hz. */
        stl_le_p(p + 4, 2);
        return 0;
    case 6: /* Build information */
        memset(p, 0, response);
        return 0;
    case 0x10d: /* Channel information */
        if (!sensor || response < 132)
            return 1;
        memset(p, 0, response);
        stl_le_p(p, ch);
        stl_le_p(p + 24, ch ? 0x9760 : 0x0145);
        /* Native H4 clients gate flash properties on this capability. The
         * rear pattern sensor has a virtual strobe; the front does not.
         * The separate torch capability remains absent. */
        stl_le_p(p + 40, ch ? 0 : 1);
        stl_le_p(p + 88, ch ? 5 : 12); /* Mode count, not sensor count. */
        return 0;
    case 0x106: /* Camera configuration get */
        if (!sensor || request < 8 || response < 72 || value >= (ch ? 5 : 12))
            return 1;
        memset(p, 0, response);
        stl_le_p(p, ch);
        stl_le_p(p + 4, value);
        uint16_t width = ch ? 1472 : 3264;
        uint16_t height = ch ? 960 : 2448;
        stw_le_p(p + 8, width);
        stw_le_p(p + 10, height);
        /* Native H4ISP computes VideoScaleFactor from the active width.
         * A zero width creates infinity and crashes Celestial's format picker. */
        stw_le_p(p + 12, width); /* Active dimensions. */
        stw_le_p(p + 14, height);
        stw_le_p(p + 20, 30 << 8); /* Maximum / minimum FPS, Q8.8. */
        stw_le_p(p + 22, 1 << 8);
        return 0;
    }
    if (ch >= 2)
        return 1;
    switch (op) {
    case 0x107: /* Camera configuration select */
        if (request < 8 || value >= (ch ? 5 : 12))
            return 1;
        sensor->mode = value;
        return 0;
    case 0x100: /* Channel start: frame delivery waits for queued buffers. */
        s->active_channel = ch;
        sensor->returning = false;
        sensor->started = true;
        h4_schedule(s);
        return 0;
    case 0x101: /* Channel stop */
        sensor->started = false;
        return 0;
    case 0x104: /* Buffer return */
        sensor->returning = true;
        h4_schedule(s);
        return 0;
    case 0x10e: /* Buffer recycle mode */
        if (request < 8)
            return 1;
        sensor->recycle_mode = value;
        return 0;
    case 0x10f: /* Buffer recycle start / stop */
    case 0x110:
        sensor->recycling = op == 0x10f;
        return 0;
    case 0x111: /* Guest-supplied physical-sensor tuning is unnecessary for bars. */
        return request >= 12 ? 0 : 1;
    case 0x11b: /* MIPI frequency selection: generated sensor has no CSI link. */
        if (request < 8)
            return 1;
        sensor->mipi_mode = value;
        return 0;
    case 0x208: /* AE maximum frame rate */
    case 0x20a: /* AE minimum frame rate */
        if (request < 6 || lduw_le_p(p + 4) > (30 << 8))
            return 1;
        if (op == 0x208)
            sensor->max_fps = lduw_le_p(p + 4);
        else
            sensor->min_fps = lduw_le_p(p + 4);
        return 0;
    case 0x500: /* Sensor frame rate, Q8.8 */
        if (request < 6 || !lduw_le_p(p + 4) || lduw_le_p(p + 4) > (30 << 8))
            return 1;
        sensor->fps = lduw_le_p(p + 4);
        return 0;
    case 0x204: /* Exposure bias and caller's frame acknowledgement token. */
        if (request != 8 && request < 12)
            return 1;
        /* bounded: ch < 2 is checked before the channel-command switch. */
        s->exposure_bias[ch] = lduw_le_p(p + 4);
        /* Earlier H4 clients send only channel and bias, without a token. */
        /* bounded: ch < 2 is checked before the channel-command switch. */
        s->exposure_tag[ch] = request >= 12 ? ldl_le_p(p + 8) : 0;
        if (s->trace) {
            fprintf(stderr, "H4 EXPOSURE ch=%u bias=%u tag=%u\n", ch,
                    s->exposure_bias[ch], s->exposure_tag[ch]);
        }
        return 0;
    case 0x203: /* Exposure bias query. Generated pixels remain deterministic. */
        if (response < 6)
            return 1;
        /* bounded: ch < 2 is checked before the channel-command switch. */
        stw_le_p(p + 4, s->exposure_bias[ch]);
        return 0;
    case 0x21c: /* AE strobe/capture request: preflash, then main exposure. */
        if (ch || request < 24)
            return 1;
        if (s->trace) {
            fprintf(stderr, "H4 CAPTURE ch=%u request=%u payload=", ch,
                    request);
            for (unsigned i = 0; i < MIN(request, 32u); i++)
                fprintf(stderr, "%02x", p[i]);
            fprintf(stderr, "\n");
        }
        if (lduw_le_p(p + 20) == 0xffff)
            sensor->still_pending = 2;
        return 0;
    case 0x801: { /* Crop rectangle */
        if (request < 20)
            return 1;
        uint32_t x = value, y = ldl_le_p(p + 8);
        uint32_t w = ldl_le_p(p + 12), h = ldl_le_p(p + 16);
        if (!w || !h || (uint64_t)x + w > (ch ? 1472 : 3264) ||
            (uint64_t)y + h > (ch ? 960 : 2448))
            return 1;
        sensor->crop_x = x;
        sensor->crop_y = y;
        sensor->crop_w = w;
        sensor->crop_h = h;
        return 0;
    }
    case 0xb01: { /* Output geometry, format and per-plane strides. */
        if (request < 32)
            return 1;
        uint32_t w = value, h = ldl_le_p(p + 8), format = ldl_le_p(p + 16);
        uint32_t stride_y = ldl_le_p(p + 20), stride_uv = ldl_le_p(p + 24);
        if (!w || !h || w > 4096 || h > 4096 || format > 2 ||
            stride_y < w * (format ? 2 : 1) || stride_y > 16384 ||
            (!format && (stride_uv < w || stride_uv > 16384 || ((w | h) & 1))))
            return 1;
        if (s->trace)
            fprintf(
                stderr,
                "H4 OUTPUT ch=%u %ux%u format=%u video_range=%u stride=%u/%u\n",
                ch, w, h, format, ldl_le_p(p + 12), stride_y, stride_uv);
        sensor->width = w;
        sensor->height = h;
        sensor->format = format;
        s->output_trace_frames = s->buffer_trace_count = 0;
        sensor->stride_y = stride_y;
        sensor->stride_uv = stride_uv;
        sensor->video_range = ldl_le_p(p + 12) != 0;
        s->active_channel = ch;
        return 0;
    }
    case 0xa16: /* Neutral backend lens-adjustment settings. */
        if (response < 4)
            return 1;
        memset(p, 0, response);
        stl_le_p(p, ch);
        return 0;
    case 0xa15:  /* Backend lens adjustment parameters */
    case 0xc300: /* Tone curve adaptation start */
    case 0xd00:  /* Face detection start */
    case 0xd04:  /* Face detection disable */
    case 0xa14:  /* Disable manual image-processing knobs */
    case 0xd01:  /* Face detection stop */
    case 0xd05:  /* Face detection enable: the pattern contains no faces. */
    case 0xc302: /* Tone curve parameters */
    case 0x229:  /* AE stability-to-stable threshold */
    case 0x218:  /* AE convergence speed */
    case 0x21a:  /* AE stability */
    case 0xc00:  /* Dynamic range correction enable */
    case 0xc306: /* Tone curve manual control enable */
    case 0x8102: /* Context switching enable */
    case 0x8206: /* AE metering mode */
    case 0x820e: /* Face-detection AE metering configuration */
    case 0x406:  /* AF search positions */
    case 0x8401: /* AF metering windows */
    case 0x8404: /* Face-detection focus windows */
    case 0x8403: /* AF window weights */
        /* Pattern pixels are already exposed and color-corrected. These
         * controls do not change them, or require physical calibration data. */
        return 0;
    case 0x407: /* One-shot AF must report searching, then completion. */
        sensor->focus_frames = 2;
        return 0;
    case 0xc100: /* Temporal filter start / stop. */
    case 0xc101:
    case 0xc102: /* Motion history start / stop. */
    case 0xc103:
    case 0xc113: /* Temporal filter enable / disable. */
    case 0xc114:
        if (request < 4)
            return 1;
        /* The generated sensor has no noise to reduce and needs no firmware
         * history allocation. Preserve pixels through this virtual stage. */
        if (op == 0xc100 || op == 0xc101)
            sensor->temporal_filter = op == 0xc100;
        if (op == 0xc102 || op == 0xc103)
            sensor->motion_history = op == 0xc102;
        if (op == 0xc113 || op == 0xc114)
            sensor->filter_enabled = op == 0xc113;
        return 0;
    default:
        return 1;
    }
}

static void h4_commands(H4ISPState *s, uint32_t doorbell)
{
    if (s->memory_requested && !s->completion_base) {
        uint8_t allocation[64];
        if (h4_dma(s, s->queue_base + 0x1000, allocation, sizeof(allocation),
                   false)) {
            uint32_t addr = ldl_le_p(allocation);
            if ((addr & 1) && (addr & ~3u)) {
                s->completion_base = s->firmware_base + (addr & ~3u);
                if (s->trace)
                    fprintf(stderr, "H4 COMPLETION heap=%08x\n",
                            s->completion_base);
            }
        }
    }
    for (unsigned i = 0; i < H4_CHANNELS; i++) {
        if (h4_channels[i].type) {
            continue;
        }
        for (unsigned j = 0; j < H4_SLOTS; j++) {
            uint32_t entry = s->queue_base + 0x1000 + i * 0x400 + j * 64;
            uint8_t data[64], header[8];
            if (!h4_dma(s, entry, data, sizeof(data), false))
                continue;
            uint32_t addr = ldl_le_p(data), request = ldl_le_p(data + 4);
            uint32_t response = ldl_le_p(data + 8);
            if (i == 2 && !(addr & 1)) {
                if (h4_submit_buffers(s, addr, request)) {
                    stl_le_p(data, addr | 1);
                    stl_le_p(data + 8, 0);
                    h4_dma(s, entry, data, sizeof(data), true);
                    s->regs[0x41000 / 4] |= 0x40;
                }
                continue;
            }
            if (i == 2 && !(addr & 1) && s->trace &&
                s->buffer_trace_count++ < 16) {
                uint8_t payload[128] = {0};
                fprintf(stderr, "H4 BUFFER entry=");
                for (unsigned k = 0; k < 64; k += 4)
                    fprintf(stderr, "%08x,", ldl_le_p(data + k));
                if (h4_dma(s, s->firmware_base + (addr & ~3u), payload,
                           sizeof(payload), false)) {
                    fprintf(stderr, " payload=");
                    for (unsigned k = 0; k < sizeof(payload); k += 4)
                        fprintf(stderr, "%08x,", ldl_le_p(payload + k));
                }
                fprintf(stderr, "\n");
            }
            if ((addr & 1) || request < 8 || response < 8 || request > 65536 ||
                response > 65536)
                continue;
            uint32_t cmdaddr = s->firmware_base + (addr & ~3u);
            if (!h4_dma(s, cmdaddr, header, sizeof(header), false))
                continue;
            uint16_t opcode = lduw_le_p(header + 4);
            unsigned payload_size = MAX(request, response) - 8;
            uint8_t *payload = g_malloc0(MAX(payload_size, 1));
            uint16_t status = 1;
            if (h4_dma(s, cmdaddr + 8, payload, request - 8, false)) {
                status = h4_io_command(s, opcode, payload, request - 8,
                                       response - 8);
                if (!h4_dma(s, cmdaddr + 8, payload, response - 8, true))
                    status = 1;
            }
            g_free(payload);
            stw_le_p(header + 6, status);
            h4_dma(s, cmdaddr, header, sizeof(header), true);
            stl_le_p(data, addr | 1);
            stl_le_p(data + 8, 0);
            h4_dma(s, entry, data, sizeof(data), true);
            s->regs[0x41000 / 4] |= 1u << (4 + h4_channels[i].source);
            if (s->trace)
                fprintf(stderr, "H4 IPC %s cmd=%04x status=%u req=%u resp=%u\n",
                        h4_channels[i].name, opcode, status, request, response);
        }
    }
    h4_irq(s);
}

static uint64_t h4_read(void *opaque, hwaddr off, unsigned size)
{
    H4ISPState *s = opaque;
    uint32_t value = s->regs[off / 4];
    if (off == 0x90058 || off == 0x90068) {
        /* Native GetTime pairs this 24 MHz counter with mach_absolute_time.
         * Frame metadata uses this same clock domain, including wraparound. */
        value = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 24 / 1000;
    }
    if (s->trace_mmio && (off >= 0xc3000 || off == 0x41000))
        fprintf(stderr, "H4 R %05x %08x\n", (unsigned)off, value);
    return value;
}

static void h4_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    H4ISPState *s = opaque;
    if (s->trace_mmio && (off >= 0xc3000 || off == 0x41020 || off == 0x40004))
        fprintf(stderr, "H4 W %05x %08x stage%u\n", (unsigned)off,
                (unsigned)value, s->stage);
    if (off == 0x41024) {
        s->regs[0x41000 / 4] &= ~value;
        h4_irq(s);
        return;
    }
    s->regs[off / 4] = value;
    if (off == 0x40004) {
        if (value == 1) {
            s->stage = 1;
            s->regs[0xc3000 / 4] = H4_CHANNELS;
            s->regs[0xc3004 / 4] = H4_QUEUE_SIZE - 1;
            s->regs[0xc300c / 4] = 0x1000;
            s->regs[0x41000 / 4] |= 0x10;
        } else {
            timer_del(s->frame_timer);
            s->buffer_count = s->completion_slot = s->frames = 0;
            s->completion_base = 0;
            s->memory_requested = false;
            s->stage = 0;
            s->regs[0x41000 / 4] = 0;
        }
        h4_irq(s);
    } else if (off == 0xc3000 && s->stage == 1) {
        s->firmware_base = value;
    } else if (off == 0xc3004 && s->stage == 1) {
        s->queue_base = value + s->firmware_base;
    } else if (off == 0x41020) {
        if (s->stage == 1)
            h4_publish_channels(s);
        else if (s->stage >= 2)
            h4_commands(s, value);
    } else if (off == 0xc300c && value == 0x08042006 && s->stage == 2) {
        s->regs[off / 4] = 0;
        s->stage = 3;
    } else if (off == 0xc3000 && value == 0xf7fbdff9) {
        s->regs[off / 4] = 0x08042006;
    }
}

static const MemoryRegionOps h4_ops = {
    .read = h4_read,
    .write = h4_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};
static void h4_reset(DeviceState *dev)
{
    H4ISPState *s = H4_ISP(dev);
    timer_del(s->frame_timer);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->sensor, 0, sizeof(s->sensor));
    memset(s->exposure_tag, 0, sizeof(s->exposure_tag));
    memset(s->exposure_bias, 0, sizeof(s->exposure_bias));
    memset(s->buffers, 0, sizeof(s->buffers));
    s->firmware_base = s->queue_base = s->completion_base = 0;
    s->stage = s->buffer_count = s->active_channel = 0;
    s->completion_slot = s->frames = 0;
    s->descriptor_size = 0;
    s->memory_requested = false;
    s->output_trace_frames = s->buffer_trace_count = 0;
    h4_irq(s);
}

static int h4_post_load(void *opaque, int version_id)
{
    H4ISPState *s = opaque;
    if (version_id < 2) {
        memset(s->exposure_tag, 0, sizeof(s->exposure_tag));
        memset(s->exposure_bias, 0, sizeof(s->exposure_bias));
    }
    if (s->stage > 3 || s->buffer_count > H4_BUFFERS ||
        s->active_channel >= 2 || s->completion_slot >= H4_SLOTS ||
        (s->descriptor_size && s->descriptor_size != 36 &&
         s->descriptor_size != 40)) {
        return -EINVAL;
    }
    for (unsigned n = 0; n < s->buffer_count; n++) {
        if (s->buffers[n].channel >= 2) {
            return -EINVAL;
        }
    }
    for (unsigned n = 0; n < 2; n++) {
        H4SensorState *sensor = &s->sensor[n];
        if (sensor->width > 4096 || sensor->height > 4096 ||
            sensor->stride_y > 16384 || sensor->stride_uv > 16384 ||
            sensor->format > 2) {
            return -EINVAL;
        }
    }
    h4_irq(s);
    return 0;
}

static const VMStateDescription h4_buffer_vmstate = {
    .name = TYPE_H4_ISP "/buffer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){
        VMSTATE_UINT8_ARRAY(descriptor, H4Buffer, 40),
        VMSTATE_UINT32(channel, H4Buffer), VMSTATE_END_OF_LIST()}};
static const VMStateDescription h4_sensor_vmstate = {
    .name = TYPE_H4_ISP "/sensor",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (const VMStateField[]){VMSTATE_UINT32(mode, H4SensorState),
                               VMSTATE_UINT32(mipi_mode, H4SensorState),
                               VMSTATE_UINT32(width, H4SensorState),
                               VMSTATE_UINT32(height, H4SensorState),
                               VMSTATE_UINT32(format, H4SensorState),
                               VMSTATE_UINT32(stride_y, H4SensorState),
                               VMSTATE_UINT32(stride_uv, H4SensorState),
                               VMSTATE_UINT32(crop_x, H4SensorState),
                               VMSTATE_UINT32(crop_y, H4SensorState),
                               VMSTATE_UINT32(crop_w, H4SensorState),
                               VMSTATE_UINT32(crop_h, H4SensorState),
                               VMSTATE_UINT32(recycle_mode, H4SensorState),
                               VMSTATE_UINT16(fps, H4SensorState),
                               VMSTATE_UINT16(max_fps, H4SensorState),
                               VMSTATE_UINT16(min_fps, H4SensorState),
                               VMSTATE_UINT16(still_pending, H4SensorState),
                               VMSTATE_UINT8(focus_frames, H4SensorState),
                               VMSTATE_BOOL(started, H4SensorState),
                               VMSTATE_BOOL(recycling, H4SensorState),
                               VMSTATE_BOOL(returning, H4SensorState),
                               VMSTATE_BOOL(video_range, H4SensorState),
                               VMSTATE_BOOL(temporal_filter, H4SensorState),
                               VMSTATE_BOOL(motion_history, H4SensorState),
                               VMSTATE_BOOL(filter_enabled, H4SensorState),
                               VMSTATE_END_OF_LIST()}};
static const VMStateDescription h4_vmstate = {
    .name = TYPE_H4_ISP,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = h4_post_load,
    .fields = (const VMStateField[]){
        VMSTATE_UINT32_ARRAY(regs, H4ISPState, 0x100000 / 4),
        VMSTATE_UINT32(firmware_base, H4ISPState),
        VMSTATE_UINT32(queue_base, H4ISPState),
        VMSTATE_UINT32(stage, H4ISPState),
        VMSTATE_STRUCT_ARRAY(sensor, H4ISPState, 2, 1, h4_sensor_vmstate,
                             H4SensorState),
        VMSTATE_STRUCT_ARRAY(buffers, H4ISPState, H4_BUFFERS, 1,
                             h4_buffer_vmstate, H4Buffer),
        VMSTATE_UINT32(buffer_count, H4ISPState),
        VMSTATE_UINT32(active_channel, H4ISPState),
        VMSTATE_UINT32(completion_slot, H4ISPState),
        VMSTATE_UINT32(frames, H4ISPState),
        VMSTATE_UINT32(descriptor_size, H4ISPState),
        VMSTATE_UINT32(completion_base, H4ISPState),
        VMSTATE_BOOL(memory_requested, H4ISPState),
        VMSTATE_TIMER_PTR(frame_timer, H4ISPState),
        VMSTATE_UINT32_ARRAY_V(exposure_tag, H4ISPState, 2, 2),
        VMSTATE_UINT16_ARRAY_V(exposure_bias, H4ISPState, 2, 2),
        VMSTATE_END_OF_LIST()}};

static void h4_init(Object *obj)
{
    H4ISPState *s = H4_ISP(obj);
    memory_region_init_io(&s->mmio, obj, &h4_ops, s, TYPE_H4_ISP, 0x100000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    object_property_add_link(obj, "dart", TYPE_DEVICE, (Object **)&s->dart,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
    s->trace = getenv("IT_H4_IPC_TRACE") != NULL;
    s->trace_mmio = getenv("IT_H4_MMIO_TRACE") != NULL;
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, h4_frame, s);
}
static void h4_finalize(Object *obj)
{
    timer_free(H4_ISP(obj)->frame_timer);
}
static void h4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = "Apple H4 ISP with two virtual camera sensors";
    dc->vmsd = &h4_vmstate;
    device_class_set_legacy_reset(dc, h4_reset);
}
static const TypeInfo h4_info = {
    .name = TYPE_H4_ISP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(H4ISPState),
    .instance_init = h4_init,
    .instance_finalize = h4_finalize,
    .class_init = h4_class_init,
};
static void h4_register(void)
{
    type_register_static(&h4_info);
}
type_init(h4_register)
