#!/usr/bin/env python3
"""Actual LCD control MMIO: stock rotation RMW, option independence and N45 map."""
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
} IPodTouchLCDState;
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
        IPodTouchLCDState s = { .planes_enabled = option };
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
    IPodTouchLCDState n45 = { .s5l8900 = true };
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
    puts("PASS: actual LCD RGB1 readback/RMW, option boundary and N45 map");
}
'''
code = header + '\n'.join(function(name) for name in (
    'lcd_s5l8900_offset', 'ipod_touch_lcd_read', 'ipod_touch_lcd_write')) + checks
with tempfile.TemporaryDirectory(prefix='lcd-readback-') as directory:
    path = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    path.write_text(code)
    subprocess.run(['clang', '-g', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(path), '-o', str(binary)],
                   check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=20)
