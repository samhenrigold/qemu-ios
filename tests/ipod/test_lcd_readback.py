#!/usr/bin/env python3
"""Actual LCD control MMIO: stock rotation RMW, option independence, N45 map and panel= size."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path,
                    default=Path(__file__).resolve().parents[2] / 'hw/arm/ipod_touch_lcd.c')
source = parser.parse_args().source.read_text()


def function(name):
    start = source.index('static ', source.index(name) - 30)
    brace = source.index('{', start)
    position, depth = brace + 1, 1
    while depth:
        if source[position] == '{':
            depth += 1
        elif source[position] == '}':
            depth -= 1
        position += 1
    return source[start:position]


header = r'''
#include <stdint.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t hwaddr;
typedef struct {
    bool planes_enabled, s5l8900;
    uint32_t plane_regs[0x300 / 4];
    uint32_t irq_enable, irq_status, lcd_con;
    uint32_t w1_display_depth_info, w1_framebuffer_base;
    uint32_t w1_hspan, w1_display_resolution_info;
    uint32_t pw, ph;
    bool ctrl_readback;
    uint32_t fb_base;
} IPodTouchLCDState;
#define LCD_FB_WIDTH  320
#define LCD_FB_HEIGHT 480
static bool lcd_trace(void) { return false; }
static bool lcd_ready_hack(void) { return false; }
static void lcd_update_irq(IPodTouchLCDState *s) { (void)s; }
static void lcd_write_irq(IPodTouchLCDState *s, hwaddr addr, uint64_t value)
{ (void)s; (void)addr; (void)value; }
#define lcd_ft(...) ((void)0)
#define qemu_log_mask(...) ((void)0)
#define LOG_UNIMP 0
'''
checks = r'''
int main(void)
{
    uint32_t values[] = {0x00310700, 0, 1, 0x80000000, 0xffffffff, 0xdeadbeef};
    for (unsigned option = 0; option < 2; option++) {
        IPodTouchLCDState s = { .planes_enabled = option, .pw = 320, .ph = 480 };
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            ipod_touch_lcd_write(&s, 0x40, values[i], 4);
            assert(ipod_touch_lcd_read(&s, 0x40, 4) == values[i]);
        }
        for (unsigned rotation = 0; rotation < 4; rotation++) {
            ipod_touch_lcd_write(&s, 0x40, 0x00310700, 4);
            /* Actual 5F138 order: control, pitch, base, size, origin, crop,
             * then control read/OR(rotation<<22)/write. */
            ipod_touch_lcd_write(&s, 0x48, 320, 4);
            ipod_touch_lcd_write(&s, 0x44, 0x0f496000, 4);
            ipod_touch_lcd_write(&s, 0x50, 0x014001e0, 4);
            ipod_touch_lcd_write(&s, 0x54, 0, 4);
            ipod_touch_lcd_write(&s, 0x4c, 0, 4);
            uint32_t value = ipod_touch_lcd_read(&s, 0x40, 4);
            ipod_touch_lcd_write(&s, 0x40, value | (rotation << 22), 4);
            assert(ipod_touch_lcd_read(&s, 0x40, 4) ==
                   (0x00310700 | (rotation << 22)));
        }
        /* Preserve other option-gated registers; no broad bank enable. */
        assert(ipod_touch_lcd_read(&s, 0x48, 4) == (option ? 320 : 0));
        ipod_touch_lcd_write(&s, 0x20, 0x00200700, 4);
        assert(ipod_touch_lcd_read(&s, 0x20, 4) == 0x00200700);
    }
    /* The shipped panel reads back what iBoot programmed; a panel= panel
     * reads its own size (what the kernel adopts), with or without lcd-planes. */
    for (unsigned option = 0; option < 2; option++) {
        IPodTouchLCDState native = { .planes_enabled = option, .pw = 320, .ph = 480 };
        IPodTouchLCDState tall = { .planes_enabled = option, .pw = 320, .ph = 504 };
        IPodTouchLCDState *both[] = { &native, &tall };
        for (unsigned i = 0; i < 2; i++) {
            ipod_touch_lcd_write(both[i], 0x28, 320, 4);
            ipod_touch_lcd_write(both[i], 0x30, 0x014001e0, 4);
        }
        assert(ipod_touch_lcd_read(&native, 0x28, 4) == 320);
        assert(ipod_touch_lcd_read(&native, 0x30, 4) == 0x014001e0);
        assert(ipod_touch_lcd_read(&tall, 0x28, 4) == 320);
        assert(ipod_touch_lcd_read(&tall, 0x30, 4) == 0x014001f8);
    }
    IPodTouchLCDState n45 = { .s5l8900 = true, .pw = 320, .ph = 480 };
    ipod_touch_lcd_write(&n45, 0x40, 0x12345678, 4);
    assert(ipod_touch_lcd_read(&n45, 0x40, 4) == 0x12345678);
    ipod_touch_lcd_write(&n45, 0x5c, 0x700, 4);
    ipod_touch_lcd_write(&n45, 0x60, 0x08500000, 4);
    ipod_touch_lcd_write(&n45, 0x64, 0x014001e0, 4);
    ipod_touch_lcd_write(&n45, 0x68, 320, 4);
    assert(ipod_touch_lcd_read(&n45, 0x5c, 4) == 0x700);
    assert(ipod_touch_lcd_read(&n45, 0x60, 4) == 0x08500000);
    assert(ipod_touch_lcd_read(&n45, 0x64, 4) == 0x014001e0);
    assert(ipod_touch_lcd_read(&n45, 0x68, 4) == 320);
    /* N45 panel=: iBoot's window-2 size/span and window 1's read as the panel's; other values as written. */
    IPodTouchLCDState n45p = { .s5l8900 = true, .pw = 384, .ph = 448 };
    ipod_touch_lcd_write(&n45p, 0x7c, 0x014001e0, 4);
    ipod_touch_lcd_write(&n45p, 0x80, 320, 4);
    ipod_touch_lcd_write(&n45p, 0x64, 0x014001e0, 4);
    ipod_touch_lcd_write(&n45p, 0x68, 320, 4);
    assert(ipod_touch_lcd_read(&n45p, 0x7c, 4) == (384u << 16 | 448));
    assert(ipod_touch_lcd_read(&n45p, 0x80, 4) == 384);
    assert(ipod_touch_lcd_read(&n45p, 0x64, 4) == (384u << 16 | 448));
    assert(ipod_touch_lcd_read(&n45p, 0x68, 4) == 384);
    ipod_touch_lcd_write(&n45p, 0x7c, 0x00400040, 4);
    assert(ipod_touch_lcd_read(&n45p, 0x7c, 4) == 0x00400040);
    ipod_touch_lcd_write(&n45, 0x7c, 0x014001e0, 4);
    assert(ipod_touch_lcd_read(&n45, 0x7c, 4) == 0x014001e0);
    puts("PASS: actual LCD RGB1 readback/RMW, option boundary, N45 map and panel size (both boards)");
}
'''
code = header + '\n'.join(function(name) for name in (
    'lcd_panel_is_native', 'lcd_s5l8900_offset', 'ipod_touch_lcd_read', 'ipod_touch_lcd_write')) + checks
with tempfile.TemporaryDirectory(prefix='lcd-readback-') as directory:
    path = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    path.write_text(code)
    subprocess.run(['clang', '-g', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(path), '-o', str(binary)],
                   check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=20)
