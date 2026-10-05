/*
 * Zephyr1 multi-touch (the original iPhone, multi-touch,z1 on SPI2). The wire
 * protocol as openiBoot's plat-s5l8900/multitouch-z1.c drives it, every
 * transaction a chip-select frame (GPIO 0x0705), each answer clocked back by
 * byte index:
 *
 *   C2 a3 a2 a1 a0 00 data.. ckh ckl   bootloader data packet (0x400 bytes, A-Speed)
 *   C2 00 00 00                        blank packet: the next transaction streams
 *                                      the main firmware, raw, at speed
 *   05 00 00 06                     -> D0 00 sum_hi sum_lo (the last upload's byte sum)
 *   C4 00 00 C4                        execute what was uploaded
 *   D0 D0 D0 D0                     -> AA ver max_hi max_lo (interface version)
 *   8F id 8F..   (8)                -> AA x x x err|len_hi len_lo ck_hi ck_lo
 *   82 id 82..   (len+6)            -> AA x x x report[len] ck_hi ck_lo
 *   46|47|64|65 x8                  -> AA len_hi len_lo ck_hi ck_lo (frame length; interface
 *                                      versions above 0x10 put it at +4; 1.0's driver sends
 *                                      46, openiBoot 64/65)
 *   47|68 x(len+1)                  -> AA frame[len-2] ck_hi ck_lo (1.0: 47 right after a
 *                                      length read that offered a frame, else 47 is one)
 *
 * All checksums are 16-bit byte sums, big-endian on the wire. The firmware
 * images are only summed, never run: once "executed" the model answers as the
 * main firmware does. Frames are the shared model's (header + finger records,
 * the same layout openiBoot parses for both chips); proximity is not reported.
 * High-level emulation (fidelity class H) of the chip's firmware.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/arm/s5l8900_multitouch_z1.h"

#define Z1_MAX_PACKET 0x294        /* the Zephyr2 model's, which its frames fit */

