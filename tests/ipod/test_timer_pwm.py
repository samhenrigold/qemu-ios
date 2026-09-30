#!/usr/bin/env python3
"""S5L8900 timers 0-3 as PWM, and the N45 piezo on timer 1, driven with no emulator.

hw/arm/ipod_touch_timer.c decodes timers 0-3 the way 3A101a's AppleS5L8900XTimer
tone routine programs them (c049049c): COUNT_BUFFER2 the period, COUNT_BUFFER
the toggle point, PRESCALER n+1, CONFIG bits 10:8 a pre-divider (/2 /4 /16 /64),
bits 5:4 == 2 a one shot, STATE bit 1 latching the buffers and bit 0 running.
hw/arm/ipod_touch_piezo.c turns the pin waveform into samples. The register
writes below are the ones the guest made for Celestial's KeyPressed (IT_TIMER_TRACE,
3A101a): 1880 Hz for 1 ms, 840 Hz one shot, 1800 Hz; nclk is 24 MHz.

Mutations (named, each must fail this test):
  1. timer_channel_output: `prediv[(c->config >> 8) & 7]` -> `1` (the tone an
     octave up: 3760 Hz for the 1880 Hz click).
  2. timer_channel_output: drop the one-shot end (`o.end_ns = INT64_MAX;`), so
     840 Hz rings on.
  3. timer_channel_write: `c->lcb2 = c->cb2;` removed (STATE bit 1 no longer
     latches the period: nothing plays).
  4. ipod_touch_piezo_timer_output: drop the `end_ns = now` close, so a stopped
     tone keeps sounding.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
timer = (root / 'hw/arm/ipod_touch_timer.c').read_text()
piezo = (root / 'hw/arm/ipod_touch_piezo.c').read_text()
header = (root / 'include/hw/arm/ipod_touch_timer.h').read_text()


def func(src, name):
    m = re.search(r'^(?:static )?[^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', src, re.M | re.S)
    assert m, name
    return m.group()


def struct(src, name):
    m = re.search(r'typedef struct ' + name + r' \{.*?\} ' + name + ';', src, re.S)
    assert m, name
    return m.group()


defines = '\n'.join(re.findall(r'^#define TIMER_\w+\s+[^\n]*$', header, re.M))
piezo_state = re.search(r'struct IPodTouchPiezoState \{.*?\};', piezo, re.S).group()
piezo_state = re.sub(r'^\s*(DeviceState|QEMUSoundCard|SWVoiceOut)[^\n]*\n', '', piezo_state, flags=re.M)

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t hwaddr;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define NANOSECONDS_PER_SECOND 1000000000LL
#define QEMU_CLOCK_VIRTUAL 0
static uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c) { return (unsigned __int128)a * b / c; }
static int64_t now;
static int64_t qemu_clock_get_ns(int c) { return now; }
#define PIEZO_SEGS 32
'''

tests = r'''
#define NCLK 24000000
static IPodTouchTimerChannel c;
static IPodTouchPiezoState pz = { .channel = 1, .amplitude = 8000 };
static void wr(hwaddr reg, uint32_t v) {
    timer_channel_write(&c, reg, v, now);
    if (reg == TIMER_STATE) {
        IPodTouchTimerOutput o = timer_channel_output(&c, NCLK);
        ipod_touch_piezo_timer_output(&pz, 1, &o);
    }
}
/* The guest's own sequence for one tone (c049049c: stop, CB, CB2, PRESCALER, CONFIG, 2, 1). */
static void tone(uint32_t cb, uint32_t cb2, uint32_t config) {
    wr(TIMER_STATE, 2);
    wr(TIMER_COUNT_BUFFER, cb); wr(TIMER_COUNT_BUFFER2, cb2); wr(TIMER_PRESCALER, 0);
    wr(TIMER_CONFIG, config); wr(TIMER_STATE, 2); wr(TIMER_STATE, 1);
}
static int16_t at(int64_t t) { return piezo_level(pz.seg, pz.nseg, t, pz.amplitude); }
/* Rising edges of the rendered 44.1 kHz stream in [t0, t1): the tone's cycles. */
static int cycles(int64_t t0, int64_t t1) {
    int n = 0; int16_t prev = at(t0);
    for (int64_t t = t0; t < t1; t += 1000000000LL / 44100) {
        int16_t v = at(t);
        if (v > 0 && prev <= 0) n++;
        prev = v;
    }
    return n;
}
int main(void) {
    /* KeyPressed tone 1: CB 0xc77, CB2 0x18ee at /2 -> 24 MHz / 2 / 6382 = 1880.2 Hz. */
    now = 1000000;
    tone(0xc77, 0x18ee, 0x40);
    IPodTouchTimerOutput o = timer_channel_output(&c, NCLK);
    assert(o.start_ns == now && o.end_ns == INT64_MAX);
    double hz = 1e9 / o.period_ns;
    printf("tone 1: %.1f Hz, high %lld of %lld ns\n", hz, (long long)o.high_ns, (long long)o.period_ns);
    assert(hz > 1879 && hz < 1881);
    assert(o.high_ns * 2 > o.period_ns - 2 && o.high_ns * 2 < o.period_ns + 2);   /* CB = CB2 / 2: square */
    assert(timer_channel_read(&c, TIMER_COUNT_BUFFER2) == 0x18ee && timer_channel_read(&c, TIMER_CONFIG) == 0x40);
    /* The rendered stream: 10 ms of it has 18-19 cycles, i.e. 1880 Hz. */
    int n = cycles(now, now + 10000000);
    printf("tone 1 rendered: %d cycles in 10 ms\n", n);
    assert(n >= 18 && n <= 19);

    /* The guest stops it (STATE 2 alone): silence from that instant, not a cycle later. */
    now += 1000000;
    wr(TIMER_STATE, 2);
    assert(timer_channel_output(&c, NCLK).period_ns == 0);
    assert(at(now - 1000) != 0 && at(now) == 0 && at(now + 5000000) == 0);

    /* Tone 2: 840 Hz as a one shot (CONFIG 0x60, bits 5:4 = 2): one period, then idle. */
    now += 40000;
    tone(0x1be6, 0x37cd, 0x60);
    o = timer_channel_output(&c, NCLK);
    hz = 1e9 / o.period_ns;
    printf("tone 2: %.1f Hz one shot, ends %lld ns after start\n", hz, (long long)(o.end_ns - o.start_ns));
    assert(hz > 839.5 && hz < 840.5 && o.end_ns == o.start_ns + o.period_ns);
    assert(at(o.start_ns + 1000) > 0 && at(o.start_ns + o.high_ns + 1000) < 0 && at(o.end_ns + 1000) == 0);

    /* The pre-divider: CONFIG bits 10:8 = 1 is /4, the same count an octave down. */
    now += 2000000;
    tone(0xc77, 0x18ee, 0x140);
    hz = 1e9 / timer_channel_output(&c, NCLK).period_ns;
    assert(hz > 939 && hz < 941);

    /* A buffer write while running changes nothing until STATE bit 1 latches it. */
    wr(TIMER_COUNT_BUFFER2, 0x1a0a);
    hz = 1e9 / timer_channel_output(&c, NCLK).period_ns;
    assert(hz > 939 && hz < 941);

    /* Other channels are not the piezo's. */
    IPodTouchTimerOutput other = { .start_ns = now, .end_ns = INT64_MAX, .period_ns = 1000, .high_ns = 500 };
    unsigned before = pz.nseg;
    ipod_touch_piezo_timer_output(&pz, 2, &other);
    assert(pz.nseg == before);
    puts("PASS: timers 0-3 PWM period/prescaler/one-shot/latch, piezo follows start and stop");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + defines + '\n' + struct(header, 'IPodTouchTimerChannel') + '\n'
                 + struct(header, 'IPodTouchTimerOutput') + '\n' + 'typedef ' + piezo_state.replace(
                     'struct IPodTouchPiezoState {', 'struct {').rstrip(';') + ' IPodTouchPiezoState;\n'
                 + '\n'.join(func(timer, n) for n in ('timer_channel_output', 'timer_channel_write',
                                                       'timer_channel_read'))
                 + '\n' + func(piezo, 'piezo_level') + '\n' + func(piezo, 'ipod_touch_piezo_timer_output')
                 + tests)
    binary = str(Path(tmp) / 'check')
    subprocess.run(['clang', '-std=gnu11', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
