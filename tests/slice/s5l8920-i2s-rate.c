/* The S5L8920's i2s0 plays at the rate the audio complex's NCO and the I2S dividers make, without a guest.
 *
 * AppleS5L8920XAudioComplex sets MCLK by writing NCO +0x18 = 2 MCLK and +0x1c = 2 MCLK - ncoref; the I2S divides
 * MCLK by +0x408 bits 8-15 (2) into BCLK and BCLK by the +0x410 low byte (32) into frames. 6.1.6 programs
 * 0x562200/0xf6ae3580 (MCLK 2.8224 MHz) for 44.1 kHz and 0x2b1100/0xf6832480 (1.4112 MHz) for the 22.05 kHz
 * VoiceOver speaks at, with the active dividers 0x20f and 0xc3200020 (its stop config, 0xf and 0x13000040, has
 * none). Before this the port read the A4's PMGR NCO address and played everything at 44.1 kHz.
 *
 * The iPod touch 3G's port is the slave of its CS42L58, which makes the frame clock from a fixed 6 MHz MCLK (NCO
 * 0xb71b00/0xf70f2e80) at the rate in its register 05; its config, 0x20000040 with the dividers 0x20f, has bits
 * 30-31 clear and divides that MCLK into 46875 Hz, which everything played at (VoiceOver's 22.05 kHz speech 2.13
 * times fast). The port follows the codec's LRCLK while it is the slave, and only then; the TX config's bit 20,
 * which makes the A4's Samsung I2S the slave, is not this block's.
 *
 * SLICE hw/arm/s5l8930_i2s.c define I2S_(TXCON|TXCON_EXT_CLK|MCLK_DIV|FRAME_BITS|FRAME_MASTER)[[:space:]]
 * SLICE hw/arm/s5l8930_i2s.c range static S5L8930I2SState *i2s_ports | static void i2s_in_cb
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
static unsigned ctpop32(uint32_t x) { return __builtin_popcount(x); }
typedef unsigned Clock;
static unsigned clock_get_hz(Clock *clock) { return clock ? *clock : 0; }
typedef struct S5L8930I2SState {
    uint32_t regs[0x400];
    bool ctrl_run;
    bool mca;
    uint32_t frame_rate;
    Clock *lrclk;
    uint64_t nco_base;
    uint32_t nco_ref_hz;
} S5L8930I2SState;

#define AUDIO_COMPLEX 0x84300000
static uint32_t nco[8];   /* the audio complex's first 0x20 bytes */
typedef uint64_t hwaddr;
static int address_space_memory;
#define MEMTXATTRS_UNSPECIFIED 0
#define S5L8930_NCO_BCLK(n) (0xbf100104 + 0x10 * (n))
static uint32_t address_space_ldl_le(int *as, hwaddr addr, int attrs, void *result)
{
    return addr >= AUDIO_COMPLEX && addr < AUDIO_COMPLEX + sizeof(nco) ? nco[(addr - AUDIO_COMPLEX) / 4] : 0;
}
#include "slice.h"

static void program(S5L8930I2SState *s, uint32_t a, uint32_t b, uint32_t div, uint32_t frame)
{
    nco[0x18 / 4] = a;
    nco[0x1c / 4] = b;
    s->regs[I2S_MCLK_DIV >> 2] = div;
    s->regs[I2S_FRAME_BITS >> 2] = frame;
}

int main(void)
{
    S5L8930I2SState i2s0 = { .ctrl_run = true, .nco_base = AUDIO_COMPLEX, .nco_ref_hz = 162000000 };

    i2s_ports[0] = &i2s0;
    /* Before the kernel has set MCLK: 44.1 kHz. */
    assert(s5l8930_i2s_rate(0) == 44100);
    program(&i2s0, 0x562200, 0xf6ae3580, 0x20f, 0xc3200020);
    assert(s5l8930_i2s_rate(0) == 44100);
    program(&i2s0, 0x2b1100, 0xf6832480, 0x20f, 0xc3200020);
    assert(s5l8930_i2s_rate(0) == 22050);
    /* The stop config divides by nothing: the fallback, not a division by zero. */
    program(&i2s0, 0x2b1100, 0xf6832480, 0xf, 0x13000040);
    assert(s5l8930_i2s_rate(0) == 44100);
    /* 48 kHz from the same dividers: MCLK 3.072 MHz. */
    program(&i2s0, 2 * 3072000, 2 * 3072000 - 162000000, 0x20f, 0xc3200020);
    assert(s5l8930_i2s_rate(0) == 48000);
    /* The output is ncoref A / (A - B) / 2, not A / 2: a reference off PLL1 moves it. */
    i2s0.nco_ref_hz = 81000000;
    assert(s5l8930_i2s_rate(0) == 24000);
    i2s0.nco_ref_hz = 162000000;

    /* The iPod touch 3G: the CS42L58's LRCLK, not the dividers' 46875 Hz. */
    Clock lrclk = 0;
    i2s0.lrclk = &lrclk;
    program(&i2s0, 0xb71b00, 0xf70f2e80, 0x20f, 0x20000040);
    assert(s5l8930_i2s_rate(0) == 44100);   /* before the codec has a rate: not the dividers' 46875 */
    lrclk = 44100;
    assert(s5l8930_i2s_rate(0) == 44100);
    lrclk = 22050;
    assert(s5l8930_i2s_rate(0) == 22050);
    lrclk = 16000;
    assert(s5l8930_i2s_rate(0) == 16000);
    /* The 3GS's port masters the clock: its codec's register file has no say. */
    program(&i2s0, 0x2b1100, 0xf6832480, 0x20f, 0xc3200020);
    assert(s5l8930_i2s_rate(0) == 22050);
    lrclk = 44100;
    assert(s5l8930_i2s_rate(0) == 22050);
    /* Its TX config's bit 20 says nothing here. */
    i2s0.regs[I2S_TXCON >> 2] = I2S_TXCON_EXT_CLK;
    assert(s5l8930_i2s_rate(0) == 22050);
    puts("PASS: S5L8920 i2s0 runs at its MCLK over its dividers as master, at the codec's LRCLK as slave");
    return 0;
}
