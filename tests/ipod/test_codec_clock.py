#!/usr/bin/env python3
"""Exercise codec I2C rate selection and I2S voice/clock updates without a guest."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned Clock;
typedef int ClockEvent;
typedef struct audsettings { int freq, nchannels; } Settings;
typedef struct {
    Clock *lrclk;
    Settings as;
    uint32_t ring_format[1];
    unsigned ring_tail, ring_level, voice_rate;
    bool enable, active, card_ok;
    unsigned pace_debt;
    int64_t pace_last_ns;
    uint64_t pace_fraction;
    unsigned fifo_bytes;
    bool dma_req, pushed_since_tick;
    void *voice;
    int card;
} IPodTouchI2SState;
typedef struct {
    unsigned cmd;
    bool have_cmd, autoinc;
    uint8_t regs[128];
    Clock *lrclk;
} CS42L58State;
typedef CS42L58State I2CSlave;
#define CS42L58(s) (s)
#define CS42L58_MAP_INCR 0x80
#define CS42L58_REG_CHIPID 1
#define IT_I2S_DPRINTF(...) ((void)0)
#define warn_report(...) ((void)0)
static bool codec_trace(void) { return false; }
static IPodTouchI2SState output;
static int opened, drained, drain_rate, voice_rate;
static bool voice_active;
static int64_t now = 1000;
#define QEMU_CLOCK_VIRTUAL 0
#define IT_I2S_PACE_PERIOD_NS 2000000
#define IT_I2S_PACE_DEBT_MAX 32768u
#define MIN(a,b) ((a)<(b)?(a):(b))
static int64_t qemu_clock_get_ns(int clock) { return now; }
static bool it_i2s_debt_enabled(void) { return true; }
static void pace_drain_actual(IPodTouchI2SState *);
static void it_i2s_clock_update(void *, ClockEvent);
static unsigned clock_get_hz(Clock *clock) { return *clock; }
static void clock_update_hz(Clock *clock, unsigned rate) {
    *clock = rate;
    it_i2s_clock_update(&output, 0);
}
static void it_i2s_pace_drain(IPodTouchI2SState *s) {
    drained++; drain_rate = s->as.freq;
    pace_drain_actual(s);
}
static void it_i2s_vlog(IPodTouchI2SState *s,const char *what,int a,int b) {}
static void it_i2s_out_cb(void *s, int n) {}
static void *AUD_open_out(int *card, void *old, const char *name, void *opaque,
                         void (*cb)(void *, int), Settings *as) {
    assert(opaque == &output && cb == it_i2s_out_cb);
    opened++; voice_rate = as->freq;
    return card;
}
static void AUD_set_volume_out(void *voice, int mute, int l, int r) {
    assert(voice && !mute && l == 255 && r == 255);
}
static void AUD_set_active_out(void *voice, int active) { voice_active = active; }
'''
for filename, names in (
    ('ipod_touch_cs42l58.c', ('cs42l58_sample_rate', 'cs42l58_send')),
    ('ipod_touch_i2s.c', ('it_i2s_clock_update','it_i2s_update_voice')),
):
    source = (root / 'hw/arm' / filename).read_text()
    for name in names:
        match = re.search(r'^static [^\n]*\b' + name + r'\([^)]*\)\n\{.*?^}',
                          source, re.M | re.S)
        assert match, name
        code += match.group() + '\n'
source = (root / 'hw/arm/ipod_touch_i2s.c').read_text()
match = re.search(r'^static void it_i2s_pace_drain\([^)]*\)\n\{.*?^}',
                  source, re.M | re.S)
assert match
code += match.group().replace('it_i2s_pace_drain', 'pace_drain_actual') + '\n'
code += r'''
int main(void) {
    Clock clock = 44100;
    CS42L58State codec = { .lrclk = &clock };
    output = (IPodTouchI2SState) { .lrclk = &clock, .as.freq = 44100,
        .enable = true, .active = true, .card_ok = true, .pace_debt = 123, .voice_rate=44100 };
    voice_rate=44100;
    const unsigned controls[] = { 0xd3, 0xcb, 0xc9, 0xcd, 0xd1, 0xd5,
                                  0xd9, 0xdb, 0xdd };
    const unsigned rates[] = { 22050, 44100, 48000, 32000, 24000, 16000,
                               12000, 11025, 8000 };
    for (unsigned i = 0; i < 9; i++) {
        int previous = output.as.freq;
        codec.have_cmd = false;
        cs42l58_send(&codec, 5);
        cs42l58_send(&codec, controls[i]);
        assert(codec.regs[5] == controls[i]);
        assert(clock == rates[i] && output.as.freq == rates[i]);
        assert(voice_rate == previous); /* queued old audio keeps its format */
        output.ring_level=4;output.ring_format[0]=rates[i]<<9;
        it_i2s_update_voice(&output);
        assert(voice_rate == rates[i] && voice_active && !output.pace_debt);
        assert(drain_rate == previous && drained == i + 1 && opened == i + 1);
        cs42l58_send(&codec, controls[i]);
        assert(opened == i + 1); /* unchanged clock does not destroy a voice */
    }
    cs42l58_send(&codec, 0); /* unknown mode is not a made-up frequency */
    assert(opened == 9 && clock == 8000);
    setenv("IT_I2S_RATE", "44100", 1);
    cs42l58_send(&codec, 0xcb);
    assert(opened == 9 && output.as.freq == 8000);
    /* Ten seconds of jittered ticks must drain exactly ten seconds of PCM,
     * with no per-tick rounding loss at any supported sample rate. */
    for (unsigned i = 0; i < 9; i++) {
        IPodTouchI2SState paced = { .as = { rates[i], 2 },
            .fifo_bytes = 10000000, .pace_last_ns = now };
        for (unsigned tick = 0; tick < 5000; tick++) {
            now += (tick & 1) ? 2999999 : 1000001;
            pace_drain_actual(&paced);
        }
        assert(10000000 - paced.fifo_bytes == rates[i] * 4 * 10);
        assert(!paced.pace_fraction && !paced.pace_debt);
    }
    puts("PASS: codec I2C clock changes pace DMA immediately and defer host format changes");
}
'''
with tempfile.TemporaryDirectory(prefix='codec-clock-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
