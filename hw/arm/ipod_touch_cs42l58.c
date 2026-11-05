#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_cs42l58.h"
#include "trace.h"
#include "hw/trace-printf.h"
#include <math.h>

/*
 * Cirrus CS42L58 stereo audio codec, I2C address 0x4A (device tree:
 * /device-tree/arm-io/i2c0/audio0, compatible "audio-control,cs42l58").
 *
 * The device is a plain 7-bit-address register file: the master writes a
 * memory-address pointer (MAP) byte, optionally with the auto-increment bit
 * 0x80 set, then either writes data bytes or issues a repeated START and
 * reads them back. Every control register reads back what was written --
 * that read/write round-trip is what the driver's gain/volume code relies on.
 */
static bool codec_trace(void)
{
    return trace_event_get_state_backends(TRACE_CS42L58_LOG);
}

#define CS42L58_MAP_INCR   0x80
#define CS42L58_REG_CHIPID 0x01
/*
 * CS42L59 (iPod touch 4): 10A403 AppleCS42L59Audio's halt handler sets both
 * power-down bits in 0x06, then polls 0x38 until bit 3 is set, with no
 * timeout. The power-down finishes at once here. ponytail: inferred from
 * that driver loop, not from a datasheet.
 */
#define CS42L59_REG_PWRCTL 0x06
#define CS42L59_REG_STATUS 0x38
#define CS42L59_PDN_DONE   0x08

static unsigned cs42l58_sample_rate(uint8_t control)
{
    /* 7E18 AppleCS42L58Audio::setSampleRate, c05dd698: register 05's
     * low five bits select LRCLK. The codec is the I2S clock master.
     * 10B500 AppleCS42L59Audio (n81) writes the same codes to register 0a
     * from a fixed 6 MHz MCLK (0x80c3908c): 0x0b for 44.1 kHz, and 0x13
     * for the 22.05 kHz VoiceOver's speech switches the hardware to. The
     * codes are the CS42L55's CLKCTL2 (Rockbox's cs42l55.h: speed mode,
     * 32 kHz group and MCLK/LRCK ratio from a 6 MHz-class MCLK), the same
     * family's part on the S5L8702 iPods.
     * ponytail: model the nine rates the driver supports; other clock modes
     * need the part's clock specification before they can be enabled. */
    switch (control & 0x1f) {
    case 0x09: return 48000;
    case 0x0b: return 44100;
    case 0x0d: return 32000;
    case 0x11: return 24000;
    case 0x13: return 22050;
    case 0x15: return 16000;
    case 0x19: return 12000;
    case 0x1b: return 11025;
    case 0x1d: return 8000;
    default: return 0;
    }
}

/*
 * Output volume. iOS sets the volume buttons' level in the codec on some routes, not in the PCM, so the host hears it
 * only if it is applied here. The top level plays at unity, the PCM as sent, and the rest keep their distance from it:
 * the codec's positive gains drive the analog side, and applied to full-scale PCM they would only clip. Inferred from
 * what the drivers write as the volume steps through its 16 levels (blake-lucas's register traces), not a datasheet.
 *
 * CS42L59 (n81, 10B500): 7-bit two's complement in 0.5 dB steps, 0x40 muted. No headphone is modeled, so the host hears
 * the speaker's output, 0x21: iOS scales the PCM itself on that route and parks 0x21 at 0x78 (-4 dB, its unity) for
 * levels 1 to 16, and 0x40 at level 0 and idle. While it is muted the loudest other output, against its own top
 * (0x1d's 0x02), plays instead, as a route iOS drives through the headphones' output would.
 *
 * CS42L61 (n88, 9B206; speaker): 0x21, 8-bit two's complement in 0.5 dB steps, levels 1-16 0xc4 (-30 dB) to 0x18
 * (+12 dB). Level 0 keeps 0xc4 and mutes: 0x07 bit 0 set and 0x67 0x3f, cleared again for any other level.
 */
double cs42l58_output_gain(CS42L58State *s)
{
    /* output register, and its top level in 0.5 dB steps; the speaker's last */
    static const struct { uint8_t reg; int8_t top; } cs42l59_out[] = {
        { 0x1d, 2 }, { 0x1f, 2 }, { 0x21, -8 },
    };
    double db = -INFINITY;

    switch (s->volume) {
    case CS42L58_VOLUME_CS42L59:
        for (unsigned i = 0; i < ARRAY_SIZE(cs42l59_out); i++) {
            uint8_t v = s->regs[cs42l59_out[i].reg] & 0x7f;

            if (v != 0x40) {
                double out = (double)((int8_t)(v << 1) / 2 - cs42l59_out[i].top) / 2;

                if (i == ARRAY_SIZE(cs42l59_out) - 1) {
                    return pow(10.0, out / 20.0);
                }
                db = MAX(db, out);
            }
        }
        break;
    case CS42L58_VOLUME_CS42L61:
        if (!((s->regs[0x07] & 1) && s->regs[0x67] == 0x3f)) {
            db = (double)(int8_t)s->regs[0x21] / 2 - 12;
        }
        break;
    default:
        return 1.0;
    }
    return db == -INFINITY ? 0.0 : pow(10.0, db / 20.0);
}

