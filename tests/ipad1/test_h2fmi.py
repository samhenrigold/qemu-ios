#!/usr/bin/env python3
"""S5L8930 H2FMI (hw/arm/s5l8930_h2fmi.c): the read transfer-start rule, O(1)
FIFO reads, and a write that waits for its page's data AND metadata.

Recent iPad fixes this pins (driven through the MMIO handlers, IOP page store
stubbed, no emulator):
  - A read transfer starts only on entering read mode (control bits 0-1 -> 3)
    or, in read mode, raising bit 7; a repeated write of 3 is not a new page
    (the FPart phantom-transfer fix).
  - FIFO reads pop from an advancing offset (O(1)), returning page bytes in
    order without re-shifting the buffer.
  - A page program completes only once the FIFOs hold the page's data AND its
    metadata (the 3.2.2 IOP-panic fix).

Mutation (named, must fail this test): in h2fmi_write's FMI_CONTROL case,
weaken the transfer-start guard
    if ((v & 3) == 3 && ((prev & 3) != 3 || ((v & 0x80) && !(prev & 0x80)))) {
to
    if ((v & 3) == 3) {
so every write of 3 re-transfers the page (the phantom transfer).
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/s5l8930_h2fmi.c').read_text()
constants = '\n'.join(re.findall(r'^#define (?:FMI_|FMC_|ECC_|NAND_|META_|H2FMI_)\w+\s+[^\n]*$', source, re.M))
enums = re.search(r'typedef enum \{[^}]*\} H2FMIMode;', source, re.S).group()
bus_struct = re.search(r'typedef struct H2FMIBus \{.*?\} H2FMIBus;', source, re.S).group()
top_struct = re.search(r'struct S5L8930H2FMIState \{.*?\n\};', source, re.S).group()

order = ('h2fmi_status', 'h2fmi_update_irq', 'h2fmi_ce', 'h2fmi_fmc_events',
         'h2fmi_command', 'h2fmi_go', 'fifo_compact', 'h2fmi_raw_read', 'h2fmi_read_bytes', 'h2fmi_room',
         'h2fmi_transfer', 'h2fmi_meta_per_page', 'h2fmi_write_check',
         'h2fmi_drain', 'fifo_pop', 'h2fmi_ecc_sector', 'h2fmi_read', 'h2fmi_write')
funcs = []
for name in order:
    m = re.search(r'^static [^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert m, name
    funcs.append(m.group())

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t hwaddr;
typedef int SysBusDevice, MemoryRegion, qemu_irq;
typedef struct DeviceState DeviceState;
typedef struct S5L8930H2FMIState S5L8930H2FMIState;
#define S5L8930_H2FMI_BASE 0x81200000u
#define LOG_UNIMP 1
#define LOG_GUEST_ERROR 2
#define HWADDR_PRIx "llx"
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define qemu_log_mask(...) ((void)0)
#define qemu_log(...) ((void)0)
#define HT(...) ((void)0)
static void qemu_set_irq(int irq, int v) { (void)irq; (void)v; }
static int ctz32(uint32_t v) { int n = 0; if (!v) return 32; while (!(v & 1)) { v >>= 1; n++; } return n; }
static int ctpop8(uint8_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
static void stl_le_p(void *p, uint32_t v) {
    uint8_t *b = p; b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}
/* IOP page-store stub: a 16-byte page (bytes 0,1,2,...) plus 10 meta bytes. */
static uint32_t g_page_bytes = 16, g_id = 0xa14d4b48;
static uint8_t g_ce_mask = 0x01;
static struct { int called, bus; uint32_t ce, page, len; uint8_t data[64], meta[16]; } g_prog;
void s5l8930_iop_nand_info(DeviceState *d, uint32_t *id, uint8_t *m, uint32_t *pb) {
    (void)d; *id = g_id; *m = g_ce_mask; *pb = g_page_bytes;
}
bool s5l8930_iop_nand_read(DeviceState *d, int bus, uint32_t ce, uint32_t page,
                           uint8_t *buf, uint32_t *stride) {
    (void)d; (void)bus; (void)ce; (void)page;
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    *stride = g_page_bytes + 16;
    return true;
}
uint32_t s5l8930_iop_nand_program(DeviceState *d, int bus, uint32_t ce, uint32_t page,
                                  const uint8_t *data, uint32_t len, const uint8_t *meta) {
    (void)d; g_prog.called++; g_prog.bus = bus; g_prog.ce = ce; g_prog.page = page;
    g_prog.len = len; memcpy(g_prog.data, data, MIN(len, 64u));
    if (meta) memcpy(g_prog.meta, meta, 10); return 1;
}
uint32_t s5l8930_iop_nand_erase(DeviceState *d, int bus, uint32_t ce, uint32_t page) {
    (void)d; (void)bus; (void)ce; (void)page; return 1;
}
void s5l8930_cdma_kick(DeviceState *d) { (void)d; }
void s5l8930_cdma_sink_done(DeviceState *d, uint32_t base, uint32_t size) { (void)d; (void)base; (void)size; }
'''

tests = r'''
struct DeviceState { int unused; };
#define RD(a)   h2fmi_read(bp, (a), 4)
#define WR(a,v) h2fmi_write(bp, (a), (v), 4)
static void load_page(H2FMIBus *b, uint32_t row) {
    h2fmi_write(b, FMC_BASE + FMC_CE, 0x1, 4);         /* CE0 */
    h2fmi_write(b, FMC_BASE + FMC_CMD, 0x3000, 4);     /* read: 0x00 then 0x30 */
    h2fmi_write(b, FMC_BASE + FMC_ADDR0, row << 16, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR1, 0, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0xb, 4);         /* cmd1 | addr | cmd2 */
}
int main(void) {
    DeviceState iop = {0};
    /* The model's buffers are multi-MB; keep the buses off the stack. */
    S5L8930H2FMIState *sp = calloc(1, sizeof *sp);
    H2FMIBus *bp = calloc(1, sizeof *bp);
    sp->iop = &iop;
    bp->s = sp; bp->n = 0;

    /* --- transfer-start rule --- */
    load_page(bp, 0);
    assert(bp->data_len == 0);
    WR(FMI_CONTROL, 3);                      /* enter read mode -> one transfer */
    assert(bp->data_len == g_page_bytes);
    WR(FMI_CONTROL, 3);                      /* still mode 3, no bit 7: NOT a new page */
    assert(bp->data_len == g_page_bytes);    /* no phantom transfer */
    WR(FMI_CONTROL, 3);                      /* and again */
    assert(bp->data_len == g_page_bytes);

    /* Pipelined next page: raise bit 7 -> a real second transfer. */
    load_page(bp, 1);
    WR(FMI_CONTROL, 0x83);
    assert(bp->data_len == 2 * g_page_bytes);

    /* --- O(1) FIFO reads: bytes come back in order, offset advancing --- */
    assert((RD(FMI_LEVEL) & 0x18));          /* data ready */
    assert(RD(FMI_DATA) == 0x03020100);      /* page bytes 0..3 */
    assert(RD(FMI_DATA) == 0x07060504);      /* next word, not the same one */
    assert(bp->data_len == 2 * g_page_bytes - 8 && bp->data_off == 8);

    /* --- write waits for BOTH data and metadata --- */
    S5L8930H2FMIState *wsp = calloc(1, sizeof *wsp);
    H2FMIBus *wp = calloc(1, sizeof *wp);
    wsp->iop = &iop; wp->s = wsp; wp->n = 0;
    h2fmi_write(wp, FMC_BASE + FMC_CE, 0x1, 4);
    h2fmi_write(wp, FMC_BASE + FMC_CMD, 0x80, 4);       /* page program */
    h2fmi_write(wp, FMC_BASE + FMC_GO, 0x9, 4);         /* cmd1 | addr */
    h2fmi_write(wp, FMI_CONTROL, 5, 4);                 /* write transfer */
    for (unsigned i = 0; i < g_page_bytes; i += 4) {    /* all the data... */
        h2fmi_write(wp, FMI_DATA, 0xa0a1a2a3, 4);
    }
    assert(!(wp->fmi[FMI_STATUS / 4] & (1u << 1)));      /* ...but no DONE without meta */
    assert(wp->wpage_len[0] == 0 && g_prog.called == 0);
    for (unsigned i = 0; i < 10; i += 2) {              /* now the 10 meta bytes */
        h2fmi_write(wp, FMI_META, 0xbbbb, 4);
    }
    assert(wp->fmi[FMI_STATUS / 4] & (1u << 1));         /* DONE once the page is whole */
    assert(wp->wpage_len[0] == g_page_bytes);
    h2fmi_write(wp, FMC_BASE + FMC_CMD, 0x10, 4);        /* program confirm */
    h2fmi_write(wp, FMC_BASE + FMC_GO, 0x1, 4);
    assert(g_prog.called == 1 && g_prog.len == g_page_bytes);
    assert(g_prog.data[0] == 0xa3 && g_prog.data[1] == 0xa2);

    free(sp); free(bp); free(wsp); free(wp);
    puts("PASS: H2FMI transfer-start rule, O(1) FIFO reads in order, write waits for data + metadata");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + constants + '\n' + enums + '\n' + bus_struct + '\n' +
                 top_struct + '\n' + '\n'.join(funcs) + tests)
    binary = str(Path(tmp) / 'check')
    subprocess.run(['clang', '-std=gnu11', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
