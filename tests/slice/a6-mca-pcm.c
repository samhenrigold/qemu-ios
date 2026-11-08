/* Native N42 MCA startup/stop words against production MMIO/PCM dispatch.
 * SLICE hw/arm/s5l8930_i2s.c define I2S_(CTRL|CTRL_TX_IDLE|TXFIFO|RXFIFO|TXCOM|RXCOM|CMD_RUN)[[:space:]]
 * SLICE hw/arm/s5l8930_i2s.c fn i2s_tx_running i2s_push i2s_read i2s_write
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-parameter
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define I2S_RING 64
typedef uint64_t hwaddr;
typedef struct {
    bool mca, ctrl_run, audio_out, audio_in;
    unsigned port, head, tail, level, rx_pos, in_head, in_tail, in_level;
    uint32_t regs[1024];
    uint8_t ring[I2S_RING];
    void *voice, *voice_in, *audio_be;
} S5L8930I2SState;
static bool active, input_active;
static unsigned configured_rate;
static void audio_be_set_active_out(void *be, void *v, bool on)
{
    active = on;
}
static void audio_be_set_active_in(void *be, void *v, bool on)
{
    input_active = on;
}
static unsigned s5l8930_i2s_rate(unsigned port)
{
    assert(port == 6);
    return 48000;
}
static void i2s_set_rate(S5L8930I2SState *s, unsigned rate)
{
    configured_rate = rate;
}
static void i2s_set_in_rate(S5L8930I2SState *s, unsigned rate)
{
}
static uint64_t i2s_rx_read(S5L8930I2SState *s, unsigned size)
{
    return 0;
}
#include "slice.h"
int main(void)
{
    S5L8930I2SState s = {.mca = true,
                         .audio_out = true,
                         .audio_in = true,
                         .port = 6,
                         .voice = &s};
    /* Original 11A465 startup writes: reset, sample padding, masks and TX
     * configuration. MCA +0 must not acquire the legacy I2S idle bit. */
    i2s_write(&s, 0x44, 0x301, 4);
    assert(i2s_read(&s, 0x44, 4) == 0 && i2s_read(&s, 0, 4) == 0);
    i2s_write(&s, 8, 0x1010, 4);
    i2s_write(&s, 0x20, 0x170004, 4);
    i2s_write(&s, 0x24, 1, 4);
    i2s_write(&s, 0x38, 15, 4);
    i2s_write(&s, 0x18, 0x108, 4);
    i2s_write(&s, 0x28, 0x7654, 2);
    assert(!s.level && !active); /* Initial FIFO priming is not playback. */
    i2s_write(&s, 0x18, 0x109, 4);
    assert(active && configured_rate == 48000);
    i2s_write(&s, 0x28, 0x1234, 2);
    i2s_write(&s, 0x28, 0xfedc, 2);
    assert(s.level == 4 && s.ring[0] == 0x34 && s.ring[1] == 0x12);
    assert(s.ring[2] == 0xdc && s.ring[3] == 0xfe);
    /* TX stop resets the FIFO, while its format survives for the next start. */
    i2s_write(&s, 0x18, 0, 4);
    assert(!active);
    i2s_write(&s, 0x44, 1, 4);
    assert(!s.level && !s.head && !s.tail && !i2s_read(&s, 0x44, 4));
    assert(i2s_read(&s, 0x20, 4) == 0x170004 && i2s_read(&s, 8, 4) == 0x1010);
    s.regs[0x40 >> 2] = 0x3000;
    i2s_write(&s, 0x40, 0x1000, 4);
    assert(i2s_read(&s, 0x40, 4) == 0x2000);
    assert(i2s_read(&s, 0x3c, 4) == 0); /* Stopped RX stays silent. */
    s.voice_in = &s;
    i2s_write(&s, 0x18, 0x109, 4);
    i2s_write(&s, 0x2c, 0x109, 4);
    assert(active && input_active);
    s.in_level = 8;
    s.rx_pos = 2;
    i2s_write(&s, 0x44, 2, 4);
    assert(active && !input_active && !s.in_level && !s.rx_pos);
    assert(i2s_read(&s, 0x18, 4) == 0x109);
    i2s_write(&s, 0x2c, 0x109, 4);
    i2s_write(&s, 0x44, 1, 4);
    assert(!active && input_active);
    puts(
        "PASS: native MCA register layout, TX gate, PCM bytes, W1C and FIFO reset retention");
}
