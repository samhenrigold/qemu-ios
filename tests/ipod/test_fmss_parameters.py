#!/usr/bin/env python3
"""Validate CPU-owned FMSS parameters through actual handlers and scripts.

FMC shadow observations prove register dataflow, not NAND commands or ECC.
Real serialization round trips are covered by the FMSS qtests.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path,
                    default=ROOT / "hw/arm/ipod_touch_fmss.c",
                    help="alternate actual source for a negative baseline")
source = parser.parse_args().source.read_text()
header = (ROOT / "include/hw/arm/ipod_touch_fmss.h").read_text()

pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <glib.h>
#define NAND_BYTES_PER_PAGE 4096
#define NAND_BYTES_PER_SPARE 64
#define error_report(...) ((void)0)
#define IPOD_TOUCH_FMSS(p) ((IPodTouchFMSSState *)(p))
typedef void DeviceState;
#define QEMU_CLOCK_VIRTUAL 0
#define LOG_UNIMP 0
#define qemu_log_mask(...) ((void)0)
#define IT_SIZE(tag,val,max) ((val) < (max) ? (val) : (max))
typedef uint64_t hwaddr;
typedef struct { int64_t deadline; bool pending; } QEMUTimer;
typedef struct {
    int irq;
    uint32_t reg_script_param_d34, reg_script_param_d38, reg_script_param_d48;
    uint32_t reg_cs_ctrl, reg_cs_irq_bit, reg_cs_irq_mask, reg_cs_script;
    uint32_t reg_cinfo_target_addr, reg_pages_in_addr, reg_cs_buf_addr;
    uint32_t reg_num_pages, reg_page_spare_out_addr, reg_pages_out_addr, reg_csgenrc, reg_script_param_d4c, reg_chunks_per_page, reg_script_csgenr15, reg_script_scratch_d7c, reg_script_scratch_d3c;
    GTree *phys_pages, *erased_blocks;
    GHashTable *overlay_pages;
    bool overlay_indexed;
    uint8_t *page_buffer, *page_spare_buffer;
    QEMUTimer *completion_timer;
} IPodTouchFMSSState;
static int level;
static gint key_compare(gconstpointer a, gconstpointer b) {
    return ((uintptr_t)a > (uintptr_t)b) - ((uintptr_t)a < (uintptr_t)b);
}
static bool fmss_physical(void) { return false; }
static void qemu_irq_lower(int irq) { level=0; }
static bool fmss_io_failed;
static int64_t now;
static void qemu_set_irq(int irq, int value) { level = !!value; }
static int64_t qemu_clock_get_ns(int clock) { return now; }
static void timer_mod(QEMUTimer *t, int64_t when) { t->deadline=when; t->pending=true; }
static void timer_del(QEMUTimer *t) { t->pending=false; }
static bool fmss_trace_on(void) { return false; }

static unsigned reads, writes;
static void read_nand_pages(IPodTouchFMSSState *s) { reads++; }
static void write_nand_pages(IPodTouchFMSSState *s) { writes++; }
static unsigned fmss_total_blocks(IPodTouchFMSSState *s) { return 2048; }

#define LOG_GUEST_ERROR 1
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
static int address_space_memory;
static uint8_t memory[65536];
static void cpu_physical_memory_read(uint32_t a, void *p, size_t n) {
    assert((uint64_t)a+n <= sizeof(memory)); memcpy(p,memory+a,n);
}
static void cpu_physical_memory_write(uint32_t a, const void *p, size_t n) {
    assert((uint64_t)a+n <= sizeof(memory)); memcpy(memory+a,p,n);
}
static int address_space_read(void *space, uint32_t a, int attrs, void *p, size_t n) {
    if ((uint64_t)a+n > sizeof(memory)) return 1;
    memcpy(p,memory+a,n); return MEMTX_OK;
}
static int address_space_write(void *space, uint32_t a, int attrs,
                               const void *p, size_t n) {
    if ((uint64_t)a + n > sizeof(memory)) return 1;
    memcpy(memory + a, p, n);
    return MEMTX_OK;
}
static void stl_le_p(void *p, uint32_t value) {
    uint8_t *bytes = p;
    for (unsigned i = 0; i < 4; i++) bytes[i] = value >> (8 * i);
}
static uint32_t ldl_le_p(const void *p) {
    const uint8_t *b=p; return b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;
}
static unsigned ctz32(uint32_t x) { return x?__builtin_ctz(x):32; }
'''

functions = []
for name in (
    "fmss_var_read", "fmss_run_script", "ipod_touch_fmss_read",
    "fmss_update_irq", "fmss_complete", "ipod_touch_fmss_write",
    "fmss_pre_load", "fmss_invalid_page", "fmss_invalid_marker",
    "fmss_post_load", "ipod_touch_fmss_reset",
):
    pattern = r"^static [^\n]*\b" + name + r"\([^)]*\)\s*\{.*?^}"
    match = re.search(pattern, source, re.M | re.S)
    assert match, name
    functions.append(match.group())
constants = "\n".join(
    line for line in header.splitlines()
    if line.startswith(("#define FMSS", "#define NAND_PAGES_PER_BLOCK")))
constants += "\n" + "\n".join(
    line for line in source.splitlines()
    if line.startswith(("#define FMSS_CHIP", "#define FMSS_SCRIPT")))

tests = r'''
static void put(IPodTouchFMSSState *s, unsigned reg, uint32_t val) {
    ipod_touch_fmss_write(s,reg,val,4);
}
int main(void) {
    QEMUTimer timer={0}; uint8_t page[4096], spare[64];
    IPodTouchFMSSState s={.completion_timer=&timer,.reg_cs_script=0x1000,
        .page_buffer=page,.page_spare_buffer=spare};
    s.phys_pages=g_tree_new(key_compare); s.erased_blocks=g_tree_new(key_compare);
    const uint32_t d34[]={0,0x16,0x80000000,0xffffffff};
    const uint32_t d48[]={0xffffffff,0x20011000,0,0x80000000};
    /* D34 uses the observed stock read/write pair. D48 readback uses
     * immediate OR801 and FMC shadow stores as an observation oracle;
     * this does not verify the stock zero-immediate register-OR form. */
    const uint32_t script[]={
        0x04000d34,0xffffffff,0x02000030,0,
        0x04010030,0xffffffff,0x05020000,0x2000,0x11010002,0,
        0x04000d48,0xffffffff,0x0b000000,0x801,0x02000000,0,
        0x04010000,0xffffffff,0x0c020002,4,0x11010002,0,0,0
    };
    uint32_t d3c_program[] = {
        0x05050000, 0, 0x02050d3c, 0,
        0x04010d3c, 0xffffffff, 0x05020000, 0x3900,
        0x11010002, 0, 0, 0
    };
    const uint32_t d3c_values[] = {0, 1, 0x40000000, 0x80000000, 0xffffffff};
    for (unsigned i = 0; i < 5; i++) {
        d3c_program[1] = d3c_values[i];
        memcpy(memory+0x1000, d3c_program, sizeof(d3c_program));
        fmss_run_script(&s);
        assert(ipod_touch_fmss_read(&s, 0xd3c, 4) == d3c_values[i]);
        assert(ldl_le_p(memory+0x3900) == d3c_values[i]);
    }
    /* Captured initializer is exactly immediate zero, not a status return. */
    d3c_program[2] = 0x01000d3c;
    memcpy(memory+0x1000, d3c_program, sizeof(d3c_program));
    fmss_run_script(&s);
    assert(!s.reg_script_scratch_d3c && !ldl_le_p(memory+0x3900));
    for (unsigned form = 0; form < 3; form++) {
        s.reg_script_scratch_d3c = 0x89abcdef;
        d3c_program[2] = form == 2 ? 0x01050d3c : (form ? 0x02050d3c : 0x01000d3c);
        d3c_program[3] = form == 2 ? 0 : (form ? 1 : 0xcafebabe);
        memcpy(memory+0x1000, d3c_program, sizeof(d3c_program));
        const uint8_t sentinel[] = {0xba, 0xdc, 0xcd, 0xab};
        memcpy(memory+0x3900, sentinel, sizeof(sentinel));
        fmss_run_script(&s);
        assert(s.reg_script_scratch_d3c == 0x89abcdef);
        assert(ldl_le_p(memory+0x3900) == 0xabcddcba);
    }
    const uint32_t scratch_values[] = { 0, 1, 0x001f0001, 0x80000000, 0xffffffff };
    uint32_t scratch_script[] = {
        0x05040000, 0, 0x02040d7c, 0,
        0x04010d7c, 0xffffffff, 0x05020000, 0x3800,
        0x11010002, 0, 0, 0
    };
    for (unsigned i = 0; i < 5; i++) {
        scratch_script[1] = scratch_values[i];
        memcpy(memory+0x1000, scratch_script, sizeof(scratch_script));
        fmss_run_script(&s);
        assert(ipod_touch_fmss_read(&s, 0xd7c, 4) == scratch_values[i]);
        assert(ldl_le_p(memory+0x3800) == scratch_values[i]);
    }
    /* Stock status initializer uses opcode01: the immediate, not r[a]. */
    const uint32_t values[]={0,0xcafebabe,0x80000000,0xffffffff};
    uint32_t init[]={0x05000000,0x12345678,0x01000d7c,0,
        0x04010d7c,0xffffffff,0x05020000,0x3800,0x11010002,0,0,0};
    for(unsigned i=0;i<4;i++) {
        init[3]=values[i];memcpy(memory+0x1000,init,sizeof(init));
        fmss_run_script(&s);
        assert(s.reg_script_scratch_d7c==values[i]);
        assert(ipod_touch_fmss_read(&s,0xd7c,4)==values[i]);
        assert(ldl_le_p(memory+0x3800)==values[i]);
    }
    /* Unmeasured opcode02 nonzero immediate still stops before later store. */
    init[2]=0x02000d7c;init[3]=1;memcpy(memory+0x1000,init,sizeof(init));
    uint32_t sentinel=0xabcddcba;memcpy(memory+0x3800,&sentinel,4);
    fmss_run_script(&s);
    assert(s.reg_script_scratch_d7c==0xffffffff);
    assert(ldl_le_p(memory+0x3800)==sentinel);
    const uint32_t d38_script[] = {
        0x04000d38, 0xffffffff, 0x02000030, 0,
        0x04010030, 0xffffffff, 0x05020000, 0x3000, 0x11010002, 0, 0, 0
    };
    const uint32_t d38[] = { 0, 12, 25, 0x80000000, 0xffffffff };
    memcpy(memory+0x1000, d38_script, sizeof(d38_script));
    for (unsigned i=0; i<5; i++) {
        put(&s, 0xd38, d38[i]);
        assert(ipod_touch_fmss_read(&s, 0xd38, 4)==d38[i]);
        fmss_run_script(&s);
        assert(ldl_le_p(memory+0x3000)==d38[i]);
        assert(!reads && !writes); /* selector0: scalar write only */
    }
    put(&s, 0xd30, 0xa01); put(&s, 0xd38, 0);
    assert(reads==1 && !writes && !s.reg_script_param_d38);
    put(&s, 0xd30, 0xa02); put(&s, 0xd38, 0x80000000);
    assert(reads==1 && writes==1 && s.reg_script_param_d38==0x80000000);
    put(&s, 0xd30, 0);
    memcpy(memory+0x1000,script,sizeof(script));
    for (unsigned i=0;i<4;i++) {
        put(&s,0xd34,d34[i]); put(&s,0xd48,d48[i]); put(&s,0xd4c,0x12345678);
        assert(ipod_touch_fmss_read(&s,0xd34,4)==d34[i]);
        assert(ipod_touch_fmss_read(&s,0xd48,4)==d48[i]);
        assert(ipod_touch_fmss_read(&s,0xd4c,4)==0x12345678);
        fmss_run_script(&s);
        assert(ldl_le_p(memory+0x2000)==d34[i]);
        assert(ldl_le_p(memory+0x2004)==(d48[i]|0x801));
    }
    ipod_touch_fmss_reset((DeviceState *)&s);
    assert(!ipod_touch_fmss_read(&s,0xd3c,4));
    assert(!ipod_touch_fmss_read(&s,0xd7c,4));
    assert(!ipod_touch_fmss_read(&s,0xd38,4));
    assert(!ipod_touch_fmss_read(&s,0xd34,4) && !ipod_touch_fmss_read(&s,0xd48,4));
    put(&s,0xd34,0x16); put(&s,0xd48,0x20011000);
    s.reg_script_scratch_d3c=0x80000000;
    s.reg_script_scratch_d7c=0x80000000;
    s.reg_script_param_d38=0xffffffff;
    fmss_pre_load(&s);
    assert(!s.reg_script_param_d38 && !s.reg_script_scratch_d7c && !s.reg_script_scratch_d3c);
    assert(!s.reg_script_param_d34 && !s.reg_script_param_d48);
    assert(fmss_post_load(&s,7)==0); /* missingfields stayzero, no inventedlegacyvalue */
    assert(!s.reg_script_param_d34 && !s.reg_script_param_d48);
    /* Field restoration is simulated here; real VMState roundtrip is qtest. */
    s.reg_script_param_d34=0xffffffff; s.reg_script_param_d48=0x80000000;
    assert(fmss_post_load(&s,8)==0);
    assert(ipod_touch_fmss_read(&s,0xd34,4)==0xffffffff &&
           ipod_touch_fmss_read(&s,0xd48,4)==0x80000000);
    g_tree_destroy(s.phys_pages);g_tree_destroy(s.erased_blocks);
    puts("PASS actual D3C initializer/latch and rejected forms, D7C scratch, D38 latch, CPU shortcut dispatch, D34/D48 CPUlatches, independentD4C, FMC shadow observations, reset and versionedload bookkeeping");
}
'''

with tempfile.TemporaryDirectory(prefix="fmss-parameters-") as temporary:
    directory = Path(temporary)
    c_file = directory / "check.c"
    binary = directory / "check"
    c_file.write_text(pre + constants + "\n" + "\n".join(functions) + tests)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    subprocess.run([
        "clang", *flags, "-std=c11", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", str(c_file), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
