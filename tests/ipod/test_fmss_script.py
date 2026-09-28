#!/usr/bin/env python3
"""Run the real READ ID programs of 7E18 and 8C148 iBoot through fmss_run_script."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_fmss.c').read_text()
header = (root / 'include/hw/arm/ipod_touch_fmss.h').read_text()
functions = []
for name in ('fmss_var_read', 'fmss_run_script'):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert match, name
    functions.append(match.group())
defines = '\n'.join(l for l in source.splitlines() if l.startswith('#define FMSS_CHIP') or l.startswith('#define FMSS_SCRIPT'))
constants = '\n'.join(l for l in header.splitlines() if l.startswith('#define FMSS'))

# iBoot-636.66.33 (7E18) at 0x25330 and iBoot-931.71.16 (8C148) at 0x25a60.
ID_7E18 = """01000c0c 000000ff 01000c10 00000001 01000c58 00000004 01000c4c 00000128
01000004 033338e0 01000048 0ff00ffe 04070d08 ffffffff 04060d0c 0000000f
05050000 00000002 0b000005 00077001 02000000 00000000 01000008 00000090
07000000 00000000 01000048 00000002 0100002c 00000000 0100000c 00000000
01000004 00000001 07010000 00000000 01000048 00000004 01000030 00000004
01000014 00000010 01000004 000000e2 07030000 00000000 01000048 00000008
01000040 00000052 04040040 00000002 0e040000 000000c8 04000060 ffffffff
11000007 00000000 0d060006 00000001 17060000 00000110 0c070007 00000004
13050005 00000001 0e060000 00000048 04000000 fffffe01 02000000 00000000
00000000 00000000"""
ID_8C148 = """01000c0c 000000ff 01000c10 00000001 01000c58 00000004 01000c4c 00000140
01000004 033338e0 01000048 0ff00ffe 04070d08 ffffffff 04060d0c 0000000f
05050000 00000002 0b000005 00077001 02000000 00000000 01000008 00000090
07000000 00000000 01000048 00000002 0100002c 00000000 0100000c 00000000
01000004 00000001 07010000 00000000 01000048 00000004 01000030 00000007
01000014 00000010 01000004 000000e2 07030000 00000000 01000048 00000008
01000040 00000082 04040040 00000002 0e040000 000000c8 04000060 ffffffff
11000007 00000000 04000064 ffffffff 0c070007 00000004 11000007 00000000
0d060006 00000001 17060000 00000128 0c070007 00000004 13050005 00000001
0e060000 00000048 04000000 fffffe01 02000000 00000000 00000000 00000000"""

def words(text):
    return ','.join('0x' + w + 'u' for w in text.split())

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LOG_UNIMP 0
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
#define ctz32(x) ((x) ? (unsigned)__builtin_ctz(x) : 32u)
typedef struct {
    uint32_t reg_cs_script, reg_cinfo_target_addr, reg_pages_in_addr, reg_cs_buf_addr;
    uint32_t reg_page_spare_out_addr, reg_pages_out_addr, reg_csgenrc;
} IPodTouchFMSSState;
static uint8_t mem[0x10000];
static void cpu_physical_memory_read(uint32_t a, void *p, size_t n) { assert(a + n <= sizeof(mem)); memcpy(p, mem + a, n); }
static void cpu_physical_memory_write(uint32_t a, const void *p, size_t n) { assert(a + n <= sizeof(mem)); memcpy(mem + a, p, n); }
'''
tests = r'''
static const uint32_t id3[] = {%s}, id4[] = {%s};
static void run(const uint32_t *prog, size_t n) {
    IPodTouchFMSSState s = {.reg_cs_script = 0x1000, .reg_cinfo_target_addr = 0x8000,
                            .reg_pages_in_addr = 8};
    memset(mem, 0, sizeof(mem));
    memcpy(mem + 0x1000, prog, n);
    fmss_run_script(&s);
}
int main(void) {
    uint32_t *out = (uint32_t *)(mem + 0x8000);
    run(id3, sizeof(id3)); /* 4 bytes per CE, 8 CEs */
    for (int i = 0; i < 8; i++) assert(out[i] == (i < 4 ? 0xb614d5adu : 0));
    assert(out[8] == 0);
    run(id4, sizeof(id4)); /* 8 bytes per CE, 8 CEs */
    for (int i = 0; i < 8; i++) assert(out[2*i] == (i < 4 ? 0xb614d5adu : 0) && out[2*i+1] == 0);
    uint32_t bad[] = {0x05000000u, 7, 0x11000001u, 0, 0x99000000u, 0, 0x11000000u, 0, 0, 0};
    run(bad, sizeof(bad)); /* an unknown op stops the program */
    assert(mem[0] == 7 && mem[7] == 0);
    puts("PASS: FMSS READ ID programs of 7E18 and 8C148, unknown op stops");
}
''' % (words(ID_7E18), words(ID_8C148))
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + constants + '\n' + defines + '\n' + '\n'.join(functions) + tests)
    binary = str(Path(tmp) / 'check')
    subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined', '-g', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