/* MT_TRACE=1: bootloader and protocol events; 2: every transaction. */
static int z1_trace(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MT_TRACE");
        on = e ? MAX(atoi(e), 1) : 0;
    }
    return on;
}
#define Z1T(fmt, ...) do { if (z1_trace()) { \
    fprintf(stderr, "[Z1 %.3f] " fmt "\n", \
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, ##__VA_ARGS__); } } while (0)

static void (*z1_parent_realize)(SSIPeripheral *dev, Error **errp);
static DeviceReset z1_parent_reset;

/* A report's bytes (openiBoot's MT_INFO_*), 0 length for one the chip does not have. */
static uint32_t z1_report(IPodTouchMultitouchState *mt, uint8_t id, uint8_t *d)
{
    const MTSensorProfile *p = mt->profile;

    switch (id) {
    case MT_REPORT_FAMILY_ID:
        d[0] = p->family_id;
        return 1;
    case MT_REPORT_SENSOR_INFO:
        d[0] = MT_ENDIANNESS;
        d[1] = p->rows;
        d[2] = p->cols;
        d[3] = p->bcd_version >> 8;
        d[4] = p->bcd_version;
        return 5;
    case MT_REPORT_SENSOR_REGION_DESC:
        memcpy(d, p->region_desc, p->region_desc_len);
        return p->region_desc_len;
    case MT_REPORT_SENSOR_REGION_PARAM:
        memcpy(d, p->region_param, p->region_param_len);
        return p->region_param_len;
    case MT_REPORT_SENSOR_DIMENSIONS:
        stl_le_p(d, p->surface_width);
        stl_le_p(d + 4, p->surface_height);
        return 8;
    default:
        qemu_log_mask(LOG_UNIMP, "[Z1] report 0x%02x not modelled (empty)\n", id);
        return 0;
    }
}

static void z1_put16(uint8_t *d, uint32_t v)
{
    d[0] = v >> 8;
    d[1] = v;
}

/* Take the shared model's pending frame, if any, as the one to offer. */
static void z1_take_frame(S5L8900MultitouchZ1State *s)
{
    IPodTouchMultitouchState *mt = &s->parent_obj;
    MTFrame *f = mt->next_frame;

    if (s->frame || !f) {
        return;          /* the offered one not read yet: offered again */
    }
    uint32_t data_len = f->frame_length.length1 | f->frame_length.length2 << 8;
    s->frame_len = data_len - 2;   /* header + fingers, without the Zephyr2 trailing sum */
    s->frame = g_memdup2(&f->frame_packet.header, s->frame_len);
    g_free(mt->next_frame);
    mt->next_frame = NULL;
    mt->next_frame_len = 0;
}

/* The answer to a transaction, once its first bytes are in. */
static void z1_prepare(S5L8900MultitouchZ1State *s)
{
    IPodTouchMultitouchState *mt = &s->parent_obj;
    uint8_t *o = s->out;
    uint32_t sum = 0, n;

    memset(o, 0, sizeof(s->out));
    s->out_len = 0;
    switch (s->cmd) {
    case 0x05:                                   /* verify */
        o[0] = 0xD0;
        z1_put16(o + 2, s->upload_sum);
        s->out_len = 4;
        break;
    case 0xD0:                                   /* interface version */
        o[0] = 0xAA;
        o[1] = MT_INTERFACE_VERSION;
        z1_put16(o + 2, Z1_MAX_PACKET);
        s->out_len = 4;
        break;
    case 0x8F: {                                 /* report info */
        uint8_t d[64];
        n = z1_report(mt, s->in[1], d);
        o[0] = 0xAA;
        o[4] = n >> 8 & 0xF;
        o[5] = n;
        z1_put16(o + 6, s->in[1] + o[4] + o[5]);
        s->out_len = 8;
        break;
    }
    case 0x82:                                   /* report */
        n = z1_report(mt, s->in[1], o + 4);
        o[0] = 0xAA;
        sum = s->in[1];
        for (uint32_t i = 0; i < n; i++) {
            sum += o[4 + i];
        }
        z1_put16(o + 4 + n, sum);
        s->out_len = n + 6;
        break;
    case 0x46:
    case 0x64:
    case 0x65:                                   /* frame length */
        z1_take_frame(s);
        n = s->frame ? s->frame_len + 2 : 0;
        o[0] = 0xAA;
        /* AppleMultitouchSPI (1.0) takes the length at +1 from an interface
         * version up to 0x10, at +4 (openiBoot's layout) from later ones. */
        z1_put16(o + (MT_INTERFACE_VERSION > 0x10 ? 4 : 1), n);
        z1_put16(o + (MT_INTERFACE_VERSION > 0x10 ? 6 : 3), (n >> 8) + (n & 0xFF));
        s->out_len = 8;
        break;
    case 0x47:
    case 0x68:                                   /* frame data */
        o[0] = 0xAA;
        if (s->frame) {
            memcpy(o + 1, s->frame, s->frame_len);
            for (uint32_t i = 0; i < s->frame_len; i++) {
                sum += s->frame[i];
            }
            z1_put16(o + 1 + s->frame_len, sum);
            s->out_len = s->frame_len + 3;
        }
        break;
    default:
        break;
    }
}

/* The end of a transaction: chip select up, or its known length clocked. */
static void z1_finish(S5L8900MultitouchZ1State *s)
{
    if (z1_trace() >= 2) {
        char hex[3 * 16 + 1] = "", rx[3 * 8 + 1] = "";
        for (unsigned i = 0; i < MIN(s->index, 16u); i++) {
            sprintf(hex + 3 * i, " %02x", s->in[i]);
        }
        for (unsigned i = 0; i < MIN(s->out_len, 8u); i++) {
            sprintf(rx + 3 * i, " %02x", s->out[i]);
        }
        Z1T("xfer cmd %02x len %u tx%s | rx%s%s", s->cmd, s->index, hex, rx, s->streaming ? " (stream)" : "");
    }
    switch (s->cmd) {
    case 0xC2:
        if (s->index == 4 && !s->in[1] && !s->in[2] && !s->in[3]) {
            s->stream_next = true;               /* blank packet: the main firmware follows */
            Z1T("blank data packet");
        } else {
            s->upload_sum = s->sum & 0xFFFF;
            Z1T("data packet, %u bytes, sum 0x%04x", s->index, s->upload_sum);
        }
        break;
    case 0xC4:
        s->stage = s->stage < 2 ? s->stage + 1 : 2;
        Z1T("execute: stage %u", s->stage);
        break;
    case 0x47:
    case 0x68:
        if (s->frame && s->index >= s->frame_len + 3) {
            g_free(s->frame);                    /* read: the next length read takes a new one */
            s->frame = NULL;
        }
        break;
    default:
        break;
    }
    if (s->streaming) {
        s->upload_sum = s->sum & 0xFFFF;
        Z1T("main firmware stream, %u bytes, sum 0x%04x", s->stream_bytes, s->upload_sum);
        s->streaming = false;
    }
    s->offered = (s->cmd == 0x46 || s->cmd == 0x64 || s->cmd == 0x65) && s->frame;
    s->cmd = 0;
    s->index = 0;
    s->length = 0;
}

static int z1_set_cs(SSIPeripheral *dev, bool deselect)
{
    S5L8900MultitouchZ1State *s = S5L8900_MULTITOUCH_Z1(dev);

    if (deselect) {
        if (s->cmd || s->streaming) {
            z1_finish(s);
        }
    } else {
        s->selected = true;
        if (s->stream_next) {
            s->stream_next = false;
            s->streaming = true;
            s->stream_bytes = 0;
            s->sum = 0;
        }
    }
    return 0;
}

static uint32_t z1_transfer(SSIPeripheral *dev, uint32_t value)
{
    S5L8900MultitouchZ1State *s = S5L8900_MULTITOUCH_Z1(dev);
    uint8_t v = value;

    if (s->streaming) {
        s->sum += v;
        s->stream_bytes++;
        return 0;
    }
    if (!s->cmd) {
        if (v == 0x00 || v == 0xFF) {
            return 0;                            /* idle filler between transactions */
        }
        s->cmd = v;
        s->index = 0;
        s->sum = 0;
        switch (v) {
        case 0x05: case 0xC4: case 0xD0: s->length = 4; break;
        case 0x47:
            if (!s->offered) {
                s->cmd = v = 0x46;               /* a length read, the other NOP */
            }
            /* fall through */
        case 0x8F: case 0x46: case 0x64: case 0x65: s->length = 8; break;
        case 0xC2: s->length = 0x400; break;     /* a blank one ends at chip select */
        default: s->length = 0; break;
        }
        if (v != 0xC2 && v != 0x05 && v != 0xC4 && v != 0xD0 && v != 0x8F && v != 0x82 &&
            v != 0x46 && v != 0x47 && v != 0x64 && v != 0x65 && v != 0x68) {
            qemu_log_mask(LOG_GUEST_ERROR, "[Z1] unknown command 0x%02x\n", v);
            Z1T("unknown command 0x%02x", v);
        }
        z1_prepare(s);
    }
    if (s->index < sizeof(s->in)) {
        s->in[s->index] = v;
    }
    if (s->cmd == 0xC2 && s->index < 0x3FE) {
        s->sum += v;                             /* header + data, not the trailing sum */
    }
    if (s->index == 1 && (s->cmd == 0x8F || s->cmd == 0x82)) {
        z1_prepare(s);                           /* the report id is in */
        if (s->cmd == 0x82) {
            s->length = s->out_len;
        }
    }
    if (s->index == 0 && (s->cmd == 0x68 || s->cmd == 0x47)) {
        s->length = s->out_len ? s->out_len : 1;
    }
    uint8_t r = s->index < s->out_len ? s->out[s->index] : 0;
    s->index++;
    /* Without chip select (none wired, or the guest never drives it) the
     * known lengths are the only framing. */
    if (!s->selected && s->length && s->index >= s->length) {
        z1_finish(s);
    }
    return r;
}

static void z1_realize(SSIPeripheral *d, Error **errp)
{
    z1_parent_realize(d, errp);
}

static void z1_reset(DeviceState *dev)
{
    S5L8900MultitouchZ1State *s = S5L8900_MULTITOUCH_Z1(dev);

    z1_parent_reset(dev);
    g_free(s->frame);
    s->frame = NULL;
    s->cmd = 0;
    s->index = s->length = s->out_len = 0;
    s->sum = s->upload_sum = 0;
    s->stream_next = s->streaming = false;
    s->stage = 0;
}

static void z1_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    z1_parent_realize = k->realize;
    k->realize = z1_realize;
    k->transfer = z1_transfer;
    k->set_cs = z1_set_cs;
    k->cs_polarity = SSI_CS_LOW;
    z1_parent_reset = dc->legacy_reset;
    device_class_set_legacy_reset(dc, z1_reset);
}

static const TypeInfo z1_info = {
    .name = TYPE_S5L8900_MULTITOUCH_Z1,
    .parent = TYPE_IPOD_TOUCH_MULTITOUCH,
    .instance_size = sizeof(S5L8900MultitouchZ1State),
    .class_init = z1_class_init,
};

static void z1_register_types(void)
{
    type_register_static(&z1_info);
}

type_init(z1_register_types)
