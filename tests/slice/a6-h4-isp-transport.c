/* H4 firmware IPC: native boot handshake, selector-vs-channel IRQ routing,
 * two independently queried sensors, config bounds, malformed input.
 * SLICE hw/arm/s5l8940_isp.c define H4_
 * SLICE hw/arm/s5l8940_isp.c range typedef struct H4Buffer { | static bool h4_dma
 * SLICE hw/arm/s5l8940_isp.c fn h4_schedule h4_buffer_layout h4_submit_buffers h4_label h4_pattern h4_frame h4_publish_channels h4_io_command h4_commands h4_read h4_write h4_reset h4_post_load
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
typedef int SysBusDevice, MemoryRegion, DeviceState, qemu_irq, QEMUTimer;
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int clock)
{
    return 0;
}
static void timer_mod(QEMUTimer *timer, int64_t time)
{
}
static void timer_del(QEMUTimer *timer)
{
}
typedef uint64_t hwaddr;
typedef struct H4ISPState H4ISPState;
#define H4_ISP(dev) ((H4ISPState *)(dev))
static uint8_t ram[0x100000];
static bool irq;
static bool h4_dma(H4ISPState *s, uint32_t va, void *b, size_t n, bool wr)
{
    if ((uint64_t)va + n > sizeof(ram))
        return false;
    if (wr)
        memcpy(ram + va, b, n);
    else
        memcpy(b, ram + va, n);
    return true;
}
static void h4_irq(H4ISPState *s);
static void pstrcpy(char *p, size_t n, const char *v)
{
    g_strlcpy(p, v, n);
}
static uint32_t ldl_le_p(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return GUINT32_FROM_LE(v);
}
static uint16_t lduw_le_p(const void *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return GUINT16_FROM_LE(v);
}
static void stl_le_p(void *p, uint32_t v)
{
    v = GUINT32_TO_LE(v);
    memcpy(p, &v, 4);
}
static void stw_le_p(void *p, uint16_t v)
{
    v = GUINT16_TO_LE(v);
    memcpy(p, &v, 2);
}
#include "slice.h"
static void h4_irq(H4ISPState *s)
{
    irq = !!(s->regs[0x41000 / 4] & s->regs[0x41004 / 4]);
}
static H4ISPState s;
static void command(uint16_t op, unsigned len, unsigned channel, unsigned mode)
{
    const unsigned cmd = 0x18000, ring = s.queue_base + 0x1000 + 4 * 0x400;
    memset(ram + cmd, 0xa5, 256);
    stw_le_p(ram + cmd + 4, op);
    stl_le_p(ram + cmd + 8, channel);
    stl_le_p(ram + cmd + 12, mode);
    stl_le_p(ram + ring, cmd - s.firmware_base);
    stl_le_p(ram + ring + 4, len);
    stl_le_p(ram + ring + 8, len);
    h4_write(&s, 0x41020, 0x80, 4);
    assert(ldl_le_p(ram + ring) & 1);
    assert(irq);
    assert(h4_read(&s, 0x41000, 4) ==
           0x80); /* Native IRQ mask f8 accepts IO source 3. */
    h4_write(&s, 0x41024, 0x80, 4);
    assert(!irq);
}
int main(void)
{
    h4_write(&s, 0x40004, 1, 4);
    assert(h4_read(&s, 0xc3000, 4) == 7);
    h4_write(&s, 0xc3000, 0x1000, 4);
    h4_write(&s, 0xc3004, 0x3000, 4);
    h4_write(&s, 0x41024, 0x10, 4);
    h4_write(&s, 0x41020, 0x10, 4);
    assert(s.stage == 2);
    assert(!strcmp((char *)ram + s.queue_base + 4 * 256, "IO"));
    assert(ldl_le_p(ram + s.queue_base + 4 * 256 + 0x44) == 3);
    /* IO completion also signals IO_T2H's source. It must not expose phantom
  * unsolicited messages: T2H's consumer expects bit 0 clear for a message. */
    for (unsigned ch = 0; ch < H4_CHANNELS; ch++)
        for (unsigned slot = 0; slot < H4_SLOTS; slot++)
            assert(ldl_le_p(ram + s.queue_base + 0x1000 + ch * 0x400 +
                            slot * 64) == 1);
    h4_write(&s, 0xc300c, 0x08042006, 4);
    assert(s.stage == 3);
    h4_write(&s, 0x41024, 0x10, 4);
    h4_write(&s, 0x41004, 0xf8, 4);
    command(0, 8, 0, 0);
    assert(!lduw_le_p(ram + 0x18006));
    command(3, 28, 0, 0);
    assert(ldl_le_p(ram + 0x18008) == 24000000);
    assert(ldl_le_p(ram + 0x1800c) == 2);
    command(0x10d, 140, 0, 0);
    assert(ldl_le_p(ram + 0x18020) == 0x145);
    assert(ldl_le_p(ram + 0x18060) == 12);
    assert(ldl_le_p(ram + 0x18030) == 1 && !ldl_le_p(ram + 0x1803c));
    command(0x10d, 140, 1, 0);
    assert(ldl_le_p(ram + 0x18008) == 1);
    assert(ldl_le_p(ram + 0x18020) == 0x9760);
    assert(ldl_le_p(ram + 0x18060) == 5);
    assert(!ldl_le_p(ram + 0x18030) && !ldl_le_p(ram + 0x1803c));
    command(0x106, 80, 1, 4);
    assert(!lduw_le_p(ram + 0x18006));
    assert(lduw_le_p(ram + 0x18010) == 1472);
    for (unsigned ch = 0; ch < 2; ch++)
        for (unsigned mode = 0; mode < (ch ? 5 : 12); mode++) {
            command(0x106, 80, ch, mode);
            unsigned width = lduw_le_p(ram + 0x18014),
                     height = lduw_le_p(ram + 0x18016);
            assert(width && height);
            assert(isfinite(640.0 / width));
            assert(lduw_le_p(ram + 0x1801c) == (30 << 8));
            assert(lduw_le_p(ram + 0x1801e) == (1 << 8));
        }
    command(0x106, 80, 1, 5);
    assert(lduw_le_p(ram + 0x18006));
    command(0xffff, 8, 0, 0);
    assert(lduw_le_p(ram + 0x18006));
    uint8_t args[64] = {0};
    stl_le_p(args, 1);
    stl_le_p(args + 4, 4);
    assert(!h4_io_command(&s, 0x107, args, 8, 8) && s.sensor[1].mode == 4);
    stl_le_p(args + 4, 5);
    assert(h4_io_command(&s, 0x107, args, 8, 8) && s.sensor[1].mode == 4);
    stl_le_p(args + 4, 640);
    stl_le_p(args + 8, 480);
    stl_le_p(args + 20, 640);
    stl_le_p(args + 24, 640);
    assert(!h4_io_command(&s, 0xb01, args, 32, 32) && s.sensor[1].width == 640);
    stl_le_p(args + 20, 1);
    assert(h4_io_command(&s, 0xb01, args, 32, 32) &&
           s.sensor[1].stride_y == 640);
    stl_le_p(args + 4, UINT32_MAX);
    stl_le_p(args + 8, 0);
    stl_le_p(args + 12, 100);
    stl_le_p(args + 16, 100);
    assert(h4_io_command(&s, 0x801, args, 20, 20)); /* no overflowing crop */
    assert(!h4_io_command(&s, 0x100, args, 4, 4) && s.sensor[1].started);
    assert(!h4_io_command(&s, 0x101, args, 4, 4) && !s.sensor[1].started);
    const unsigned ring = s.queue_base + 0x1000 + 4 * 0x400;
    stl_le_p(ram + ring, 0x17000);
    stl_le_p(ram + ring + 4, 1);
    stl_le_p(ram + ring + 8, 1);
    h4_write(&s, 0x41020, 0x80, 4);
    assert(!irq);
    assert(!(ldl_le_p(ram + ring) & 1));
    /* Native buffer lists must be copied before H2T acknowledgement. */
    s.active_channel = 0;
    s.sensor[0] = (H4SensorState){.width = 16,
                                  .height = 8,
                                  .stride_y = 16,
                                  .stride_uv = 16,
                                  .started = true,
                                  .video_range = true};
    const unsigned submit = 0x20000, bufring = s.queue_base + 0x1800;
    memset(ram + submit, 0, 0x180);
    stl_le_p(ram + submit, 1);
    stl_le_p(ram + submit + 4, 1);
    stl_le_p(ram + submit + 8, 0x30000);
    stl_le_p(ram + submit + 12, 0x31000);
    stl_le_p(ram + submit + 8 + 24, 2);
    stl_le_p(ram + submit + 40, 0x12345678);
    stl_le_p(ram + bufring, submit - s.firmware_base);
    stl_le_p(ram + bufring + 4, 0x180);
    stl_le_p(ram + bufring + 8, 0x30000000);
    h4_commands(&s, 0x40);
    assert(s.buffer_count == 1);
    assert(ldl_le_p(ram + bufring) & 1);
    memset(ram + submit, 0xcc, 0x180);
    h4_frame(&s);
    assert(s.memory_requested && !s.completion_base);
    stl_le_p(ram + s.queue_base + 0x1000, 0x40001);
    h4_commands(&s, 0x10);
    assert(s.completion_base == 0x41000);
    h4_frame(&s);
    assert(s.buffer_count == 0 && s.completion_slot == 1);
    unsigned completion = s.completion_base;
    assert(ldl_le_p(ram + completion) == 2);
    assert(ldl_le_p(ram + completion + 4) == 1);
    assert(ldl_le_p(ram + completion + 40) == 0x12345678);
    assert(!(ldl_le_p(ram + s.queue_base + 0x1c00) & 1));
    assert(ram[0x31000 + 7 * 16] == 235 && ram[0x31000 + 7 * 16 + 15] == 16);
    assert(ram[0x32000] == 128 && ram[0x32000 + 14] == 128);
    /* Keep at most one image in each completion; include its statistics.
  * Never overwrite a completion the native driver has not acknowledged. */
    s.buffer_count = 3;
    for (unsigned i = 0; i < 3; i++)
        memset(&s.buffers[i], 0, sizeof(s.buffers[i]));
    stl_le_p(s.buffers[0].descriptor, 0x30000);
    stl_le_p(s.buffers[0].descriptor + 4, 0x31000);
    stl_le_p(s.buffers[1].descriptor, 0x32000);
    stl_le_p(s.buffers[1].descriptor + 4, 0x33000);
    stl_le_p(s.buffers[2].descriptor, 0x50000);
    stl_le_p(s.buffers[2].descriptor + 24, 1);
    uint8_t channel0[4] = {0};
    assert(!h4_io_command(&s, 0x407, channel0, 4, 4));
    h4_frame(&s);
    assert(s.buffer_count == 1);
    assert(ldl_le_p(s.buffers[0].descriptor) == 0x32000);
    assert(ldl_le_p(ram + s.completion_base + 0x180 + 4) == 2);
    assert(ldl_le_p(ram + 0x51000 + 12) == 10);
    assert(ldl_le_p(ram + 0x51000 + 20) ==
           0); /* Unsupported optional block stays absent. */
    assert(lduw_le_p(ram + 0x51000 + 64 + 0x24) == s.sensor[0].width);
    assert(lduw_le_p(ram + 0x51000 + 64 + 0x26) == s.sensor[0].height);
    assert(lduw_le_p(ram + 0x51000 + 64 + 0x38) == s.sensor[0].width);
    assert(lduw_le_p(ram + 0x51000 + 64 + 0x3a) == s.sensor[0].height);
    assert(ldl_le_p(ram + 0x51000 + 0x140) == 800000);
    assert(ldl_le_p(ram + 0x51000 + 16 + 3 * 4) == 0x700);
    assert(lduw_le_p(ram + 0x51000 + 0x700) == 512);
    assert(lduw_le_p(ram + 0x51000 + 0x416) == 1);
    assert(lduw_le_p(ram + 0x51000 + 0x418) == 0);
    assert(s.sensor[0].focus_frames == 1);
    unsigned next = s.queue_base + 0x1c00 + s.completion_slot * 64;
    stl_le_p(ram + next, 0);
    h4_frame(&s);
    assert(s.buffer_count == 1);
    stl_le_p(ram + next, 1);
    h4_frame(&s);
    assert(s.buffer_count == 0);
    assert(s.sensor[0].focus_frames == 0);
    assert(h4_pattern(&s, &s.buffers[2]));
    assert(lduw_le_p(ram + 0x51000 + 0x416) == 0);
    assert(lduw_le_p(ram + 0x51000 + 0x418) == 1);
    /* Stop/return must drain even when streaming is disabled, without DMA. */
    s.sensor[0].started = false;
    s.buffer_count = 2;
    memset(s.buffers, 0, sizeof(H4Buffer) * 2);
    stl_le_p(s.buffers[0].descriptor, 0xfffff000);
    assert(!h4_io_command(&s, 0x104, channel0, 4, 4));
    unsigned returned = s.completion_base + s.completion_slot * 0x180;
    h4_frame(&s);
    assert(s.buffer_count == 0);
    assert(ldl_le_p(ram + returned) == 1);
    assert(ldl_le_p(ram + returned + 4) == 2);
    assert(!h4_submit_buffers(&s, 0, 8));
    h4_write(&s, 0x40004, 0, 4);
    assert(s.stage == 0 && !irq);
    s.buffer_count = 1;
    s.buffers[0].channel = 2;
    assert(h4_post_load(&s, 1) == -EINVAL);
    s.buffers[0].channel = 0;
    s.sensor[0].width = 4097;
    assert(h4_post_load(&s, 1) == -EINVAL);
    s.sensor[0].width = 640;
    assert(!h4_post_load(&s, 1));
    s.sensor[0].started = true;
    s.sensor[0].temporal_filter = true;
    h4_reset((DeviceState *)&s);
    assert(!s.buffer_count && !s.completion_base && !s.stage && !irq);
    assert(!s.sensor[0].started && !s.sensor[0].temporal_filter);
    assert(!s.memory_requested && !s.frames);
    /* Later wire format: three 40-byte descriptors, including the appended
  * word, must retain their tags and plane counts in the completion. */
    s.stage = 3;
    s.queue_base = 0x4000;
    s.firmware_base = 0x1000;
    s.completion_base = 0x41000;
    s.sensor[0] = (H4SensorState){.width = 16,
                                  .height = 8,
                                  .stride_y = 16,
                                  .stride_uv = 16,
                                  .started = true};
    memset(ram + submit, 0, 0x180);
    stl_le_p(ram + submit, 1);
    stl_le_p(ram + submit + 4, 3);
    for (unsigned i = 0; i < 3; i++) {
        uint8_t *d = ram + submit + 8 + i * 40;
        stl_le_p(d, i == 0 ? 0x30000 : 0x50000 + i * 0x2000);
        stl_le_p(d + 4, i == 0 ? 0x31000 : 0);
        stl_le_p(d + 24, i == 0 ? 2 : 1);
        stl_le_p(d + 32, 0x12340000 + i);
        stl_le_p(d + 36, 0xa5a50000 + i);
    }
    assert(h4_submit_buffers(&s, submit - s.firmware_base, 0x180));
    assert(s.descriptor_size == 40 && s.buffer_count == 3);
    stl_le_p(ram + s.queue_base + 0x1c00, 1);
    h4_frame(&s);
    assert(!s.buffer_count && ldl_le_p(ram + s.completion_base + 4) == 3);
    for (unsigned i = 0; i < 3; i++) {
        assert(ldl_le_p(ram + s.completion_base + 8 + i * 40 + 32) ==
               0x12340000 + i);
        assert(ldl_le_p(ram + s.completion_base + 8 + i * 40 + 36) ==
               0xa5a50000 + i);
    }
    /* A malformed list cannot enqueue partial descriptors or change layout. */
    stl_le_p(ram + submit + 8 + 40 + 24, 0);
    assert(!h4_submit_buffers(&s, submit - s.firmware_base, 0x180));
    assert(!s.buffer_count && s.descriptor_size == 40);
    /* Exposure tokens acknowledge the caller's setting on the corresponding
  * sensor's delivered frame, rather than leaving a capture waiting forever. */
    memset(args, 0, sizeof(args));
    stw_le_p(args + 4, 256);
    stl_le_p(args + 8, 0x12345678);
    assert(h4_io_command(&s, 0x204, args, 11, 12));
    assert(!s.exposure_tag[0]);
    assert(!h4_io_command(&s, 0x204, args, 8, 8));
    assert(s.exposure_bias[0] == 256 && !s.exposure_tag[0]);
    assert(!h4_io_command(&s, 0x204, args, 12, 12));
    assert(s.exposure_tag[0] == 0x12345678 && s.exposure_bias[0] == 256);
    H4Buffer meta = {0};
    stl_le_p(meta.descriptor, 0x70000);
    stl_le_p(meta.descriptor + 24, 1);
    assert(h4_pattern(&s, &meta));
    assert(ldl_le_p(ram + s.firmware_base + 0x70000 + 0x150) == 0x12345678);
    meta.channel = 1;
    assert(h4_pattern(&s, &meta));
    assert(!ldl_le_p(ram + s.firmware_base + 0x70000 + 0x150));
    stw_le_p(args + 4, 0);
    assert(h4_io_command(&s, 0x203, args, 4, 5));
    assert(!h4_io_command(&s, 0x203, args, 4, 6));
    assert(lduw_le_p(args + 4) == 256);
    assert(!h4_post_load(&s, 2) && s.exposure_tag[0] == 0x12345678);
    assert(!h4_post_load(&s, 1) && !s.exposure_tag[0] && !s.exposure_bias[0]);
    s.exposure_tag[1] = 99;
    s.exposure_bias[1] = 128;
    h4_reset((DeviceState *)&s);
    assert(!s.exposure_tag[1] && !s.exposure_bias[1]);
    /* Strobe completion is delivered with two successful images: preflash,
  * then main flash. A blocked completion ring must not consume either. */
    s.stage = 2;
    s.queue_base = 0x8000;
    s.firmware_base = 0x1000;
    s.completion_base = 0x10000;
    s.sensor[0] = (H4SensorState){.width = 16,
                                  .height = 8,
                                  .stride_y = 16,
                                  .stride_uv = 16,
                                  .started = true,
                                  .video_range = true};
    memset(args, 0, sizeof(args));
    stw_le_p(args + 20, 0xffff);
    assert(h4_io_command(&s, 0x21c, args, 23, 24));
    assert(!s.sensor[0].still_pending);
    stl_le_p(args, 1);
    assert(h4_io_command(&s, 0x21c, args, 24, 24));
    stl_le_p(args, 0);
    assert(!h4_io_command(&s, 0x21c, args, 24, 24));
    assert(s.sensor[0].still_pending == 2);
    for (unsigned phase = 1; phase <= 3; phase++) {
        memset(s.buffers, 0, sizeof(s.buffers));
        s.buffer_count = 2;
        stl_le_p(s.buffers[0].descriptor, 0x30000);
        stl_le_p(s.buffers[0].descriptor + 4, 0x31000);
        stl_le_p(s.buffers[0].descriptor + 24, 2);
        stl_le_p(s.buffers[1].descriptor, 0x50000);
        stl_le_p(s.buffers[1].descriptor + 24, 1);
        unsigned ring = s.queue_base + 0x1c00 + s.completion_slot * 64;
        unsigned pending = s.sensor[0].still_pending;
        stl_le_p(ram + ring, 0);
        h4_frame(&s);
        assert(s.sensor[0].still_pending == pending);
        stl_le_p(ram + ring, 1);
        h4_frame(&s);
        assert(lduw_le_p(ram + 0x51000 + 64 + 0x28) ==
               (phase <= 2 ? phase : 0));
        assert(ram[0x51000 + 64 + 0x2a] == (phase <= 2));
        assert(s.sensor[0].still_pending == (phase == 1 ? 1 : 0));
    }
    puts("ok");
}
