/* The iPod touch 4's i2s0 plays at the rate its codec is set to, without a guest.
 *
 * The CS42L59 masters i2s0's frame clock and AppleCS42L59Audio picks the rate in the codec's register 0a (0x0b
 * 44.1 kHz; 0x13 22.05 kHz, which iOS 6 switches to for VoiceOver's speech). The port's frame rate, which the
 * output voice and the CDMA's pacing both use, must follow it while TX runs, because the port's own TX config
 * (10B500's 0x03100081) has bit 20 set, the Samsung I2S's external clock. The CS42L58's rate stays in register 05.
 *
 * Where the port masters the clock (the iPad, the iPhone 4: TX config 0x03000081) its rate is PMGR NCO 0's, whatever
 * the codec connected to it is set to, which 5.x and 6.x set for
 * VoiceOver's 22.05 kHz, and back, while TX runs: each CDMA chain starts at the NCO's rate as it is then
 * (s5l8930_i2s_sync_rate) and the voice must play at that rate too, not at the one TX started with.
 *
 * SLICE hw/arm/ipod_touch_cs42l58.c fn cs42l58_sample_rate cs42l58_send
 * SLICE hw/arm/s5l8930_i2s.c define I2S_(CTRL|TXCON|TXCON_EXT_CLK|TXCOM|CMD_RUN|MCLK_DIV|FRAME_BITS|FRAME_MASTER)[[:space:]]
 * SLICE hw/arm/s5l8930_i2s.c range static S5L8930I2SState *i2s_ports | static void i2s_in_cb
 * SLICE hw/arm/s5l8930_i2s.c fn i2s_tx_running i2s_set_rate i2s_lrclk_update s5l8930_i2s_sync_rate
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
static unsigned ctpop32(uint32_t x) { return __builtin_popcount(x); }
typedef unsigned Clock;
typedef int ClockEvent;
static unsigned clock_get_hz(Clock *clock) { return clock ? *clock : 0; }

/* The codec's LRCLK out is i2s0's lrclk in, as hw/arm/ipad1.c connects them. */
typedef struct S5L8930I2SState {
    uint32_t regs[0x400];
    bool ctrl_run;
    bool mca;
    uint32_t frame_rate;
    unsigned out_channels, out_bytes;
    uint8_t port;
    Clock *lrclk;
    uint64_t nco_base;
    uint32_t nco_ref_hz;   /* 0: the A4's PMGR NCO */
    unsigned rate;
    void *voice, *audio_be;
} S5L8930I2SState;
static S5L8930I2SState i2s0;
static void i2s_lrclk_update(void *, ClockEvent);
static void clock_update_hz(Clock *clock, unsigned rate)
{
    *clock = rate;
    i2s_lrclk_update(&i2s0, 0);
}

typedef struct {
    unsigned cmd;
    bool have_cmd, autoinc;
    uint8_t regs[128];
    bool pdn_status;
    uint8_t rate_reg;
    Clock *lrclk;
} CS42L58State;
typedef CS42L58State I2CSlave;
#define CS42L58(s) (s)
#define CS42L58_MAP_INCR 0x80
#define CS42L58_REG_CHIPID 1
static bool codec_trace(void) { return false; }

/* PMGR NCO 0's bit-clock register, which nothing programs on the iPod touch 4. */
static uint32_t nco_bclk;
typedef uint64_t hwaddr;
static int address_space_memory;
#define MEMTXATTRS_UNSPECIFIED 0
#define S5L8930_NCO_BCLK(n) (0xbf100104 + 0x10 * (n))
static uint32_t address_space_ldl_le(int *as, hwaddr addr, int attrs, void *result)
{
    return addr == S5L8930_NCO_BCLK(0) ? nco_bclk : 0;
}

struct audsettings { int freq, nchannels, fmt; bool big_endian; };
#define AUDIO_FORMAT_S16 1
#define AUDIO_FORMAT_S32 2
static int opened, voice_rate, active = -1;
static void i2s_out_cb(void *opaque, int free_bytes) {}
static void *audio_be_open_out(void *be, void *old, const char *name, void *opaque,
                               void (*cb)(void *, int), struct audsettings *as)
{
    assert(opaque == &i2s0 && cb == i2s_out_cb && as->nchannels == 2);
    opened++;
    active = 0;   /* as QEMU's audio: a reopened voice is inactive */
    voice_rate = as->freq;
    return opaque;
}
static void audio_be_set_volume_out_lr(void *be, void *voice, int mute, int l, int r) {}
static void __attribute__((unused)) audio_be_set_active_out(void *be, void *voice, int on) { active = on; }
#include "slice.h"

static void codec_write(CS42L58State *codec, uint8_t reg, uint8_t value)
{
    codec->have_cmd = false;
    cs42l58_send(codec, reg);
    cs42l58_send(codec, value);
}

