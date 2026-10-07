/*
 * The iPod touch 1G's piezo buzzer: the DT's /arm-io/timer/buzzer (device_type
 * pwm, reg <0x20 0 0xffff>), a transducer on timer 1's output pin. N45 has no
 * speaker; every system sound is Beep (the WM8758, headphones) + Buzz (this),
 * per Celestial's N45/SystemSoundBehaviour.plist. mediaserverd plays a Buzz as
 * SystemSoundBuzzToneSequences.plist's hertz_millisecs pairs, which the kernel's
 * AppleS5L8900XTimerDevice turns into timer 1 programs (KeyPressed: 1880 Hz
 * 1 ms, 840 Hz 1 ms, 1800 Hz 2 ms).
 *
 * The timer model reports each change of the pin's waveform (start, period,
 * high time, end: ipod_touch_timer.h); this renders that square wave into a
 * mono voice, 40 ms behind the guest's clock so a tone's end is always known
 * before its samples are written. The voice stays active (silence between
 * tones) so the stream keeps the guest's timeline.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "qemu/audio.h"
#include "hw/arm/ipod_touch_piezo.h"

OBJECT_DECLARE_SIMPLE_TYPE(IPodTouchPiezoState, IPOD_TOUCH_PIEZO)

#define PIEZO_RATE 44100
#define PIEZO_LAG_NS (40 * SCALE_MS)
#define PIEZO_MAX_BEHIND_NS (200 * SCALE_MS)   /* after a stall, skip rather than replay */
#define PIEZO_SEGS 32

struct IPodTouchPiezoState {
    DeviceState parent_obj;
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    IPodTouchTimerOutput seg[PIEZO_SEGS];   /* oldest first */
    unsigned nseg;
    int64_t base_ns;       /* guest time of sample 0 of this run */
    uint64_t played;       /* samples written since base_ns */
    uint32_t channel;      /* "channel": the timer it is on */
    uint32_t amplitude;    /* "amplitude": peak, of 32767 */
};

/* One sample: +amp while the pin is high, -amp while low, 0 while idle. */
static int16_t piezo_level(const IPodTouchTimerOutput *seg, unsigned n,
                           int64_t t, int16_t amp)
{
    for (unsigned i = n; i-- > 0;) {
        const IPodTouchTimerOutput *o = &seg[i];
        if (o->period_ns > 0 && t >= o->start_ns && t < o->end_ns) {
            return (t - o->start_ns) % o->period_ns < o->high_ns ? amp : -amp;
        }
    }
    return 0;
}

void ipod_touch_piezo_timer_output(void *opaque, unsigned ch, const IPodTouchTimerOutput *out)
{
    IPodTouchPiezoState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (ch != s->channel) {
        return;
    }
    /* A new program, or a stop, ends whatever the pin was doing. */
    if (s->nseg && s->seg[s->nseg - 1].end_ns > now) {
        s->seg[s->nseg - 1].end_ns = now;
    }
    if (out->period_ns <= 0) {
        return;
    }
    if (s->nseg == PIEZO_SEGS) {
        memmove(s->seg, s->seg + 1, sizeof(s->seg[0]) * --s->nseg);
    }
    s->seg[s->nseg++] = *out;
}

static void piezo_out_cb(void *opaque, int free_bytes)
{
    IPodTouchPiezoState *s = opaque;
    int16_t buf[1024];
    int64_t target = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - PIEZO_LAG_NS;
    int64_t pos = s->base_ns + muldiv64(s->played, NANOSECONDS_PER_SECOND, PIEZO_RATE);

    if (pos < target - PIEZO_MAX_BEHIND_NS) {
        s->base_ns = pos = target - PIEZO_MAX_BEHIND_NS;
        s->played = 0;
    }
    while (free_bytes >= 2 && pos < target) {
        unsigned frames = MIN(free_bytes / 2, ARRAY_SIZE(buf));
        frames = MIN(frames, muldiv64(target - pos, PIEZO_RATE, NANOSECONDS_PER_SECOND) + 1);
        for (unsigned i = 0; i < frames; i++) {
            int64_t t = s->base_ns + muldiv64(s->played + i, NANOSECONDS_PER_SECOND, PIEZO_RATE);
            buf[i] = piezo_level(s->seg, s->nseg, t, s->amplitude);
        }
        size_t wrote = audio_be_write(s->audio_be, s->voice, buf, frames * 2) / 2;
        if (!wrote) {
            break;
        }
        s->played += wrote;
        free_bytes -= wrote * 2;
        pos = s->base_ns + muldiv64(s->played, NANOSECONDS_PER_SECOND, PIEZO_RATE);
    }
    /* Forget tones that are wholly played. */
    unsigned keep = 0;
    for (unsigned i = 0; i < s->nseg; i++) {
        if (s->seg[i].end_ns > pos) {
            s->seg[keep++] = s->seg[i];
        }
    }
    s->nseg = keep;
}

static void piezo_realize(DeviceState *dev, Error **errp)
{
    IPodTouchPiezoState *s = IPOD_TOUCH_PIEZO(dev);
    struct audsettings as = { .freq = PIEZO_RATE, .nchannels = 1, .fmt = AUDIO_FORMAT_S16 };

    if (s->amplitude > 32767) {
        error_setg(errp, "amplitude is at most 32767");
        return;
    }
    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    s->voice = audio_be_open_out(s->audio_be, NULL, "ipod-piezo.out", s, piezo_out_cb, &as);
    if (!s->voice) {
        warn_report("ipod piezo: could not open an output voice; the buzzer is silent");
        return;
    }
    audio_be_set_volume_out_lr(s->audio_be, s->voice, 0, 255, 255);
    audio_be_set_active_out(s->audio_be, s->voice, 1);
}

static const Property piezo_properties[] = {
    DEFINE_AUDIO_PROPERTIES(IPodTouchPiezoState, audio_be),
    DEFINE_PROP_UINT32("channel", IPodTouchPiezoState, channel, 1),
    /* A calibration knob, not a measurement: how loud the host plays it. */
    DEFINE_PROP_UINT32("amplitude", IPodTouchPiezoState, amplitude, 8000),
};

static void piezo_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = piezo_realize;
    device_class_set_props(dc, piezo_properties);
}

static const TypeInfo piezo_info = {
    .name          = TYPE_IPOD_TOUCH_PIEZO,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(IPodTouchPiezoState),
    .class_init    = piezo_class_init,
};

static void piezo_register_types(void)
{
    type_register_static(&piezo_info);
}

type_init(piezo_register_types)
