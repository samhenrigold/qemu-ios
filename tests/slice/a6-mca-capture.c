/* Host PCM and test tone must reach native MCA DMA slots without changing
 * frame rate or losing the legacy stereo byte ordering.
 * SLICE hw/arm/s5l8930_i2s.c fn i2s_channels i2s_sample_bytes i2s_next_frame i2s_rx_read
 * CFLAGS -Wall -Wextra -Werror
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#define I2S_RING 64
#define TONE_AMPLITUDE 8000
#define MAX(a, b) ((a) > (b) ? (a) : (b))
static unsigned ctpop32(uint32_t x)
{
    return __builtin_popcount(x);
}
static uint16_t lduw_le_p(const void *ptr)
{
    const uint8_t *p = ptr;
    return p[0] | p[1] << 8;
}
static void stw_le_p(void *ptr, uint16_t v)
{
    uint8_t *p = ptr;
    p[0] = v;
    p[1] = v >> 8;
}
static void stl_le_p(void *ptr, uint32_t v)
{
    uint8_t *p = ptr;
    for (int i = 0; i < 4; i++)
        p[i] = v >> (i * 8);
}
typedef struct {
    bool mca;
    uint32_t regs[1024], tone_hz, in_rate, in_level, in_tail;
    uint64_t tone_frame;
    unsigned rx_pos;
    uint8_t in_ring[I2S_RING], rx_frame[128];
} S5L8930I2SState;
#include "slice.h"
int main(void)
{
    S5L8930I2SState s = {0};
    /* A legacy frame straddles the host ring boundary. */
    s.in_tail = 62;
    s.in_level = 4;
    s.in_ring[62] = 0x34;
    s.in_ring[63] = 0x12;
    s.in_ring[0] = 0xdc;
    s.in_ring[1] = 0xfe;
    assert(i2s_rx_read(&s, 1) == 0x34);
    assert(i2s_rx_read(&s, 1) == 0x12);
    assert(i2s_rx_read(&s, 2) == 0xfedc && !s.rx_pos && !s.in_level &&
           s.in_tail == 2);
    s.mca = true;
    s.regs[8 >> 2] = 0x1010;
    s.regs[0x38 >> 2] = 15;
    s.in_tail = 62;
    s.in_level = 4;
    assert(i2s_rx_read(&s, 4) == 0xfedc1234);
    assert(i2s_rx_read(&s, 4) == 0xfedc1234 && !s.rx_pos && !s.in_level);
    /* Native Codec signed S16 stays right-aligned in a four-byte word, once per frame, across slots. */
    s.regs[8 >> 2] = 0;
    s.in_tail = 62;
    s.in_level = 4;
    assert(i2s_rx_read(&s, 4) == 0x00001234);
    assert(i2s_rx_read(&s, 4) == 0xfffffedc);
    assert(i2s_rx_read(&s, 4) == 0x00001234);
    assert(i2s_rx_read(&s, 4) == 0xfffffedc && !s.rx_pos);
    /* Every programmable channel mask fits; underrun is a whole silent frame. */
    s.regs[0x38 >> 2] = 0xffffffff;
    for (int i = 0; i < 32; i++)
        assert(!i2s_rx_read(&s, 4));
    assert(!s.rx_pos);
    /* Four slots must not advance the source clock four times as fast. */
    s.regs[0x38 >> 2] = 15;
    s.regs[8 >> 2] = 0x1010;
    s.tone_hz = 1000;
    s.in_rate = 44100;
    int previous = 0, crossings = 0;
    for (int frame = 0; frame < 44100; frame++) {
        uint32_t a = i2s_rx_read(&s, 4), b = i2s_rx_read(&s, 4);
        assert(a == b && (uint16_t)a == (a >> 16));
        int sample = (int16_t)a;
        if (previous <= 0 && sample > 0)
            crossings++;
        previous = sample;
    }
    assert(s.tone_frame == 44100 && crossings == 1000 && !s.rx_pos);
    puts(
        "PASS: legacy stereo, MCA S16/S32 slots, ring wrap, underrun, maximum frame and 1 kHz capture clock");
}