static int cs42l58_event(I2CSlave *i2c, enum i2c_event event)
{
    CS42L58State *s = CS42L58(i2c);

    if (event == I2C_START_SEND) {
        s->have_cmd = false;   /* next byte written is the MAP */
    }
    return 0;
}

static uint8_t cs42l58_recv(I2CSlave *i2c)
{
    CS42L58State *s = CS42L58(i2c);
    uint8_t reg = s->cmd & 0x7f;
    uint8_t res = s->regs[reg];

    if (s->pdn_status && reg == CS42L59_REG_STATUS &&
        (s->regs[CS42L59_REG_PWRCTL] & 3) == 3) {
        res |= CS42L59_PDN_DONE;
    }

    if (codec_trace()) {
        TRACE_PRINTF(trace_cs42l58_log, "CODEC R %02x -> %02x\n", reg, res);
    }
    if (s->autoinc) {
        s->cmd = (reg + 1) & 0x7f;
    }
    return res;
}

static int cs42l58_send(I2CSlave *i2c, uint8_t data)
{
    CS42L58State *s = CS42L58(i2c);

    if (!s->have_cmd) {
        s->autoinc = !!(data & CS42L58_MAP_INCR);
        s->cmd = data & 0x7f;
        s->have_cmd = true;
        return 0;
    }

    if (codec_trace()) {
        TRACE_PRINTF(trace_cs42l58_log, "CODEC W %02x <- %02x\n", (uint8_t)(s->cmd & 0x7f), data);
    }
    if ((s->cmd & 0x7f) != CS42L58_REG_CHIPID) {
        s->regs[s->cmd & 0x7f] = data;
        if ((s->cmd & 0x7f) == s->rate_reg) {
            unsigned rate = cs42l58_sample_rate(data);
            if (rate) {
                clock_update_hz(s->lrclk, rate);
            }
        }
    }
    if (s->autoinc) {
        s->cmd = ((s->cmd & 0x7f) + 1) & 0x7f;
    }
    return 0;
}

static void cs42l58_reset(DeviceState *dev)
{
    CS42L58State *s = CS42L58(dev);
    const char *id = getenv("IT_CODEC_ID");

    memset(s->regs, 0, sizeof(s->regs));
    s->cmd = 0;
    s->have_cmd = false;
    s->autoinc = false;
    /* Chip ID / revision: read-only on the real part. */
    s->regs[CS42L58_REG_CHIPID] = id ? (uint8_t)strtoul(id, NULL, 0) : 0xe0;
}

static void cs42l58_init(Object *obj)
{
    CS42L58(obj)->lrclk = qdev_init_clock_out(DEVICE(obj), "lrclk");
    CS42L58(obj)->rate_reg = 0x05;
}

static const VMStateDescription vmstate_cs42l58 = {
    .name = TYPE_CS42L58,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, CS42L58State),
        VMSTATE_UINT32(cmd, CS42L58State),
        VMSTATE_BOOL(have_cmd, CS42L58State),
        VMSTATE_BOOL(autoinc, CS42L58State),
        VMSTATE_UINT8_ARRAY(regs, CS42L58State, 128),
        VMSTATE_CLOCK(lrclk, CS42L58State),
        VMSTATE_END_OF_LIST()
    },
};

static void cs42l58_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_cs42l58;
    k->event = cs42l58_event;
    k->recv = cs42l58_recv;
    k->send = cs42l58_send;
    device_class_set_legacy_reset(dc, cs42l58_reset);
}

static const TypeInfo cs42l58_info = {
    .name          = TYPE_CS42L58,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = cs42l58_init,
    .instance_size = sizeof(CS42L58State),
    .class_init    = cs42l58_class_init,
};

static void cs42l59_init(Object *obj)
{
    CS42L58(obj)->pdn_status = true;
    CS42L58(obj)->rate_reg = 0x0a;
    CS42L58(obj)->volume = CS42L58_VOLUME_CS42L59;
}

static const TypeInfo cs42l59_info = {
    .name          = TYPE_CS42L59,
    .parent        = TYPE_CS42L58,
    .instance_init = cs42l59_init,
};

static void cs42l61_init(Object *obj)
{
    CS42L58(obj)->volume = CS42L58_VOLUME_CS42L61;
}

static const TypeInfo cs42l61_info = {
    .name          = TYPE_CS42L61,
    .parent        = TYPE_CS42L58,
    .instance_init = cs42l61_init,
};

static void cs42l58_register_types(void)
{
    type_register_static(&cs42l58_info);
    type_register_static(&cs42l59_info);
    type_register_static(&cs42l61_info);
}

type_init(cs42l58_register_types)
