#!/usr/bin/env python3
"""A short peripheral packet must retain its untransferred DMA residue."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'hw/dma/pl080.c').read_text()
start = source.index('void pl080_set_dma_last_request(')
end = source.index('\n}', start) + 2
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>
#define PL080_CCONF_H (1<<18)
#define PL080_CCONF_E 1
#define PL080_CCTRL_I (1u<<31)
#define MEMTXATTRS_UNSPECIFIED 0
#define QEMU_CLOCK_VIRTUAL 0
static int64_t qemu_clock_get_ns(int clock) { return 0; }
typedef struct { uint32_t src,dest,lli,ctrl,conf; } pl080_channel;
typedef struct { int nchannels,trace_id,downstream_as; uint32_t tc_int; pl080_channel chan[2]; } PL080State;
static int updates;
static void pl080_run(PL080State *s) { /* Seven bytes already transferred. */ }
static void pl080_update(PL080State *s) { updates++; }
static bool it_dmac_trace_on(void) { return false; }
static uint32_t address_space_ldl_le(int *as,uint32_t addr,int attr,void *result) { assert(0); return 0; }
''' + source[start:end] + r'''
int main(void) {
 PL080State s = {.nchannels=2};
 s.chan[0] = (pl080_channel){.ctrl=PL080_CCTRL_I|2041, .conf=1|(2<<11)|(3<<1)};
 s.chan[1] = (pl080_channel){.ctrl=PL080_CCTRL_I|2048, .conf=1|(2<<11)|(4<<1)};
 pl080_set_dma_last_request(&s,3);
 assert((s.chan[0].ctrl&0xfff)==2041);
 assert(!(s.chan[0].conf&1));
 assert(s.tc_int==1 && updates==1);
 assert((s.chan[1].ctrl&0xfff)==2048 && (s.chan[1].conf&1));
 pl080_set_dma_last_request(&s,3);
 assert(s.tc_int==1 && (s.chan[0].ctrl&0xfff)==2041);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'check.c'
    path.write_text(code)
    binary = Path(directory) / 'check'
    subprocess.run(['clang', '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: short DMA packet preserves residue and completes only its channel')
