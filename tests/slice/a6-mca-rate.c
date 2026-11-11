/* The A6 MCA's frame rate follows the FIFO watermarks 14G61 AppleAE2MCA programs for it, as captured from the
 * Bedtime wake sounds (48 kHz) and the lock sound (44.1 kHz), and keeps frame-rate until it has programmed them.
 * SLICE hw/arm/s5l8930_i2s.c fn i2s_channels i2s_mca_rate
 * CFLAGS -Wall -Wextra -Werror
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
static unsigned ctpop32(uint32_t x)
{
    return __builtin_popcount(x);
}
typedef struct {
    bool mca;
    uint32_t regs[1024], frame_rate;
} S5L8930I2SState;
#include "slice.h"
int main(void)
{
    S5L8930I2SState s = {.mca = true, .frame_rate = 44100};

    /* Unprogrammed: frame-rate. */
    assert(i2s_mca_rate(&s) == 44100);
    s.frame_rate = 32000;
    assert(i2s_mca_rate(&s) == 32000);
    /* The speaker port's streams as 14G61 programs them: mono TX, stereo RX. */
    s.regs[0x24 >> 2] = 1;
    s.regs[0x38 >> 2] = 3;
    s.regs[0x20 >> 2] = 0x00170004;
    s.regs[0x34 >> 2] = 0x002b0004;
    assert(i2s_mca_rate(&s) == 44100);
    s.regs[0x34 >> 2] = 0x002f0004;
    assert(i2s_mca_rate(&s) == 48000);
    /* Mono TX alone can't tell 44.1 from 48 kHz (24 for both): 44.1 kHz. */
    s.regs[0x34 >> 2] = 0;
    assert(i2s_mca_rate(&s) == 44100);
    /* Other rates, from the driver's formula. */
    s.regs[0x20 >> 2] = 0;
    s.regs[0x34 >> 2] = 0xffff0000u + (((16000 * 2 / 2000 + 3) & ~3u) << 16) + 4;
    assert(i2s_mca_rate(&s) == 16000);
    s.regs[0x34 >> 2] = 0xffff0000u + (((96000 * 2 / 2000 + 3) & ~3u) << 16) + 4;
    assert(i2s_mca_rate(&s) == 96000);
    /* A watermark no rate gives (the FIFO clamps it): frame-rate. */
    s.regs[0x34 >> 2] = 0x00050004;
    assert(i2s_mca_rate(&s) == 32000);
    return 0;
}
