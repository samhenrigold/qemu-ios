/* The guest's output volume reaches the host: the codec's registers as the drivers write them through the 16 levels
 * (10B500's CS42L59, 9B206's CS42L61), and the port's scaling on the way out. The top level plays the PCM as sent.
 *
 * SLICE hw/arm/ipod_touch_cs42l58.c fn cs42l58_output_gain
 * SLICE hw/arm/s5l8930_i2s.c fn i2s_out_cb
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
enum { CS42L58_VOLUME_NONE, CS42L58_VOLUME_CS42L59, CS42L58_VOLUME_CS42L61 };
typedef struct { uint8_t regs[0x80]; uint8_t volume; } CS42L58State;
#define I2S_RING 16384
typedef struct {
    CS42L58State *codec;
    uint8_t ring[I2S_RING];
    uint32_t tail, level;
    int audio_be, voice;
} S5L8930I2SState;
static int16_t heard[8192];
static unsigned heard_bytes;
static unsigned lduw_le_p(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8; }
static void stw_le_p(void *p, unsigned v) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; }
static size_t audio_be_write(int be, int voice, void *data, size_t n)
{
    memcpy((char *)heard + heard_bytes, data, n);
    heard_bytes += n;
    return n;
}
#include "slice.h"

static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

int main(void)
{
    CS42L58State c = { .volume = CS42L58_VOLUME_NONE };

    assert(cs42l58_output_gain(&c) == 1.0);       /* a codec not traced stays at unity */

    /* CS42L59 speaker: 0x78 for levels 1-16 is unity, iOS scaling the PCM itself; 0x40 at level 0 and idle mutes. */
    c.volume = CS42L58_VOLUME_CS42L59;
    c.regs[0x1d] = c.regs[0x1f] = c.regs[0x21] = 0x40;
    assert(cs42l58_output_gain(&c) == 0.0);
    c.regs[0x21] = 0x78;
    assert(near(cs42l58_output_gain(&c), 1.0));
    /* The speaker muted, another output playing: against that one's top. */
    c.regs[0x21] = 0x40;
    c.regs[0x1d] = 0x02;
    assert(near(cs42l58_output_gain(&c), 1.0));
    c.regs[0x1d] = 0x47;                          /* -28.5 dB, 29.5 under the top */
    assert(near(cs42l58_output_gain(&c), pow(10, -29.5 / 20)));
    /* The speaker wins whenever it isn't muted. */
    c.regs[0x21] = 0x78;
    assert(near(cs42l58_output_gain(&c), 1.0));

    /* CS42L61: 0x18 (+12 dB) is unity, 0xc4 (-30 dB) 42 dB under it, and level 0 mutes. */
    memset(c.regs, 0, sizeof(c.regs));
    c.volume = CS42L58_VOLUME_CS42L61;
    c.regs[0x21] = 0x18;
    assert(near(cs42l58_output_gain(&c), 1.0));
    c.regs[0x21] = 0xc4;
    assert(near(cs42l58_output_gain(&c), pow(10, -42.0 / 20)));
    c.regs[0x07] = 1;
    c.regs[0x67] = 0x3f;
    assert(cs42l58_output_gain(&c) == 0.0);

    /* The port: scaled samples on the way out, the ring untouched and consumed. */
    static S5L8930I2SState s;
    static const int16_t pcm[] = { 16384, -16384, 32767, -32768 };
    memcpy(s.ring, pcm, sizeof(pcm));
    s.level = sizeof(pcm);
    s.codec = &c;
    c.regs[0x07] = c.regs[0x67] = 0;
    c.regs[0x21] = 0x0c;                          /* +6 dB, 6 under the top */
    i2s_out_cb(&s, sizeof(pcm));
    double g = pow(10, -6.0 / 20);
    assert(heard_bytes == sizeof(pcm) && !s.level && s.tail == sizeof(pcm));
    for (unsigned i = 0; i < 4; i++) {
        assert(heard[i] == (int16_t)lrint(pcm[i] * g));
    }
    assert(!memcmp(s.ring, pcm, sizeof(pcm)));

    /* No codec linked (the other ports): the PCM as sent. */
    heard_bytes = 0;
    s.codec = NULL;
    s.tail = 0;
    s.level = sizeof(pcm);
    i2s_out_cb(&s, sizeof(pcm));
    assert(!memcmp(heard, pcm, sizeof(pcm)));
    puts("PASS: the codec's output volume scales what the host hears, the top level at unity");
    return 0;
}