int main(void)
{
    Clock lrclk = 0;
    CS42L58State cs42l59 = { .rate_reg = 0x0a, .lrclk = &lrclk };

    i2s_ports[0] = &i2s0;
    i2s0 = (S5L8930I2SState) { .lrclk = &lrclk, .rate = 44100, .voice = &i2s0,
                             .out_channels = 2, .out_bytes = 2 };
    i2s0.regs[I2S_TXCOM >> 2] = I2S_CMD_RUN;
    i2s0.regs[I2S_TXCON >> 2] = 0x03100081;        /* 10B500's TX config: the codec's slave */
    i2s0.regs[I2S_FRAME_BITS >> 2] = 0x28000300;

    /* Before the codec has a rate: 44.1 kHz, not the NCO the slave does not use. */
    assert(s5l8930_i2s_rate(0) == 44100);
    nco_bclk = 64 * 48000;
    assert(s5l8930_i2s_rate(0) == 44100);
    nco_bclk = 0;

    /* Boot sets the codec to 44.1 kHz; VoiceOver's speech takes it to 22.05 kHz and back. */
    codec_write(&cs42l59, 0x0a, 0x0b);
    assert(lrclk == 44100 && s5l8930_i2s_rate(0) == 44100 && opened == 0);
    codec_write(&cs42l59, 0x0a, 0x13);
    assert(lrclk == 22050 && s5l8930_i2s_rate(0) == 22050);
    assert(opened == 1 && voice_rate == 22050 && i2s0.rate == 22050 && active == 1);
    codec_write(&cs42l59, 0x0a, 0x0b);
    assert(s5l8930_i2s_rate(0) == 44100 && opened == 2 && voice_rate == 44100);

    /* The CS42L59's register 05 is not its rate; the CS42L58's is. */
    codec_write(&cs42l59, 0x05, 0x13);
    assert(lrclk == 44100 && opened == 2);
    CS42L58State cs42l58 = { .rate_reg = 0x05, .lrclk = &lrclk };
    codec_write(&cs42l58, 0x05, 0x13);
    assert(lrclk == 22050 && voice_rate == 22050);

    /* Stopped, a rate change waits for the next start (which reads the port's rate). */
    i2s0.regs[I2S_TXCOM >> 2] = 0;
    codec_write(&cs42l59, 0x0a, 0x09);
    assert(s5l8930_i2s_rate(0) == 48000 && voice_rate == 22050);

    /* A port no codec drives runs at its NCO. */
    assert(s5l8930_i2s_rate(1) == 44100);

    /* The iPad: master of its frame clock, at its NCO, whatever its codec's register file says (the codec's LRCLK
     * is still 48 kHz here). TX starts at 44.1 kHz; the NCO goes to 22.05 kHz and back while it runs, and each
     * chain's start takes the voice along. */
    i2s0.regs[I2S_TXCON >> 2] = 0x03000081;
    i2s0.regs[I2S_FRAME_BITS >> 2] = 0xc8000300;
    i2s0.regs[I2S_TXCOM >> 2] = I2S_CMD_RUN;
    nco_bclk = 64 * 44100;
    assert(s5l8930_i2s_sync_rate(0) == 44100 && voice_rate == 44100);
    nco_bclk = 64 * 22050;
    assert(s5l8930_i2s_sync_rate(0) == 22050 && voice_rate == 22050 && i2s0.rate == 22050 && active == 1);
    nco_bclk = 64 * 44100;
    assert(s5l8930_i2s_sync_rate(0) == 44100 && voice_rate == 44100 && active == 1);
    /* Stopped, nothing to follow. */
    i2s0.regs[I2S_TXCOM >> 2] = 0;
    nco_bclk = 64 * 16000;
    assert(s5l8930_i2s_sync_rate(0) == 16000 && voice_rate == 44100);
    /* The +0x410 word is the S5L8920 block's; on the A4 bit 20 alone decides. */
    i2s0.regs[I2S_FRAME_BITS >> 2] = 0x28000300;
    assert(s5l8930_i2s_rate(0) == 16000);
    i2s0.regs[I2S_TXCON >> 2] = 0x03100081;
    assert(s5l8930_i2s_rate(0) == 48000);
    /* Native N42 speaker startup: mono S16, four-channel RX, 48 kHz
     * virtual clock. Legacy ports remain stereo S16. */
    S5L8930I2SState mca = { .mca = true, .frame_rate = 48000 };
    i2s_ports[6] = &mca;
    mca.regs[8 >> 2] = 0x1010;
    mca.regs[0x24 >> 2] = 1;
    mca.regs[0x38 >> 2] = 15;
    assert(s5l8930_i2s_rate(6) == 48000);
    assert(s5l8930_i2s_frame_bytes(6, false) == 2);
    assert(s5l8930_i2s_frame_bytes(6, true) == 8);
    assert(s5l8930_i2s_frame_bytes(0, false) == 4);
    assert(s5l8930_i2s_frame_bytes(99, false) == 4);
    mca.regs[0x18 >> 2] = 0x109;
    assert(i2s_tx_running(&mca));
    mca.regs[0x18 >> 2] = 0;
    assert(!i2s_tx_running(&mca));
    puts("PASS: i2s0 runs at the codec's LRCLK as the slave (TX config bit 20), at its NCO as the master, and its "
         "voice follows either");
    return 0;
}
