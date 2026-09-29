#!/usr/bin/env python3
"""IOP HLE NAND page store (hw/arm/s5l8930_iop.c): the raw-flash contract the
H2FMI model and the IOP ring both read through.

The mailbox/ring/doorbell and the real second core (s5l8930_iop_core.c) are
boot-only -- they need guest DRAM, address_space_memory and, for the core, an
ARM CPU -- so they have no host-slice seam (documented, not faked). The one
clean pure-function seam in the IOP is the page store behind
s5l8930_iop_nand_{info,read,program,erase}; those are exactly the calls the
H2FMI model makes so iBoot's direct NAND path and the kernel's IOP path see the
same flash. This pins the round-trip, blank semantics, erase, and the
page_bytes = 0 signal a blank chip gives (which H2FMI branches on). No emulator.

Mutation (named, must fail this test): in nand_program_page, drop the store
    memcpy(p, data, n);
so a programmed page reads back blank instead of its data.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/s5l8930_iop.c').read_text()
state = re.search(r'struct S5L8930IOPState \{.*?\n\};', source, re.S).group()

order = ('nand_addr_bad', 'nand_dirty', 'nand_set_dirty', 'nand_touch',
         'nand_page', 'nand_read_page', 'nand_program_page', 'nand_erase_block',
         's5l8930_iop_nand_read', 's5l8930_iop_nand_program',
         's5l8930_iop_nand_erase', 's5l8930_iop_nand_info')
funcs = []
for name in order:
    m = re.search(r'^(?:static |inline |bool |void |int |uint\w+ )[^\n]*\b' + name +
                  r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
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
typedef int SysBusDevice, MemoryRegion;
typedef struct DeviceState DeviceState;
typedef struct QEMUTimer QEMUTimer;
typedef struct S5L8930IOPState S5L8930IOPState;
#define S5L8930_IOP(s) ((S5L8930IOPState *)(s))
#define NAND_BUSES 2
#define NAND_CES 8
#define IOP_VIC_COUNT 4
#define IOP_VIC_REGS 0x1000
#define IOP_MAX_ENDPOINTS 8
#define FMI_STATUS_OK 1
#define FMI_STATUS_BLANK 2
#define FMI_STATUS_PARAM 0x80000004
#define FMI_META_BYTES 10
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define DPRINTF(...) ((void)0)
#define qemu_log_mask(...) ((void)0)
#define LOG_GUEST_ERROR 1
#define futimens(a,b) 0
'''

tests = r'''
#define STRIDE 64
#define PAGEB 48
#define PPB 8
#define PPCE 64
static S5L8930IOPState *make_chip(void) {
    S5L8930IOPState *s = calloc(1, sizeof *s);
    static char dir[] = "store";
    s->nand_dir = dir;
    s->nand_id = 0xa14d4b48;
    s->nand_ce_mask = 0x01;         /* only CE0 populated */
    s->page_stride = STRIDE;
    s->store_page_bytes = PAGEB;
    s->store_ppb = PPB;
    s->pages_per_ce = PPCE;
    for (int b = 0; b < NAND_BUSES; b++) {
        s->bytes_per_page[b] = PAGEB;
        s->bytes_per_spare[b] = STRIDE - PAGEB;
        s->chip[b][0] = calloc(PPCE, STRIDE);
        s->chip_fd[b][0] = -1;
    }
    return s;
}
int main(void) {
    S5L8930IOPState *s = make_chip();
    DeviceState *dev = (DeviceState *)s;
    uint8_t data[STRIDE], meta[FMI_META_BYTES], buf[128], rmeta[FMI_META_BYTES];
    uint32_t stride;

    for (int i = 0; i < STRIDE; i++) data[i] = i + 1;
    memset(meta, 0xab, sizeof meta);

    /* --- nand_info: id, ce mask, and page_bytes for a real chip --- */
    uint32_t id, pb; uint8_t cm;
    s5l8930_iop_nand_info(dev, &id, &cm, &pb);
    assert(id == 0xa14d4b48 && cm == 0x01 && pb == PAGEB);

    /* --- program / read round-trip (page 5) --- */
    assert(s5l8930_iop_nand_program(dev, 0, 0, 5, data, STRIDE, meta) == FMI_STATUS_OK);
    assert(s5l8930_iop_nand_read(dev, 0, 0, 5, buf, &stride));
    assert(stride == STRIDE);
    assert(buf[0] == 1 && buf[1] == 2);           /* the data we stored */
    assert(buf[PAGEB] == 0xab);                   /* meta leads the spare */
    assert(nand_read_page(s, 0, 0, 5, buf, rmeta) == FMI_STATUS_OK);

    /* --- an unprogrammed page is blank, not stale --- */
    assert(nand_read_page(s, 0, 0, 10, buf, rmeta) == FMI_STATUS_BLANK);
    assert(!s5l8930_iop_nand_read(dev, 0, 0, 10, buf, &stride));

    /* --- erase returns a programmed block to blank --- */
    assert(s5l8930_iop_nand_program(dev, 0, 0, 3, data, STRIDE, meta) == FMI_STATUS_OK);
    assert(s5l8930_iop_nand_read(dev, 0, 0, 3, buf, &stride));
    assert(s5l8930_iop_nand_erase(dev, 0, 0, 3) == FMI_STATUS_OK);   /* block 0 (pages 0-7) */
    assert(!s5l8930_iop_nand_read(dev, 0, 0, 3, buf, &stride));      /* blank again */

    /* --- addresses off the part are rejected, no OOB (ASan) --- */
    assert(s5l8930_iop_nand_program(dev, 0, 1, 0, data, STRIDE, meta) == FMI_STATUS_PARAM); /* CE1 not populated */
    assert(nand_read_page(s, 0, 0, 999, buf, rmeta) == FMI_STATUS_PARAM);                   /* page past the CE */

    /* --- a blank chip (no store dir) reports page_bytes 0: H2FMI keys on this --- */
    S5L8930IOPState blank = {0};
    blank.nand_id = 0x1234; blank.nand_ce_mask = 0x03;
    blank.store_page_bytes = PAGEB;             /* set, but ignored while nand_dir is NULL */
    s5l8930_iop_nand_info((DeviceState *)&blank, &id, &cm, &pb);
    assert(pb == 0 && cm == 0x03);

    for (int b = 0; b < NAND_BUSES; b++) free(s->chip[b][0]);
    free(s);
    puts("PASS: IOP NAND page store round-trip, blank/erase semantics, bounds, blank-chip page_bytes=0");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'check.c'
    c.write_text(prelude + state + '\n' + '\n'.join(funcs) + tests)
    binary = str(Path(tmp) / 'check')
    subprocess.run(['clang', '-std=gnu11', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', str(c), '-o', binary], check=True)
    subprocess.run([binary], check=True)
